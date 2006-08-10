/*?? TODO:
  - add functions:
	- CM_PointCluster(CVec3&) -- PointLeafnum()+LeafCluster()
	- CM_BoxClusters(CBox&, dst) -- BoxLeafnums()+LeafCluster()+compress list (no duplicated clusters) -- used few times in server
	- CM_BoxPVS(CBox&, dst) -- have SV_FatPVS() func; may be useful for renderer too
	? CM_InPVS(pos1, pos2) -- have func in sv_game.cpp (uses PVS + areas)
  - can create abstract CMapLoader with "virtual Load()" - it will check map format and load when supported
    (similar to CFileContainerArc ?)
  ! most Load*() functions have arguments, which can be used directly from bspfile struc
*/


#include "qcommon.h"
#include "cmodel.h"
#include "protocol.h"					// for MATERIAL_* consts
#include "MapBrush.h"


#if TRACE_DEBUG

#include "../client/ref.h"
bool cm_showTrace;

#define TRACE(m,c)		if (cm_showTrace) RE_DrawTextLeft(m,c)

#else
#define TRACE(m,c)
#endif

// can use also:
//#define TRACE(m,c)	DebugPrintf(m);DebugPrintf("\n");


// requirements for InitBoxHull()
#define HULL_NODES			6
#define HULL_LEAFS			1
#define HULL_BRUSHES		1
#define HULL_BRUSH_SIDES	6


struct cbrushside_t
{
	CPlane	*plane;
	csurface_t *surface;
};

//?? we can use CBrush instead of cbrush_t:
//?? 1. on q1/hl maps: we should build them anyway
//?? 2. on q2 maps: there is not too much space required for brush allocation (brush number is not too large)
//?? 3. on q3 maps: should load brushes
//?? 4. we cannot use dBsp2Brush_t instead of cbrush_t: needed "traceFrame" field
struct cbrush_t
{
	unsigned contents;
	int		numsides;
	int		firstbrushside;
	int		traceFrame;					// to avoid repeated testings
};

struct carea_t
{
	int		numAreaPortals;
	int		firstAreaPortal;
	int		floodnum;					// if two areas have equal floodnums, they are connected
	int		floodvalid;
};

struct surfBounds_t						// used for texture trace in Q1/HL maps
{
	float	mins[2];
	float	maxs[2];
};

char	map_name[MAX_QPATH];
static CMemoryChain *dataChain;
static bool isMapLoaded;
bool	map_clientLoaded;

int		c_traces, c_pointcontents;		// stats


//!!! REMOVE !!!
static int		numBrushSides;
static cbrushside_t *map_brushSides;

static surfBounds_t *map_faceBounds;	// [numFaces]; for Q1/HL maps trace

static int		numBrushes;
static cbrush_t	*map_brushes;

static int		numAreas;
static carea_t	*map_areas;

static int		numareaportals;
static dAreaPortal_t *map_areaportals;

static csurface_t nullsurface;

static int		floodvalid;

static bool portalopen[MAX_MAP_AREAPORTALS];


static cvar_t	*map_noareas;

// forwards
static void InitBoxHull ();
static void FloodAreaConnections ();



/*-----------------------------------------------------------------------------
	Material by texture
-----------------------------------------------------------------------------*/


class surfMaterial_t : public CStringItem
{
public:
	int		material;
};


static CMemoryChain *surfMaterialChain;
static TList<surfMaterial_t> surfMaterialList;


static void ReadSurfMaterials (const char *filename)
{
	if (surfMaterialChain)
	{
		delete surfMaterialChain;
		surfMaterialChain = NULL;
		surfMaterialList.Reset();
	}

	char *file;
	if (!(file = (char*) GFileSystem->LoadFile (filename)))
	{
		Com_DPrintf ("ReadSurfMaterials: %s is not found\n", filename);
		return;
	}

	surfMaterialChain = new CMemoryChain;
	surfMaterial_t *prev = NULL;
	const char *in = file;

	const char *s;
	while (s = COM_Parse (in), in)
	{
		int m;

		// s points to material name string
		switch (toLower (s[0]))
		{
#define T(c,mat) case c: m = MATERIAL_##mat; break;
		T('m',METAL);
		T('t',TILE);
		T('y',GLASS);
		T('l',GRAVEL);
		T('d',DIRT);
		T('c',CONCRETE);
		T('w',WOOD);
		T('n',SNOW);
		T('z',SILENT);
#undef T
		default:
			appWPrintf ("Unknown material mark \"%s\" in %s\n", s, filename);
			m = MATERIAL_CONCRETE;
		}

		s = COM_Parse (in);
		if (!in)
		{
			appWPrintf ("Unexpected end of file %s\n", filename);
			break;
		}
		// s points to surface name
		surfMaterial_t *sm = new (s, surfMaterialChain) surfMaterial_t;
		sm->material = m;
		// add to list
		surfMaterialList.InsertAfter (sm, prev);

		prev = sm;
//		Com_DPrintf ("Added surface material: %d for %s\n", m, s);
	}

	delete file;
}

// name must be without "textures/" prefix and without extension
static int GetSurfMaterial (const char *name)
{
	const char *checkname = strrchr (name, '/');		// points to a name without path
	if (checkname)
		checkname++;
	else
		checkname = name;

	for (TListIterator<surfMaterial_t> sm = surfMaterialList; sm; ++sm)
	{
		const char *s = sm->name;
		if (strchr (s, '/'))
		{	// mask have a path separator - compare full names
			if (!appMatchWildcard (name, sm->name, true))
				continue;
		}
		else
		{	// compare without path
			if (!appMatchWildcard (checkname, sm->name, true))
				continue;
		}

//		Com_DPrintf ("set material %d for %s\n", sm->material, name);
		return sm->material;
	}
//	Com_DPrintf ("set default material for %s\n", name);
	return MATERIAL_CONCRETE;	// default material
}


/*-----------------------------------------------------------------------------
	common bsp loading code
-----------------------------------------------------------------------------*/

// T is dBsp[123]Node_t
// Should be called after LoadLeafs()

static void SetChild (CBspNode *node, int index, int nodeIndex)
{
	// setup child node
	if (nodeIndex >= 0)
		node->children[index] = bspfile.nodes + nodeIndex;
	else
		node->children[index] = (CBspNode*)(bspfile.leafs + (-nodeIndex-1));
	// setup parent for child node/leaf
	assert(!node->children[index]->parent);
	node->children[index]->parent = node;
}

template<class T> static void LoadNodes (T *data, int size)
{
	if (size < 1) Com_DropError ("map with no nodes");

	CBspNode *out;
	out = bspfile.nodes = new (dataChain) CBspNode [size + HULL_NODES];

	for (int i = 0; i < size; i++, data++, out++)
	{
		out->isLeaf      = false;
		out->num         = i;
		out->plane       = bspfile.planes + data->planeNum;
		// bounds: int[3] -> CVec3
		for (int j = 0; j < 3; j++)
		{
			out->bounds.mins[j] = data->mins[j];
			out->bounds.maxs[j] = data->maxs[j];
		}
		SetChild (out, 0, data->children[0]);
		SetChild (out, 1, data->children[1]);
	}
}


/*-----------------------------------------------------------------------------
	q2 bsp map loading
-----------------------------------------------------------------------------*/

static void LoadSurfaces2 (dBsp2Texinfo_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no surfaces");

	ReadSurfMaterials ("sound/materials.lst");

	csurface_t	*out;
	out = bspfile.surfaces = new (dataChain) csurface_t [size];

	for (int i = 0; i < size; i++, data++, out++)
	{
		appStrncpyz (out->shortName, data->texture, sizeof(out->shortName));	// texture name limited to 16 chars (compatibility with older mods?)
		appStrncpyz (out->fullName, data->texture, sizeof(out->fullName));		// name limited to 32 chars (w/o "textures/" and ".wal")
		int f = data->flags;
		out->flags = f & ~SURF_KP_MATERIAL;
		out->value = data->value;

		int m;
		if		(f & SURF_CONCRETE)	m = MATERIAL_CONCRETE;
		else if (f & SURF_FABRIC)	m = MATERIAL_FABRIC;
		else if (f & SURF_GRAVEL)	m = MATERIAL_GRAVEL;
		else if (f & SURF_METAL) 	m = MATERIAL_METAL;
		else if (f & SURF_METAL_L)	m = MATERIAL_METAL_L;
		else if (f & SURF_SNOW)		m = MATERIAL_TIN;		//?? MATERIAL_SNOW;
		else if (f & SURF_TILE)		m = MATERIAL_TILE;
		else if (f & SURF_WOOD)		m = MATERIAL_WOOD;
		else if (f & SURF_WATER)	m = MATERIAL_WATER;
		else						m = GetSurfMaterial (data->texture);
		out->material = m;
	}
}


static void LoadBrushes2 (dBsp2Brush_t *data, int size)
{
	cbrush_t	*out;
	out = map_brushes = new (dataChain) cbrush_t [size + HULL_BRUSHES];
	numBrushes = size;

	for (int i = 0; i < size; i++, data++, out++)
	{
		out->firstbrushside = data->firstside;
		out->numsides       = data->numsides;
		out->contents       = data->contents;
	}
}


static void LoadLeafs2 (dBsp2Leaf_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no leafs");

	CBspLeaf *out;
	out = bspfile.leafs = new (dataChain) CBspLeaf [size + HULL_LEAFS];
	for (int i = 0; i < size; i++, data++, out++)
	{
		out->isLeaf    = true;
		out->num       = i;
		out->contents  = data->contents;
		out->cluster   = data->cluster;
		out->area      = data->area;
		out->firstFace = data->firstleafface;
		out->numFaces  = data->numleaffaces;
		for (int j = 0; j < 3; j++)
		{
			// short[3] -> CVec
			out->bounds.mins[j] = data->mins[j];
			out->bounds.maxs[j] = data->maxs[j];
		}
	}

	if (bspfile.leafs[0].contents != CONTENTS_SOLID)
		Com_DropError ("map leaf 0 is not CONTENTS_SOLID");
}


static void LoadPlanes2 (dBsp2Plane_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no planes");

	CPlane *out;
	staticAssert(sizeof(CPlane) == sizeof(dBsp2Plane_t), sizeof_cplane_same_as_dplane);
	// can simply set bspfile.planes=data; note: "data" is dBsp2Plane_t, but "bspfile.planes" is CPlane,
	// but both have the same size (see FileFormats.h for details) and the same layout
	out = bspfile.planes = (CPlane*)data;
	for (int i = 0; i < size; i++, out++)
		out->Setup ();
}


// should be called after LoadLeafs() and LoadBrushes()
//?? may be used as template for 16/32-bit "*data" (q2/q3 formats)
inline void LoadLeafBrushes (unsigned short *data, int size)
{
	if (size < 1) Com_DropError ("Map with no leafbrushes");

	CBspLeaf *leaf = bspfile.leafs;
	dBsp2Leaf_t *leaf0 = bspfile.leafs2;
	for (int i = 0; i < bspfile.numLeafs; i++, leaf++, leaf0++)
	{
		if (!leaf0->numleafbrushes) continue;
		leaf->numBrushes = leaf0->numleafbrushes;
		leaf->brushes    = new (dataChain) cbrush_t* [leaf->numBrushes];
		unsigned short *src = data + leaf0->firstleafbrush;
		for (int j = 0; j < leaf->numBrushes; j++)
			leaf->brushes[j] = map_brushes + src[j];
	}
}


static void LoadBrushSides2 (dBsp2Brushside_t *data, int size)
{
	cbrushside_t *out;
	out = map_brushSides = new (dataChain) cbrushside_t [size + HULL_BRUSH_SIDES];
	numBrushSides = size;

	for (int i = 0; i < size; i++, data++, out++)
	{
		out->plane = &bspfile.planes[data->planenum];
		int j = data->texinfo;
		if (j >= bspfile.numTexinfo)
			Com_DropError ("Bad brushside texinfo");
		out->surface = (j >= 0) ? &bspfile.surfaces[j] : &nullsurface;
	}
}


static void LoadAreas (dArea_t *data, int size)
{
	carea_t *out;
	out = map_areas = new (dataChain) carea_t [size];
	numAreas = size;

	for (int i = 0; i < size; i++, data++, out++)
	{
		out->numAreaPortals  = data->numAreaPortals;
		out->firstAreaPortal = data->firstAreaPortal;
		out->floodvalid      = 0;
		out->floodnum        = 0;
	}
}


inline void LoadAreaPortals (dAreaPortal_t *data, int size)
{
	numareaportals  = size;
	map_areaportals = data;
}


static void LoadQ2Map (bspfile_t *bsp)
{
	LoadPlanes2 (bsp->planes2, bsp->numPlanes);
	LoadSurfaces2 (bsp->texinfo2, bsp->numTexinfo);
	LoadBrushes2 (bsp->brushes2, bsp->numBrushes);
	LoadBrushSides2 (bsp->brushsides2, bsp->numBrushSides);
	LoadLeafs2 (bsp->leafs2, bsp->numLeafs);
	LoadNodes (bsp->nodes2, bsp->numNodes);
	LoadLeafBrushes (bsp->leafbrushes2, bsp->numLeafbrushes);
	LoadAreas (bsp->areas, bsp->numAreas);
	LoadAreaPortals (bsp->areaportals, bsp->numAreaportals);
}


/*-----------------------------------------------------------------------------
	q1/hl bsp map loading
-----------------------------------------------------------------------------*/

static CBrush *GetHugeBrush ()
{
#define BSIZE		65536
	// create large brush: should be allocated dynamically, because it will
	// be modified and stored to leafBrushes[]
	static const CBox largeBounds = {{-BSIZE,-BSIZE,-BSIZE}, {BSIZE,BSIZE,BSIZE}};
	return new (CBrush::mem) CBrush (largeBounds);
}


static void LoadSurfaces1 (dBsp1Texinfo_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no surfaces");

	//?? hack (?): convert bsp1 -> bsp2 texinfo
	//?? note: this will be used by renderer too!
	// note: this will erase texinfo1 field!
	dBsp2Texinfo_t *out;
	out = bspfile.texinfo2 = new (dataChain) dBsp2Texinfo_t [size];

	const char *texNames[4096];
	memset (texNames, 0, sizeof(texNames));

	for (int i = 0; i < size; i++, data++, out++)
	{
		out->value = 0;
		unsigned flags = 0;
		// find miptex
		int miptex = data->miptex;
		int offset = bspfile.miptex1->dataofs[miptex];
		if (offset != -1)
		{
			dBsp1Miptex_t *tex = (dBsp1Miptex_t*)( (byte*)bspfile.miptex1 + offset );
			// texture with data, but without name:
			if (!tex->name[0])
				appSprintf (ARRAY_ARG(out->texture), "unnamed#%d", miptex);
			else if (texNames[miptex])
				strcpy (out->texture, texNames[miptex]);	// texture already checked
			else
			{
				appStrncpyz (out->texture, tex->name, sizeof(out->texture));
				// check wads
				for (TStringSplitter<64, ';'> Wad = *bspfile.Wads; Wad; ++Wad)
				{
					if (!Wad[0]) continue;
					if (ImageExists (va("textures/wads/%s/%s", *Wad, tex->name)))
					{
						appSprintf (ARRAY_ARG(out->texture), "wads/%s/%s", *Wad, tex->name);
						break;
					}
				}
				texNames[miptex] = out->texture;
			}
			// compute surface flags
			if ((tex->name[0] == '*' && bspfile.type == map_q1) ||
				(tex->name[0] == '!' && bspfile.type == map_hl))
				flags |= SURF_WARP;
			else if (tex->name[0] == '{' && bspfile.type == map_hl)
				flags |= SURF_ALPHA;
			else if (!strncmp (tex->name, "sky", 3))
				flags |= SURF_SKY;
			else if (!strncmp (tex->name, "scroll", 6) && bspfile.type == map_hl)
				flags |= SURF_FLOWING;	// should be disabled for all non-"func_conveyor" models
			if (bspfile.type == map_hl)
			{
				// check lights.rad file
				static const char *lightsRad = NULL;
				EXEC_ONCE(
					lightsRad = (char*) GFileSystem->LoadFile ("gfx/lights.rad");
				);
				CSimpleParser text;
				text.InitFromBuf (lightsRad);
				while (const char *line = text.GetLine ())
				{
					char field1[128];
					int r, g, b, intens;
					if (sscanf (line, "%s %d %d %d %d", &field1, &r, &g, &b, &intens) != 5) continue;
					if (!stricmp (field1, tex->name))
					{
						flags |= SURF_LIGHT;
						out->value = intens;
						break;
					}
				}
			}
		}
		else
		{
			miptex = -1;
			out->texture[0] = 0;	// null name
			flags |= SURF_NODRAW;	// not used, but ...
		}
		out->flags = flags;
		memcpy (out->vecs, data->vecs, sizeof(out->vecs));
		out->nexttexinfo = -1;		// no next texture: animation will be processed later
		// HL indirect sun light
		if (bspfile.type == map_hl && (flags & SURF_SKY))
		{
			out->value = 1000;
			out->flags |= SURF_LIGHT;
		}
	}
	// load Q2 textures
	LoadSurfaces2 (bspfile.texinfo2, size);
}


static void LoadFaces1 (dBspFace_t *data, int size)
{
	// compute face bounds in face.plane coordinates (2D)
	// note: same work performed in renderer too
	surfBounds_t *out;
	out = map_faceBounds = new (dataChain) surfBounds_t [size];
	// get map data
	const dEdge_t *edges = bspfile.edges;
	const CVec3 *verts   = bspfile.vertexes2;
	const int *surfedges = bspfile.surfedges;

	for (int i = 0; i < size; i++, data++, out++)
	{
		// prepare
		float mins[2], maxs[2];				// surface extents
		mins[0] = mins[1] =  BIG_NUMBER;
		maxs[0] = maxs[1] = -BIG_NUMBER;
		// compute
		const dBsp2Texinfo_t *stex = bspfile.texinfo2 + data->texinfo;
		const int *pedge = surfedges + data->firstedge;
		for (int j = 0; j < data->numedges; j++, pedge++)
		{
			int idx = *pedge;
			const CVec3 *v;
			if (idx > 0)
				v = verts + (edges+idx)->v[0];
			else
				v = verts + (edges-idx)->v[1];
			float v1 = dot (*v, stex->vecs[0].vec) + stex->vecs[0].offset;
			float v2 = dot (*v, stex->vecs[1].vec) + stex->vecs[1].offset;
			// Update bounds
			EXPAND_BOUNDS(v1, mins[0], maxs[0]);
			EXPAND_BOUNDS(v2, mins[1], maxs[1]);
		}
		// store
		out->mins[0] = mins[0];
		out->mins[1] = mins[1];
		out->maxs[0] = maxs[0];
		out->maxs[1] = maxs[1];
	}
}


static void LoadLeafs1 (dBsp1Leaf_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no leafs");

	int i;

	// count number of leafs, which should be generated: q1 bsp uses single leaf with
	// CONTENTS_SOLID - so, we should clone this leaf for correct brush generation
	int numSolidLeafs = 0;
	for (i = 0; i < bspfile.numNodes; i++)
	{
		dBsp1Node_t *node = bspfile.nodes1 + i;			// do not use bspfile.nodes[] -- nodes should be generated after leafs
		// leaf[0] should be CONTENTS_SOLID (checked below)
		// count number of links to leaf[0] and replace with new leaf index
		// (leafs will be generated later)
		if (node->children[0] == -1)
			node->children[0] = -1 - (size + numSolidLeafs++);
		if (node->children[1] == -1)
			node->children[1] = -1 - (size + numSolidLeafs++);
	}
	Com_DPrintf ("Found %d solid leafs (have %d normal leafs)\n", numSolidLeafs, size);

	CBspLeaf *out;
	out = bspfile.leafs = new (dataChain) CBspLeaf [size + numSolidLeafs + HULL_LEAFS];
	bspfile.numLeafs = size + numSolidLeafs;

	dBsp1Leaf_t *in;
	int numClipLeafs = 0;
	for (i = 0, in = data; i < size; i++, in++, out++)
	{
//		memcpy (out->mins, in->mins, sizeof(short)*6);	// copy mins[]+maxs[] -- unused here
		// find corresponding Q2 contents
		static const int q1Contents[] =
		{
			0,							// -1 (EMPTY)
			CONTENTS_SOLID,				// -2 (SOLID)
			CONTENTS_WATER,				// -3 (WATER)
			CONTENTS_SLIME,				// ...
			CONTENTS_LAVA,
			CONTENTS_MONSTER,			// SKY
			CONTENTS_ORIGIN,			// removed while bsp
			CONTENTS_SOLID,				// CLIP
			CONTENTS_CURRENT_0|CONTENTS_WATER,
			CONTENTS_CURRENT_90|CONTENTS_WATER,
			CONTENTS_CURRENT_180|CONTENTS_WATER,
			CONTENTS_CURRENT_270|CONTENTS_WATER,
			CONTENTS_CURRENT_UP|CONTENTS_WATER,
			CONTENTS_CURRENT_DOWN|CONTENTS_WATER,
			CONTENTS_TRANSLUCENT,		// -15
			CONTENTS_LADDER|CONTENTS_SOLID
		};
		if (in->contents >= 0 || in->contents < -16)
			Com_DropError ("unknown contents in Q1/HL map: %d", in->contents);
		out->isLeaf     = true;
		out->num        = i;
		out->contents   = q1Contents[-in->contents - 1];	// -1 => 0, -2 => 1 ...
		out->area       = 0;
		out->numBrushes = 0;
		// texture info (here for trace)
		out->firstFace  = in->firstleafface;
		out->numFaces   = in->numleaffaces;
		for (int j = 0; j < 3; j++)
		{
			// short[3] -> CVec
			out->bounds.mins[j] = in->mins[j];
			out->bounds.maxs[j] = in->maxs[j];
		}

		if (out->contents == CONTENTS_SOLID)
			out->cluster = -1;
		else if (bspfile.visInfo)
			out->cluster = i-1;			// == leaf number -1 (no visinfo for leaf[0]==SOLID); visinfo generated in models.cpp
		// else - cluster = 0
		//?? use in->ambient_level[]
		// count other non-empty leafs
		if (out->contents && out->contents != CONTENTS_SOLID)
			numClipLeafs++;
	}

	// generate solid leafs
	for (i = 0; i < numSolidLeafs; i++, out++)
	{
		out->isLeaf     = true;
		out->num        = size+i;
		out->contents   = CONTENTS_SOLID;
		out->area       = 0;
		out->numBrushes = 0;			// will be set to 1 later
		out->cluster    = -1;
	}

	numBrushes = numSolidLeafs + numClipLeafs;

#if 0
	int minCounts[17], surfCounts[17], leafCounts[17];
	memset(minCounts,0x7F,17*4);
	memset(surfCounts,0,17*4);
	memset(leafCounts,0,17*4);
	const char *q1names[] = {
		"??", "EMPTY", "SOLID", "WATER", "SLIME", "LAVA", "SKY", "ORIGIN", "CLIP",
		"CURR_0", "CURR_90", "CURR_180", "CURR_270", "CURR_UP", "CURR_DOWN", "TRANS", "LADDER"
	};
	for (i = 0, in = data; i < size; i++, in++)
	{
		int n = -in->contents;
		minCounts[n] = min(minCounts[n], in->numleaffaces);
		surfCounts[n] += in->numleaffaces;
		leafCounts[n]++;
	}
	for (i = 0; i < 17; i++)
		if (leafCounts[i])
			appPrintf ("%2d/%10s: min=%5d  surf=%5d  leaf=%5d\n", i, q1names[i], minCounts[i], surfCounts[i], leafCounts[i]);
#endif

	if (bspfile.leafs[0].contents != CONTENTS_SOLID)
		Com_DropError ("map leaf 0 is not CONTENTS_SOLID");
}


#if BRUSH_PROFILE
static int64 brushFindPlaneTime;
static int brushFindPlaneCount, brushFindPlaneSuccess;
#endif

// Most slowest operation: FindPlane()
// from experiments: only about 10% of search requests successfull
// On VERY complex map: 1000 found planes == 20Kb of memory
// So, in a cost of 20Kb of memory we can SIGNIFICANTLY speedup map loading
// by disabling search in already existent planes.
#define DONT_SEARCH_PLANES		1

static CPlane *FindPlane (const CVec3 &norm, float dist)
{
	BRUSH_STAT(clock(brushFindPlaneTime));
	BRUSH_STAT(brushFindPlaneCount++);
	CPlane *p2 = bspfile.planes;
#if !DONT_SEARCH_PLANES
	for (int i = 0; i < bspfile.numPlanes; i++, p2++)
	{
		if (p2->dist   != dist) continue;
		if (p2->normal == norm)
		{
			BRUSH_STAT(unclock(brushFindPlaneTime));
			BRUSH_STAT(brushFindPlaneSuccess++);
			return p2;
		}
	}
#endif
	//?? can remember new planes too, and use them when possible
	p2 = new (dataChain) CPlane;
	p2->normal = norm;
	p2->dist   = dist;
	p2->Setup ();
	BRUSH_STAT(unclock(brushFindPlaneTime));
	return p2;
}

static CPlane *FindBackplane (CPlane *plane)
{
	BRUSH_STAT(clock(brushFindPlaneTime));
	BRUSH_STAT(brushFindPlaneCount++);
	CPlane *p2 = bspfile.planes;
	CVec3 norm;
	VectorNegate (plane->normal, norm);
	float dist = -plane->dist;
#if !DONT_SEARCH_PLANES
	for (int i = 0; i < bspfile.numPlanes; i++, p2++)
	{
		if (p2->dist   != dist) continue;
		if (p2->normal == norm)
		{
			BRUSH_STAT(unclock(brushFindPlaneTime));
			BRUSH_STAT(brushFindPlaneSuccess++);
			return p2;
		}
	}
#endif
	//?? can remember new planes too, and use them when possible
	p2 = new (dataChain) CPlane;
	p2->normal = norm;
	p2->dist   = dist;
	p2->Setup ();
	BRUSH_STAT(unclock(brushFindPlaneTime));
	return p2;
}


//?? profile function parts, optimize
static void BuildBrushes1 (int numLeafsOrig)
{
	guard(BuildBrushes1);

	// prepare
	CBrush::mem = new CMemoryChain;
	int64 time = appCycles64 ();
#if BRUSH_PROFILE
	brushFindPlaneTime = brushFindPlaneCount = brushFindPlaneSuccess = 0;
	int64 brushBevelTime = 0;
	int64 brushSplitTime = 0;
	int64 brushConvertTime = 0;
#endif

	map_brushes = new (dataChain) cbrush_t [numBrushes+HULL_BRUSHES];

	numBrushSides = 0;
	CBrush *leafBrushes[MAX_MAP_LEAFS];
	memset (leafBrushes, 0, sizeof(leafBrushes));
	for (int model = 0; model < bspfile.numModels; model++)
	{
		// stack
		CBspNode *stack[MAX_TREE_DEPTH];
		int sptr = 0;								// stack
		CBrush *stack2[MAX_TREE_DEPTH];

		CBspNode *node = bspfile.nodes + bspfile.models[model].headnode;
		CBrush *brush = GetHugeBrush ();
		while (true)
		{
			if (node->isLeaf)
			{
				const CBspLeaf *leaf = (CBspLeaf*)node;
				assert(leaf->num > 0);				// leaf[0] should be replaced
				assert(leaf->num == leaf - bspfile.leafs);
				if (leaf->contents)					// non-empty leafs
				{
					// remember brush
					assert(!leafBrushes[leaf->num]);
					// build bevels for correct trace() against box
					BRUSH_STAT(clock(brushBevelTime));
					brush->AddBevels (FindPlane);
					BRUSH_STAT(unclock(brushBevelTime));
					leafBrushes[leaf->num] = brush;
					// count number of brush sides
					for (CBrushSide *s = brush->sides; s; s = s->next)
						if (s->plane->dist != BSIZE)
							numBrushSides++;
				}

				if (!sptr)
					break;							// whole tree visited
				else
				{
					sptr--;
					node  = stack[sptr];
					brush = stack2[sptr];
					continue;
				}
			}

			// split brush with plane
			BRUSH_STAT(clock(brushSplitTime));
			CBrush *backBrush = brush->Split (node->plane);
			BRUSH_STAT(unclock(brushSplitTime));
			if (!backBrush) Com_DropError ("NULL backBrush");
			// remember back side ...
			stack[sptr]  = node->children[1];
			stack2[sptr] = backBrush;
			sptr++;
			// and continue with front side
			node = node->children[0];
		}
	}

	// alloc brushsides
	map_brushSides = new (dataChain) cbrushside_t [numBrushSides+HULL_BRUSH_SIDES];

	// convert CBrush to cbrush_t and cbrushside_t
	int brushSideIdx = 0;
	CBspLeaf *leaf   = bspfile.leafs;
	cbrushside_t *bs = map_brushSides;
	cbrush_t *dst    = map_brushes;
	BRUSH_STAT(clock(brushConvertTime));
	for (int i = 0; i < bspfile.numLeafs; i++, leaf++)
	{
		if (!leaf->contents || (leaf->contents == CONTENTS_SOLID && i < numLeafsOrig))
			continue;

		leaf->brushes = new (dataChain) cbrush_t*;
		leaf->brushes[0] = dst;
		leaf->numBrushes = 1;

		dst->contents = leaf->contents;
		CBrush *brush = leafBrushes[i];
		if (!brush)
			Com_DropError ("brush %d not generated", i);
		dst->firstbrushside = brushSideIdx;
		for (CBrushSide *s = brush->sides; s; s = s->next)
			if (s->plane->dist != BSIZE)			// ignore largeBounds sides
			{
				bs->plane   = (s->back) ? FindBackplane (s->plane) : s->plane;
				bs->surface = &nullsurface;			// texture will be found by trace function
				bs++; brushSideIdx++;				// next brushside
				dst->numsides++;
			}
		dst++;										// next brush
	}
	BRUSH_STAT(unclock(brushConvertTime));

	// finish
	time = appCycles64 () - time;
	Com_DPrintf ("Built %d brushes in %g msec, used temporary memory %dKb\n",
		numBrushes, appCyclesToMsecf (time), CBrush::mem->GetSize() >> 10);
#if BRUSH_PROFILE
	Com_DPrintf ("FindPlane: %g ms (found %d/%d)\n"
				 "SplitBrush: %g ms\n"
				 "BrushBevels: %g ms\n"
				 "Convert: %g ms\n",
		appCyclesToMsecf (brushFindPlaneTime), brushFindPlaneSuccess, brushFindPlaneCount,
		appCyclesToMsecf (brushSplitTime),
		appCyclesToMsecf (brushBevelTime),
		appCyclesToMsecf (brushConvertTime));
#endif
	delete CBrush::mem;

	unguard;
}


//?? recursive (use 'stack'?)
static void SetNodeContents (CBspNode *node, unsigned contents)
{
	while (true)
	{
		if (node->isLeaf)
		{
			// leaf
			CBspLeaf *leaf = (CBspLeaf*)node;
			if (leaf->contents) leaf->contents = contents;
			// brushes
			for (int i = 0; i < leaf->numBrushes; i++)
			{
				cbrush_t *b = leaf->brushes[0];
				if (b->contents) b->contents = contents;
			}
			return;
		}
		else
		{
			SetNodeContents (node->children[0], contents);
			node = node->children[1];
		}
	}
}


static void ProcessModels1 ()
{
	CBspModel *m = bspfile.models;
	for (int i = 0; i < bspfile.numModels; i++, m++)
	{
		if (!(m->flags & CMODEL_CONTENTS)) continue;
		SetNodeContents (bspfile.nodes + m->headnode, m->contents);
	}
}


static void LoadQ1Map (bspfile_t *bsp)
{
	int numLeafsOrig = bspfile.numLeafs;
	LoadPlanes2 (bsp->planes2, bsp->numPlanes);
	LoadSurfaces1 (bsp->texinfo1, bsp->numTexinfo);
	LoadLeafs1 (bsp->leafs1, bsp->numLeafs);
	LoadNodes (bsp->nodes1, bsp->numNodes);
	BuildBrushes1 (numLeafsOrig);
	LoadFaces1 (bsp->faces2, bsp->numFaces);

	// Q1 areas: simulate loading (create 1 area)
	map_areas = new (dataChain) carea_t;
	numAreas  = 1;
//	map_areas[0].numareaportals  = 0;
//	map_areas[0].firstareaportal = 0;
//	map_areas[0].floodvalid      = 0;
//	map_areas[0].floodnum        = 0;
	numareaportals = 0;

	ProcessModels1 ();
}


/*-----------------------------------------------------------------------------
	q3 bsp loading
-----------------------------------------------------------------------------*/

#if 0
#define Q3MAP_SUPPORT	1		//!!! REMOVE CONST !!!

static unsigned Q3Contents (unsigned f)
{
	unsigned r = 0;
#define T(c)	if (f & Q3_CONTENTS_##c) r |= CONTENTS_##c;
	T(SOLID)
	T(LAVA)
	T(SLIME)
	T(WATER)
	T(PLAYERCLIP)
	T(MONSTERCLIP)
#undef T
	return r;
}

static void LoadBrushes3 (dBsp3Brush_t *data, int size)
{
	cbrush_t	*out;
	out = map_brushes = new (dataChain) cbrush_t [size + HULL_BRUSHES];
	numBrushes = size;

	for (int i = 0; i < size; i++, data++, out++)
	{
		out->firstbrushside = data->firstside;
		out->numsides       = data->numsides;
		out->contents       = Q3Contents (bspfile.texinfo3[data->shaderNum].contentFlags);
	}
}


void LoadLeafBrushes3 (unsigned *data, int size)
{
	if (size < 1) Com_DropError ("Map with no leafbrushes");

	// NOTE: uses int -> short conversion
	//!!! CHANGE !!!
	unsigned short *out;
	out = map_leafBrushes = new (dataChain) unsigned short [size+1];	// extra for box hull
	numLeafBrushes = size;
	for (int i = 0; i < size; i++, data++, out++)
	{
		assert(*data < 65535);
		*out = *data;
	}
}


static void LoadLeafs3 (dBsp3Leaf_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no leafs");

	dBsp2Leaf_t *out;
	out = map_leafs = new (dataChain) dBsp2Leaf_t [size + HULL_LEAFS];
	numLeafs = size;

	for (int i = 0; i < size; i++, data++, out++)
	{
		// NOTE: some fields uses "short" in q2, but corresponding q3 field used "int"
		// To increase limits should use something else than dBsp2Leaf_t
		assert(data->cluster < 65536);
		assert(data->firstleafbrush < 65536);
		out->cluster        = data->cluster;
		out->area           = 0;  //?? data->area -- q3 areas too different from q2
		out->firstleafbrush = data->firstleafbrush;
		out->numleafbrushes = data->numleafbrushes;

		out->contents = 0xFFFFFFFF; //!! compute leaf contents from brushes
		//!! NOTE: q3 uses surfaces from leafs to trace with patches!
	}

	if (!(map_leafs[0].contents & CONTENTS_SOLID))
		Com_DropError ("map leaf 0 is not CONTENTS_SOLID");
}


static void LoadPlanes3 (dBsp3Plane_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no planes");

	CPlane *out;
	out = bspfile.planes = new CPlane [size];
	for (int i = 0; i < size; i++, data++, out++)
	{
		out->normal = data->normal;
		out->dist   = data->dist;
		out->Setup ();
	}
}


static void LoadSurfaces3 (dBsp3Shader_t *data, int size)
{
	if (size < 1) Com_DropError ("Map with no shaders");

	csurface_t	*out;
	out = bspfile.surfaces = new (dataChain) csurface_t [size];

	for (int i = 0; i < size; i++, data++, out++)
	{
		appStrncpyz (out->shortName, data->name, sizeof(out->shortName));	// texture name limited to 16 chars (compatibility with older mods?)
		appStrncpyz (out->fullName, data->name, sizeof(out->fullName));		// name limited to 32 chars (w/o "textures/" and ".wal")
		out->value = 0; //!! get light value from shader
		out->flags = 0; //!! convert from data->surfaceFlags

		//?? SURF_FLESH, SURF_NOSTEPS, SURF_METALSTEPS, SURF_DUST
		int m = GetSurfMaterial (data->name);
		out->material = m;
	}
}


//!! LoadSubmodels3 (): should create 1 leaf to hold all brushes

static void LoadQ3Map (bspfile_t *bsp)
{
	LoadPlanes3 (bsp->planes3, bsp->numPlanes);
	LoadNodes (bsp->nodes3, bsp->numNodes);
	LoadSurfaces3 (bsp->texinfo3, bsp->numTexinfo);
	LoadBrushes3 (bsp->brushes3, bsp->numBrushes);
//!!	LoadBrushSides2 (bsp->brushsides2, bsp->numBrushSides);
	LoadLeafBrushes3 (bsp->leafbrushes3, bsp->numLeafbrushes);
	LoadLeafs3 (bsp->leafs3, bsp->numLeafs);
//!!	LoadSubmodels3 (bsp->models, bsp->numModels);
//!!	LoadAreas (bsp->areas, bsp->numAreas);
//!!	LoadAreaPortals (bsp->areaportals, bsp->numAreaportals);
}

#endif

/*-----------------------------------------------------------------------------
	Common map loading code
-----------------------------------------------------------------------------*/

// Loads in the map and all submodels
CBspModel *CM_LoadMap (const char *name, bool clientload, unsigned *checksum)
{
	guard(CM_LoadMap);

	map_noareas = Cvar_Get ("map_noareas", "0");

	//!! NOTE about "flushmap": should be synced with flushmap in LoadBspFile(): some
	//!!   functions in cmodel.cpp will modify base fields in "bspfile", and CM_* functions
	//!!   should not be called again
	static unsigned	last_checksum;
	if (map_name[0] && !stricmp (map_name, name) && (clientload || !Cvar_VariableInt ("flushmap")))
	{
		*checksum = last_checksum;
		if (!clientload)
		{
			memset (portalopen, 0, sizeof(portalopen));
			FloodAreaConnections ();
		}
		return &bspfile.models[0];
	}

	// free old stuff
	map_name[0] = 0;
	isMapLoaded = false;
	if (dataChain) delete dataChain;
	dataChain = NULL;

	if (!name || !name[0])
	{
		numAreas    = 0;
		*checksum   = 0;
		map_clientLoaded = false;
		return &bspfile.models[0];			// cinematic servers won't have anything at all
	}

	//?? use "bspfile" var (but should call LoadBspFile() anyway)
	bspfile_t *bsp = LoadBspFile (name, clientload, &last_checksum);
	dataChain = new CMemoryChain;
	*checksum = last_checksum;

	switch (bsp->type)
	{
	case map_q2:
	case map_kp:
		LoadQ2Map (bsp);
		break;
	case map_q1:
	case map_hl:
		LoadQ1Map (bsp);
		break;
#if Q3MAP_SUPPORT
	case map_q3:
		LoadQ3Map (bsp);
		break;
#endif
	default:
		Com_DropError ("unknown bsp->type for %s", name);
	}
#if MAX_DEBUG
	if (bspfile.models[0].headnode)
		Com_DropError ("map has invalid headnode = %d", bspfile.models[0].headnode);
#endif

	isMapLoaded = true;

	memset (portalopen, 0, sizeof(portalopen));
	FloodAreaConnections ();
	InitBoxHull ();

	strcpy (map_name, name);
	return &bspfile.models[0];

	unguardf(("%s", name));
}


/*-----------------------------------------------------------------------------
	Debugging brush/node/leaf volumes
-----------------------------------------------------------------------------*/

#if !NO_DEBUG

CBrush *CM_BuildBrush (int brushNum, CMemoryChain *mem)
{
	if (brushNum < 0 || brushNum >= numBrushes) return NULL;

	guard(CM_BuildBrush);

	CBrush::mem = mem;
	CBrush *brush = GetHugeBrush ();

	cbrush_t &b = map_brushes[brushNum];
	int i;
	cbrushside_t *side;
	for (i = 0, side = &map_brushSides[b.firstbrushside]; i < b.numsides; i++, side++)
	{
		CPlane *plane = side->plane;
		brush = brush->Split (plane);		// use backside
		if (!brush) break;					// should not happens
	}

	CBrush::mem = NULL;
	return brush;

	unguardf(("%d", brushNum));
}

#endif


//=============================================================================

CBspModel *CM_InlineModel (const char *name)
{
	if (!name || name[0] != '*') Com_DropError ("CM_InlineModel: bad name");
	return CM_InlineModel (atoi (name+1));
}

CBspModel *CM_InlineModel (int index)
{
	if (index < 1 || index >= bspfile.numModels) Com_DropError ("CM_InlineModel: bad number");
	return &bspfile.models[index];
}

int CM_NumClusters ()
{
	if (!isMapLoaded) return 0;
	return bspfile.numClusters;
}

int CM_NumInlineModels ()		//?? rudimental
{
	return bspfile.numModels;
}


/*-----------------------------------------------------------------------------
	Temporaty box for entity traces
-----------------------------------------------------------------------------*/

static CPlane	boxPlanes[12];
static int		boxHeadnode;


// Set up the planes and nodes so that the six floats of a bounding box
// can just be stored out and get a proper clipping hull structure.
static void InitBoxHull ()
{
	int i;

	// static data
	static CBspLeaf emptyLeaf;
	emptyLeaf.isLeaf = true;
	static cbrush_t *leafBrush[1];

	boxHeadnode = bspfile.numNodes;

	cbrush_t &boxBrush = map_brushes[numBrushes];		//?? can allocate dynamically
	boxBrush.numsides       = 6;
	boxBrush.firstbrushside = numBrushSides;
	boxBrush.contents       = CONTENTS_MONSTER;

	CBspLeaf &boxLeaf = bspfile.leafs[bspfile.numLeafs];
	boxLeaf.isLeaf     = true;
	boxLeaf.contents   = CONTENTS_MONSTER;
	boxLeaf.brushes    = leafBrush;
	boxLeaf.numBrushes = 1;
	leafBrush[0] = &boxBrush;

	for (i = 0; i < 6; i++)
	{
		int side = i & 1;

		// brush sides
		cbrushside_t *s = &map_brushSides[numBrushSides+i];
		s->plane   = boxPlanes + i*2+side;
		s->surface = &nullsurface;

		// nodes
		CBspNode *c = &bspfile.nodes[boxHeadnode+i];
		c->plane = boxPlanes + i*2;
		c->children[side] = (CBspNode*)&emptyLeaf;		// one child -> empty leaf
		if (i != 5)
			c->children[side^1] = c + 1;				// another child -> next node
		else
			c->children[side^1] = (CBspNode*)&boxLeaf;	// ... or -> boxLeaf

		// planes
		CPlane *p = &boxPlanes[i*2];
		p->normal.Zero ();
		p->normal[i>>1] = 1;
		p->Setup ();
		p++;											// switch to opposite plane
		p->normal.Zero ();
		p->normal[i>>1] = -1;
		p->Setup ();
	}
}


// To keep everything totally uniform, bounding boxes are turned into small
// BSP trees instead of being compared directly.
int	CM_HeadnodeForBox (const CBox &box)
{
	boxPlanes[0].dist  = box.maxs[0];
	boxPlanes[2].dist  = box.mins[0];
	boxPlanes[4].dist  = box.maxs[1];
	boxPlanes[6].dist  = box.mins[1];
	boxPlanes[8].dist  = box.maxs[2];
	boxPlanes[10].dist = box.mins[2];

	boxPlanes[1].dist  = -box.maxs[0];
	boxPlanes[3].dist  = -box.mins[0];
	boxPlanes[5].dist  = -box.maxs[1];
	boxPlanes[7].dist  = -box.mins[1];
	boxPlanes[9].dist  = -box.maxs[2];
	boxPlanes[11].dist = -box.mins[2];

	return boxHeadnode;
}


/*-----------------------------------------------------------------------------
	Point info
-----------------------------------------------------------------------------*/

// inline model may be created without nodes -- single leaf; in that case,
// functions will receive headnode < 0; this is a helper function for this
inline CBspNode *GetHeadnode (int index)
{
	if (index >= 0)
		return bspfile.nodes + index;
	else
		return (CBspNode*) (bspfile.leafs + (-index-1));
}


// NOTE: headnode arg used only inside cmodel.cpp
const CBspLeaf *CM_FindLeaf (const CVec3 &p, int headnode)
{
	guard(CM_FindLeaf);
	static CBspLeaf dummyLeaf;
	if (!isMapLoaded) return &dummyLeaf;		// map is not yet loaded; should not use NULL as result?

	const CBspNode *n;
	for (n = GetHeadnode (headnode); !n->isLeaf; )
	{
		float d = n->plane->DistanceTo (p);
		n = n->children[IsNegative(d)];
	}

	c_pointcontents++;							// stats
	return (CBspLeaf*)n;
	unguard;
}


int CM_PointContents (const CVec3 &p, int headnode)
{
	guard(CM_PointContents);
	if (!isMapLoaded) return 0;					// map not loaded
	return CM_FindLeaf (p, headnode)->contents;
	unguard;
}


/* !! ============================================================================== !!
 * CM_HeadnodeForBox() used with CM_Transformed...(..., angles=(0,0,0)) funcs only
 * angle versions of BoxTrace and PointContents used here only (i.e. with offset,
 * no angles, and HeadnodeForBox)
 * Can remove (combine) this functions, simplify BoxTrace/PointContents for box,
 * but: if will combine later with Q3 engine, it exports to game/client/server
 * functions to create temp box (and capsule?) model with ability to trace it later ...
 * Can (in this case) return from TempBoxModel() special handle, which will be handled
 * by trace functions separately (TempBoxModel() will simply remember box params, and
 * trace functions will use them for fast check)
 * If do so, can remove reserving of 6 planes, 1 brush, leafs, brushsides etc (will reduce
 * memory requirements for map loading - some funcs have copied data with reserving few
 * strucs, and without reserving can simply setup data pointer !)
 */

int	CM_TransformedPointContents (const CVec3 &p, int headnode, const CVec3 &origin, const CVec3 &angles)
{
	guard(CM_TransformedPointContents);

	CVec3	p1;
	if (headnode != boxHeadnode && (angles[0] || angles[1] || angles[2]))
	{
		CAxis axis;
		axis.FromEuler (angles);
		TransformPoint (origin, axis, p, p1);
	}
	else
		VectorSubtract (p, origin, p1);

	return CM_FindLeaf (p1, headnode)->contents;

	unguard;
}

int	CM_TransformedPointContents (const CVec3 &p, int headnode, const CVec3 &origin, const CAxis &axis)
{
	guard(CM_TransformedPointContents2);

	CVec3	p1;
	if (headnode != boxHeadnode)
		TransformPoint (origin, axis, p, p1);
	else
		VectorSubtract (p, origin, p1);

	return CM_FindLeaf (p1, headnode)->contents;

	unguard;
}


/*-----------------------------------------------------------------------------
	Box info
-----------------------------------------------------------------------------*/

int CM_BoxLeafs (const CBox &bounds, CBspLeaf **list, int listsize, int *topnode, int headnode)
{
	guard(CM_BoxLeafs);

	if (!isMapLoaded) return 0;

	// stack
	CBspNode *stack[MAX_TREE_DEPTH];
	int sptr = 0;

	int count = 0;
	int _topnode = -1;
	CBspNode *n = GetHeadnode (headnode);

	while (true)
	{
		if (n->isLeaf)
		{
			if (count >= listsize) break;		// list is full
			list[count++] = (CBspLeaf*)n;

			if (!sptr)
				break;							// whole tree visited
			else
			{
				n = stack[--sptr];
				continue;
			}
		}

		switch (BOX_ON_PLANE_SIDE(bounds, *n->plane))
		{
		case 1:
			n = n->children[0];
			break;
		case 2:
			n = n->children[1];
			break;
		default:
			// go down both
			if (_topnode == -1)
				_topnode = n->num;				// remember top node, which subdivides box
			stack[sptr++] = n->children[1];
			n = n->children[0];
		}
	}

	if (topnode) *topnode = _topnode;
	return count;

	unguard;
}


/*-----------------------------------------------------------------------------
	Box trace
-----------------------------------------------------------------------------*/

#define	DIST_EPSILON	(1.0f/32)
#define MAX_CLIP_FACES	256

//?? when extending trace with SphereTrace: can add special function (TraceDistanceToPlane()), which
//?? will be called by pointer, and create 3 funcs: for point/box/sphere trace

static struct
{
	CVec3	start, end;
	trace_t	trace;
	unsigned contents;
	unsigned skipContents;
	int		numClipFaces;			// for q1/hl surface trace
	// following fields used in GetTraceOffset() and SplitTrace() only
	CBox	bounds;					// trace box bounds; symetric, bounds.maxs == bounds half-size
	CVec3	boundsVecs[8];			// corners of bounds
	bool	isPoint;				// used outside of these functions, but exclusively for SplitTrace()
} tr;
static int traceFrame;		// cannot place into "tr": should increment value between frames ("tr" will be zeroed)

static int clipFaces[MAX_CLIP_FACES]; // [tr.numClipFaces]


FORCEINLINE float GetTraceOffset (const CPlane &plane)
{
	if (plane.type <= CPlane::PLANE_Z)
		// HERE: plane.normal[i] == 0 || 1 for i==type
		return tr.bounds.maxs[plane.type]; // symmetric bounds => == - tr.bounds.mins[plane->type]
	else if (plane.type <= CPlane::PLANE_MZ)
		// HERE: plane.normal[i] == 0 || -1 for i==type-3
		return tr.bounds.maxs[plane.type-3];
	else
		return -dot(plane.normal, tr.boundsVecs[plane.signbits]);
}


FORCEINLINE float SplitTrace (const CVec3 &p1, const CVec3 &p2, const CPlane &plane, float &d1, float &d2, bool isPoint = true)
{
	// this is a 2 plane->DistanceTo() calls with a common plane->type analysis
	if (plane.type <= CPlane::PLANE_Z)
	{
		d1 = p1[plane.type] - plane.dist;
		d2 = p2[plane.type] - plane.dist;
	}
	else if (plane.type <= CPlane::PLANE_MZ)
	{
		d1 = -p1[plane.type-3] - plane.dist;
		d2 = -p2[plane.type-3] - plane.dist;
	}
	else
	{
		d1 = dot(plane.normal, p1) - plane.dist;
		d2 = dot(plane.normal, p2) - plane.dist;
	}
	if (isPoint) return 0;

	return GetTraceOffset (plane);
}


// return 'true' when brush clipped trace
static bool TraceBrush (const cbrush_t &brush)
{
	int		i;
	cbrushside_t *side;

	if (!brush.numsides) return false;

	float enterfrac = -1;
	float leavefrac = 1;					// used only for validating enterfrac
	const CPlane *clipplane = NULL;

	bool	getout, startout;
	getout = startout = false;
	cbrushside_t *leadside = NULL;

	for (i = 0, side = &map_brushSides[brush.firstbrushside]; i < brush.numsides; i++, side++)
	{
		const CPlane *plane = side->plane;
		float d1, d2;
		float dist = SplitTrace (tr.start, tr.end, *plane, d1, d2, tr.isPoint);
		d1 -= dist;
		d2 -= dist;

		TRACE(va("plane: %g %g %g : %g -- d1=%g d2=%g (%s)",VECTOR_ARG(plane->normal),plane->dist, d1, d2, side->surface->fullName),RGB(1,0,0));
		// d1 and d2: 0 -- on plane, <0 -- inside brush plane, >0 -- outside brush plane

		if (d1 > 0 && d2 > 0)
		{
			/* math note: here 2 situations
			 * 1) d1 > d2    -- enterfrac will be > 1
			 * 2) d1 < d2    -- leavefrac will be < 0
			 */
			TRACE("d1>0 && d2>0",RGB(0,1,0));
			return false;
		}

		if (d1 <= 0 && d2 <= 0)			// completely in back of this face
			continue;

		if (d1 > 0) startout = true;	// startpoint is not inside this brush
		if (d2 > 0) getout   = true;	// endpoint is not inside this brush

		// crosses face
		float f;
		if (d1 > d2)
		{	// enter
			f = (d1-DIST_EPSILON) / (d1 - d2);
			if (f > enterfrac)
			{
				enterfrac = f;
				clipplane = plane;
				leadside  = side;
			}
		}
		else
		{	// leave
			f = (d1+DIST_EPSILON) / (d1 - d2);
			if (f < leavefrac)
				leavefrac = f;
		}

		TRACE(va("f=%g => ef=%g : lf=%g",f,enterfrac,leavefrac),RGB(1,1,1));
		if (enterfrac >= leavefrac)
		{
			TRACE("** END: enterfrac < leavefrac",RGB(0.7,0.6,0.5));
			return false;			// intersects (at least) 2 faces outside the brush
		}
	}

	if (!startout)
	{
		TRACE("** END: startout",RGB(0.7,0.6,0.5));
		// original point was inside brush
		tr.trace.startsolid = true;
		tr.trace.allsolid   = !getout;
		return true;
	}

	TRACE(va("** END: ef=%g (>-1 & <%g)",enterfrac,tr.trace.fraction),RGB(0.7,0.6,0.5));
	if (enterfrac > -1 && enterfrac < tr.trace.fraction)
	{
		if (enterfrac < 0)			// when startsolid && allsolid
			enterfrac = 0;
		tr.trace.fraction = enterfrac;
		tr.trace.plane    = *clipplane;
		tr.trace.surface  = leadside->surface;
		tr.trace.contents = brush.contents;
		TRACE(va("* best: plane=%g %g %g / %g", VECTOR_ARG(clipplane->normal),clipplane->dist),RGB(1,0.3,0.3));
		return true;
	}

	return false;
}


static void TraceLeaf (const CBspLeaf &leaf)
{
	if (!(leaf.contents & tr.contents))
		return;
	// trace line against all brushes in the leaf
	for (int i = 0; i < leaf.numBrushes; i++)
	{
		cbrush_t *b = leaf.brushes[i];
		TRACE(va("--- %d cont=%X numSides=%d ---", b - map_brushes, b->contents, b->numsides),RGB(0.3,1,0.3));
		if (b->traceFrame == traceFrame)
			continue;					// already checked this brush in another leaf
		b->traceFrame = traceFrame;
		if (!(b->contents & tr.contents)) continue;
		if (b->contents & tr.skipContents) continue;

		if (TraceBrush (*b))
			tr.trace.brushNum = b - map_brushes;
		if (!tr.trace.fraction)			// when startsolid && allsolid
			return;
	}
}


/*
================
TestBoxInBrush

if box inside this brush or intersects its brushsides:
 1) trace->fraction=0
 2) trace->startsolid = true
 3) trace->allsolid = true
else trace is unchanged
================
*/
static bool TestBrush (const cbrush_t &brush)
{
	if (!brush.numsides)
		return false;

	float clipdist = -BIG_NUMBER;
	cbrushside_t *clipside = NULL;
	int i;
	cbrushside_t *side;
	for (i = 0, side = &map_brushSides[brush.firstbrushside]; i < brush.numsides; i++, side++)
	{
		const CPlane &plane = *side->plane;
		float d = plane.DistanceTo (tr.start) - GetTraceOffset (plane) - 0.000001;//?? - DIST_EPSILON;	// -DIST_EPSILON move surface farther of trace
					//??? -DIST_EPSILON => bugs with rotating model on q2dm5; without this => bugs with water cistern on boot_camp
		// if completely in front of face, no intersection
		if (d > 0) return false;
		if (d > clipdist)
		{
			clipdist = d;
			clipside = side;
		}
	}

	// inside this brush
	tr.trace.startsolid = tr.trace.allsolid = true;
	tr.trace.fraction = 0;
	tr.trace.contents = brush.contents;
	tr.trace.plane    = *clipside->plane;
	tr.trace.surface  = clipside->surface;				//?? note: not valid for Q1/HL map

	return true;
}


// Call TestBoxInBrush for each leafbrush.
// When first intersection found (trace.fraction == 0) - return
static void TestLeaf (const CBspLeaf &leaf)
{
	if (!(leaf.contents & tr.contents))
		return;

	// trace line against all brushes in the leaf
	for (int i = 0; i < leaf.numBrushes; i++)
	{
		cbrush_t *b = leaf.brushes[i];
		if (b->traceFrame == traceFrame)
			continue;	// already checked this brush in another leaf
		b->traceFrame = traceFrame;

		if (!(b->contents & tr.contents))
			continue;
		if (TestBrush (*b))
		{
			tr.trace.brushNum = b - map_brushes;
			return;
		}
	}
}


/*-----------------------------------------------------------------------------
	Searching trace surface for Q1/HL BSP
-----------------------------------------------------------------------------*/

static void FindSurface1 (const CBspLeaf &leaf, float p1f, float p2f)
{
	const unsigned short *leafFaces = bspfile.leaffaces2 + leaf.firstFace;
	TRACE("----------------",RGB(1,1,0));
	for (int i = 0; i < leaf.numFaces; i++, leafFaces++)
	{
		int faceNum = *leafFaces;
		const dBspFace_t &face = bspfile.faces2[faceNum];
		const CPlane *plane = bspfile.planes + face.planenum;

		float d1, d2;
		float dist = SplitTrace (tr.start, tr.end, *plane, d1, d2, tr.isPoint);
		d1 -= dist;
		d2 -= dist;

		int s1 = IsNegative (d1);
		int s2 = IsNegative (d2);
		if (s1 != face.side) continue;		// 'start' placed on a back side of surface => skip surface
		if (s1 == s2) continue;				// start/end on the same side of plane
		if (face.side)
		{
			FNegate (d1);
			FNegate (d2);
		}

		// crosses face
		// here: d1 and d2 have different signs, and 'start' placed on the front side of face
		// and: sign(d1) == 0 (positive); so: d1 always > d2
		float frac = (d1-DIST_EPSILON) / (d1 - d2);
		if (frac < p1f || frac > p2f) continue; // crosses plane outside leaf (outside surface)

		// check surface bounds
		CVec3 mid;
		Lerp (tr.start, tr.end, frac, mid);
		dBsp2Texinfo_t *tex = bspfile.texinfo2 + face.texinfo;
		float s = dot (tex->vecs[0].vec, mid) + tex->vecs[0].offset;
		float t = dot (tex->vecs[1].vec, mid) + tex->vecs[1].offset;
		const surfBounds_t &bounds = map_faceBounds[faceNum];
		if (s < bounds.mins[0] || s > bounds.maxs[0]) continue;
		if (t < bounds.mins[1] || t > bounds.maxs[1]) continue;

		// remember surface
		// note: there can be any surface from leaf, with any frac: we do not know, which contents
		// behind the face, so - remember all crossed faces
		TRACE(va("surf[%d] %g %g %g : %g (%d) \"%s\" <-> %g [%g .. %g]", faceNum, VECTOR_ARG(plane->normal), plane->dist,
			face.side, tex->texture, frac, p1f, p2f),RGB(0.6,0.2,0.6));
		if (tr.numClipFaces < ARRAY_COUNT(clipFaces))
			clipFaces[tr.numClipFaces++] = faceNum;
		//?? if array is too small, can remove 1st face (make FIFO?) -- if so, can reduce array size
	}
}


static void SetTraceFace1 ()
{
	if (tr.trace.fraction == 1) return;		// no collision
	if (!tr.numClipFaces) return;			// no faces crossed
	for (int i = 0; i < tr.numClipFaces; i++)
	{
		int faceNum = clipFaces[i];
		const dBspFace_t &face = bspfile.faces2[faceNum];
		const CPlane *plane = bspfile.planes + face.planenum;
		// check face plane
		if (!face.side)
		{
			if (fabs (plane->dist - tr.trace.plane.dist) > DIST_EPSILON) continue;
			if (dot (plane->normal, tr.trace.plane.normal) < 0.99f) continue;
		}
		else
		{
			if (fabs (-plane->dist - tr.trace.plane.dist) > DIST_EPSILON) continue;
			if (dot (plane->normal, tr.trace.plane.normal) > -0.99f) continue;
		}
		// surface found
		tr.trace.surface = bspfile.surfaces + face.texinfo;
	}
}


/*-----------------------------------------------------------------------------
	Common trace code
-----------------------------------------------------------------------------*/

static void RecursiveHullCheck (const CBspNode *node, float p1f, float p2f, const CVec3 &point1, const CVec3 &point2)
{
	CVec3 p1 = point1;
	CVec3 p2 = point2;

	while (true)
	{
		TRACE(va("trace: n=%d f=%g .. %g (best=%g)",node->num,p1f,p2f,tr.trace.fraction),RGB(0,1,0.2));
		if (tr.trace.fraction <= p1f)
		{
			TRACE("too far",RGB(1,1,0));
			return;		// already hit something nearer; this will break tree walking
		}

		// if < 0, we are in a leaf node
		if (node->isLeaf)
		{
			const CBspLeaf &leaf = *((CBspLeaf*)node);
			if (bspfile.type == map_q1 || bspfile.type == map_hl)
				FindSurface1 (leaf, p1f, p2f);
			TraceLeaf (leaf);
			return;
		}

		// find the point distances to the separating plane
		// and the offset for the size of the box
		const CPlane &plane = *node->plane;

		float d1, d2;
		float offset = SplitTrace (p1, p2, plane, d1, d2, tr.isPoint);

		// see which sides we need to consider
		if (d1 >= offset && d2 >= offset)
		{
			node = node->children[0];
			continue;
		}
		if (d1 < -offset && d2 < -offset)
		{
			node = node->children[1];
			continue;
		}

		int side;
		float frac1, frac2;
		// put the crosspoint DIST_EPSILON pixels on the near side
		if (d1 == d2)
		{
			frac1 = 1;
			frac2 = 0;
			side  = 0;
		}
		else
		{
			float idist = 1.0f / (d1 - d2);
			offset += DIST_EPSILON;
#if 0
			if (d1 < d2)						// NOTE: here idist<0
			{
				frac1 = (d1 - offset) * idist;	// -offset*idist > 0
				frac2 = (d1 + offset) * idist;	// +offset*idist < 0
				side  = 1;
			}
			else if (d1 > d2)					// here idist>0
			{
				frac1 = (d1 + offset) * idist;	// +offset*idist > 0
				frac2 = (d1 - offset) * idist;	// -offset*idist < 0
				side  = 0;
			}
#else
			frac1 = frac2 = d1 * idist;
			float offset2;
			FAbsSign (idist, offset2, side);	// side=1 when t1<t2
			offset2 *= offset;					// offset2 = fabs(idist)*offset; >0
			frac1 += offset2;
			frac2 -= offset2;
#endif

			frac1 = bound(frac1, 0, 1);
			frac2 = bound(frac2, 0, 1);
		}

		float midf;
		CVec3 mid;

		// visit near-side node (recurse)
		midf = Lerp (p1f, p2f, frac1);
		Lerp (p1, p2, frac1, mid);
		RecursiveHullCheck (node->children[side], p1f, midf, p1, mid);

		// visit far-side node (loop)
		midf = Lerp (p1f, p2f, frac2);
#if 0
		Lerp (p1, p2, frac2, mid);
		RecursiveHullCheck (node->children[side^1], midf, p2f, mid, p2);
#else
		Lerp (p1, p2, frac2, p1);
		// do "RecursiveHullCheck (node.children[side^1], midf, p2f, mid, p2);"
		p1f = midf;
		node = node->children[side^1];
#endif
	}
}


/*
==================
CM_BoxTrace

When start==end:
  1) trace.contents is set (only this field contains information!)
  2) trace.fraction = 0
  3) trace.endpos = start
  4) trace.allsolid = trace.startsolid = true
  5) trace.surface = nullsurface
  6) trace.ent & trace.plane are unchanged
==================
*/

bool trace_skipAlpha;	//!! hack!

//?? check: can this be faster if trace with sphere
void CM_BoxTrace (trace_t &trace, const CVec3 &start, const CVec3 &end, const CBox &bounds, int headnode, unsigned contents)
{
	guard(CM_BoxTrace);

	int i;

	traceFrame++;	// to avoid testing the same brush from different leafs
	c_traces++;		//??

	// fill in a default trace
	memset (&tr, 0, sizeof(tr));
	tr.trace.fraction = 1;
	tr.trace.surface  = &nullsurface;
	tr.trace.brushNum = -1;

	if (!isMapLoaded)	// map not loaded
	{
		trace = tr.trace;
		return;
	}

	tr.contents = contents;
	tr.start    = start;
	tr.end      = end;
	tr.bounds   = bounds;
	if (trace_skipAlpha) tr.skipContents = CONTENTS_ALPHA;
	// make trace bounds symmetric
	for (i = 0; i < 3; i++)
	{
		float f = (tr.bounds.mins[i] + tr.bounds.maxs[i]) / 2;	// center of bounds
		tr.start[i] += f;
		tr.end[i]   += f;
		tr.bounds.mins[i] -= f;				// move bounds center to {0,0,0}
		tr.bounds.maxs[i] -= f;
	}
	for (int boundsVec = 0; boundsVec < 8; boundsVec++)
	{
		int signbits = boundsVec;
		for (int j = 0; j < 3; j++, signbits >>= 1)
			tr.boundsVecs[boundsVec][j] = (signbits & 1) ? tr.bounds.maxs[j] : tr.bounds.mins[j];
	}

	// check for "position test" special case
	if (start[0] == end[0] && start[1] == end[1] && start[2] == end[2])
	{
		CBox box;
		for (i = 0; i < 3; i++)
		{
			box.mins[i] = tr.start[i] + tr.bounds.mins[i] - 1;
			box.maxs[i] = tr.start[i] + tr.bounds.maxs[i] + 1;
		}

		CBspLeaf *leafs[1024];
		int numLeafs = CM_BoxLeafs (box, ARRAY_ARG(leafs), NULL, headnode);
		for (i = 0; i < numLeafs; i++)
		{
			TestLeaf (*leafs[i]);
			if (tr.trace.allsolid)	// always set when 1st intersection by CM_TestBoxInBrush()
				break;
		}
		tr.trace.endpos = start;
		trace = tr.trace;
		return;
	}

	// check for "point" special case
	if (tr.bounds.maxs[0] == 0 && tr.bounds.maxs[1] == 0 && tr.bounds.maxs[2] == 0)
		tr.isPoint = true;

	// general sweeping through world
	TRACE(va("*** trace: headnode=%d ***",headnode),RGB(0.8,0.8,1));
	RecursiveHullCheck (GetHeadnode (headnode), 0, 1, tr.start, tr.end);

	if (tr.trace.fraction == 1)
		tr.trace.endpos = end;
	else
		Lerp (start, end, tr.trace.fraction, tr.trace.endpos);
	if (bspfile.type == map_q1 || bspfile.type == map_hl)
		SetTraceFace1 ();

	trace = tr.trace;
	return;
	unguard;
}


// Handles offseting and rotation of the end points for moving and rotating entities
void CM_TransformedBoxTrace (trace_t &trace, const CVec3 &start, const CVec3 &end, const CBox &bounds,
	int headnode, unsigned contents, const CVec3 &origin, const CVec3 &angles)
{
	guard(CM_TransformedBoxTrace);

	// rotate start and end into the models frame of reference
	bool rotated = (headnode != boxHeadnode && (angles[0] || angles[1] || angles[2]));

	CVec3 start1, end1;
	CAxis axis;
	if (rotated)
	{
		axis.FromEuler (angles);
		// transform start/end to axis (model coordinate system)
		TransformPoint (origin, axis, start, start1);
		TransformPoint (origin, axis, end,   end1);
	}
	else
	{
		VectorSubtract (start, origin, start1);
		VectorSubtract (end,   origin, end1);
	}

	// sweep the box through the model
	CM_BoxTrace (trace, start1, end1, bounds, headnode, contents);

	// transform normal/endpos back to world coordinate system
	if (rotated)
	{
		if (trace.fraction != 1.0f)
			axis.UnTransformVector (trace.plane.normal, trace.plane.normal);
		UnTransformPoint (origin, axis, trace.endpos, trace.endpos);
	}
	else
		trace.endpos.Add (origin);

	unguard;
}


//?? use CCoords instead of origin+axis
void CM_TransformedBoxTrace (trace_t &trace, const CVec3 &start, const CVec3 &end, const CBox &bounds,
	int headnode, unsigned contents, const CVec3 &origin, const CAxis &axis)
{
	guard(CM_TransformedBoxTrace2);

	CVec3 start1, end1;
	// transform start/end to axis (model coordinate system)
	TransformPoint (origin, axis, start, start1);
	TransformPoint (origin, axis, end,   end1);

	// sweep the box through the model
	CM_BoxTrace (trace, start1, end1, bounds, headnode, contents);

	// transform normal/endpos to world coordinate system
	if (trace.fraction != 1.0f)
		axis.UnTransformVector (trace.plane.normal, trace.plane.normal);
	UnTransformPoint (origin, axis, trace.endpos, trace.endpos);

	unguard;
}


//?? implement as method of trace_t
bool CM_CombineTrace (trace_t &trace1, const trace_t &trace2)
{
	if (trace1.allsolid)
		return false;

	bool ret = false;

	if (trace2.allsolid)
	{
		trace1.startsolid = true;
		trace1.allsolid   = true;
		ret = true;
	}

	if (trace1.startsolid)
	{
		trace1.startsolid = true;
		ret = true;
	}

	if (trace2.fraction < trace1.fraction)
	{
		bool solid1 = trace1.startsolid;
		trace1 = trace2;
		trace1.startsolid |= solid1;
		return true;
	}

	return ret;
}


// clip trace to all CMODEL_WALL models
void CM_ClipTraceToModels (trace_t &trace, const CVec3 &start, const CVec3 &end, const CBox &bounds, unsigned contents)
{
	guard(CM_ClipTraceToModels);

	int i;

	CBox traceVolume;
	for (i = 0; i < 3; i++)
	{
		if (start[i] < end[i])
		{
			traceVolume.mins[i] = bounds.mins[i] + start[i];
			traceVolume.maxs[i] = bounds.maxs[i] + end[i];
		}
		else
		{
			traceVolume.mins[i] = bounds.mins[i] + end[i];
			traceVolume.maxs[i] = bounds.maxs[i] + start[i];
		}
	}

	CBspModel *model;
	for (i = 0, model = bspfile.models; i < bspfile.numModels; i++, model++)
	{
		if (!(model->flags & CMODEL_WALL)) continue;
		if (!model->bounds.Intersects (traceVolume)) continue;

		// these entities are not roteted and have origin = {0,0,0}
		trace_t tr;
		CM_BoxTrace (tr, start, end, bounds, model->headnode, contents);
		CM_CombineTrace (trace, tr);
		if (trace.allsolid) return;
	}

	unguard;
}


int CM_PointModelContents (const CVec3 &p)
{
	int contents = 0;

	int i;
	CBspModel *model;
	for (i = 0, model = bspfile.models; i < bspfile.numModels; i++, model++)
	{
		if (!(model->flags & CMODEL_WALL)) continue;
		if (!model->bounds.Contains (p)) continue;

		contents |= CM_FindLeaf (p, model->headnode)->contents;
	}

	return contents;
}


/*-----------------------------------------------------------------------------
	Occlusion test
-----------------------------------------------------------------------------*/

static int trace_numBrushes, trace_maxBrushes;
static int *trace_brushes;


// returns true when start->end line intersects brush
static bool TestBrush (const CVec3 &start, const CVec3 &end, const cbrush_t &brush)
{
	int		i;
	cbrushside_t *side;

	float ef = -1;					// enterfrac
	float lf =  1;					// leavefrac
	for (i = 0, side = &map_brushSides[brush.firstbrushside]; i < brush.numsides; i++, side++)
	{
		float d1, d2;
		SplitTrace (start, end, *side->plane, d1, d2);

		// d1 and d2: 0 -- on plane, <0 -- inside brush plane, >0 -- outside brush plane
		if (d1 >= 0 && d2 >= 0)		// start and end points are outside this plane
			return false;
		if (d1 < 0 && d2 < 0)		// both points inside plane
			continue;

		// check for situation, when line crosses 2 planes outside brush (in this case,
		// line will leave brush before enter -- "inside-out")
		// here: d1 != d1 (have different signs)
		float f = d1 / (d1 - d2);
		if (d1 > d2)
		{
			if (f > ef) ef = f;
		}
		else
		{
			if (f < lf) lf = f;
		}
		if (ef > lf) return false;
	}
	return true;
}


static void RecursiveBrushTest (const CVec3 &point1, const CVec3 &point2, const CBspNode *node)
{
	CVec3 p1 = point1;
	CVec3 p2 = point2;
	while (trace_numBrushes < trace_maxBrushes)					// have buffer space
	{
		if (node->isLeaf)
		{
			//------------------ test leaf ---------------------
			CBspLeaf &leaf = *((CBspLeaf*)node);
			if (!(leaf.contents & CONTENTS_SOLID)) return;		// we are checking only SOLID leafs

			for (int i = 0; i < leaf.numBrushes; i++)
			{
				//-------------- test brush --------------------
				cbrush_t *b = leaf.brushes[i];
				if (b->traceFrame == traceFrame || !(b->contents & CONTENTS_SOLID))
					continue;									// already checked, or non-solid
				b->traceFrame = traceFrame;
				if (TestBrush (p1, p2, *b))
				{
					// remember brush
					trace_brushes[trace_numBrushes++] = b - map_brushes;
					if (trace_numBrushes >= trace_maxBrushes)
						return;									// buffer full
				}
			}
			return;
		}

		const CPlane &plane = *node->plane;

		float d1, d2;
		SplitTrace (p1, p2, plane, d1, d2);

		int s1, s2;
		s1 = IsNegative (d1); s2 = IsNegative (d2);
		if (s1 == s2)
		{
			node = node->children[s1];		// d1 >= 0  => [0], < 0  => [1]
			continue;
		}

		// here: sign(t1) != sign(t2), t1-t2 != 0
		float idist = 1.0f / (d1 - d2);
		float frac1, frac2;
		int side;
#if 0
		if (d1 < d2)						// d1<0, d2>=0; here idist<0
		{
			frac1 = (d1 - DIST_EPSILON) * idist;
			frac2 = (d1 + DIST_EPSILON) * idist;
			side  = 1;
		}
		else								// d1>=0, d2<0
		{
			frac1 = (d1 + DIST_EPSILON) * idist;
			frac2 = (d1 - DIST_EPSILON) * idist;
			side  = 0;
		}
#else
		frac1 = frac2 = d1 * idist;
		float offset2;
		FAbsSign (idist, offset2, side);	// side=1 when t1<t2 (when t1<0 and t2>0)
		offset2 *= DIST_EPSILON;			// offset2 = fabs(idist)*DIST_EPSILON; >0
		frac1 += offset2;
		frac2 -= offset2;
#endif
		frac1 = bound(frac1, 0, 1);
		frac2 = bound(frac2, 0, 1);

		CVec3 mid;
		// move up to the node
		Lerp (p1, p2, frac1, mid);

		RecursiveBrushTest (p1, mid, node->children[side]);

		// go past the node
		Lerp (p1, p2, frac2, p1);

		// do "RecursiveBrushTest (start, end, node.children[side^1]);"
		node = node->children[side^1];
	}
}


// returns number of brushes, intersected with line
int CM_BrushTrace (const CVec3 &start, const CVec3 &end, int *brushes, int maxBrushes)
{
	guard(CM_BrushTrace);

	trace_numBrushes = 0;
	trace_brushes    = brushes;
	trace_maxBrushes = maxBrushes;
	traceFrame++;

	RecursiveBrushTest (start, end, &bspfile.nodes[0]);
	return trace_numBrushes;

	unguard;
}


// gets list of brushes (taken from CM_BrushTrace() call) and removes brushes,
// which are not intersected with line "start-end"
int CM_RefineBrushTrace (const CVec3 &start, const CVec3 &end, int *brushes, int numBrushes)
{
	guard(CM_RefineBrushTrace);

	int *src, *dst;
	src = dst = brushes;
	int newNum = 0;
	for (int i = 0; i < numBrushes; i++)
	{
		int b = *src++;
		if (TestBrush (start, end, map_brushes[b]))
		{
			*dst++ = b;
			newNum++;
		}
	}

	return newNum;
	unguard;
}


/*-----------------------------------------------------------------------------
	PVS support
-----------------------------------------------------------------------------*/

const byte *CM_ClusterPVS (int cluster)
{
	guard(CM_ClusterPVS);

	if (cluster <= -1)
	{
		static byte	pvsrow[MAX_MAP_LEAFS/8];
		return pvsrow;
	}
	if (!bspfile.visInfo)
	{
		// only 1 cluster per whole map
		static const byte noVisPVS[] = { 255, 255, 255, 255 };	// one byte enough ...
		return noVisPVS;
	}

	return bspfile.visInfo + cluster * bspfile.visRowSize;
	unguard;
}


/*-----------------------------------------------------------------------------
	Areaportals
-----------------------------------------------------------------------------*/

static void FloodArea_r (carea_t *area, int floodnum)
{
	if (area->floodvalid == floodvalid)
	{
		if (area->floodnum == floodnum)
			return;
		Com_DropError ("FloodArea_r: reflooded");
	}

	area->floodnum   = floodnum;
	area->floodvalid = floodvalid;
	dAreaPortal_t *p = &map_areaportals[area->firstAreaPortal];
	for (int i = 0; i < area->numAreaPortals; i++, p++)
	{
		if (portalopen[p->portalNum])
			FloodArea_r (&map_areas[p->otherArea], floodnum);
	}
}

static void FloodAreaConnections ()
{
	// all current floods are now invalid
	floodvalid++;
	int floodnum = 0;

	// area 0 is not used
	for (int i = 1; i < numAreas; i++)
	{
		carea_t *area = &map_areas[i];
		if (area->floodvalid == floodvalid)
			continue;		// already flooded into
		floodnum++;
		FloodArea_r (area, floodnum);
	}

}

void CM_SetAreaPortalState (int portalnum, bool open)
{
	if (portalnum > numareaportals)
		Com_DropError ("areaportal > numareaportals");

	portalopen[portalnum] = open;
	FloodAreaConnections ();
}

// used by game dll
bool CM_AreasConnected (int area1, int area2)
{
	if (map_noareas->integer)
		return true;

	if (area1 > numAreas || area2 > numAreas)
		Com_DropError ("area > numAreas");

	if (map_areas[area1].floodnum == map_areas[area2].floodnum)
		return true;
	return false;
}


//?? can move CM_Write... CM_Read... functions to server
/*
=================
CM_WriteAreaBits

Writes a length byte followed by a bit vector of all the areas
that area in the same flood as the area parameter

This is used by the renderer to cull visibility
=================
*/
int CM_WriteAreaBits (byte *buffer, int area)
{
	int bytes = (numAreas + 7) >> 3;

	if (map_noareas->integer)
	{	// for debugging, send everything
		memset (buffer, 255, bytes);
	}
	else
	{
		memset (buffer, 0, bytes);

		int floodnum = map_areas[area].floodnum;
		for (int i = 0; i < numAreas; i++)
		{
			if (map_areas[i].floodnum == floodnum || !area)
				buffer[i >> 3] |= 1 << (i & 7);
		}
	}

	return bytes;
}


// Writes the portal state to a savegame file
void CM_WritePortalState (FILE *f)
{
	fwrite (portalopen, sizeof(portalopen), 1, f);
}


// Reads the portal state from a savegame file and recalculates the area connections
void CM_ReadPortalState (FILE *f)
{
	fread (portalopen, sizeof(portalopen), 1, f);
	FloodAreaConnections ();
}

/*
=============
CM_HeadnodeVisible

Returns true if any leaf under headnode has a cluster that
is potentially visible
=============
*/
// This function used only from sv_ents.c :: SV_BuildClientFrame()
bool CM_HeadnodeVisible (int nodenum, const byte *visbits)
{
	guard(CM_HeadnodeVisible);

	// stack
	CBspNode *stack[MAX_TREE_DEPTH];
	int sptr = 0;

	const CBspNode *node = GetHeadnode (nodenum);
	while (true)
	{
		if (node->isLeaf)
		{
			CBspLeaf *leaf = (CBspLeaf*)node;
			int cluster = leaf->cluster;
			if (cluster == -1 || !(visbits[cluster>>3] & (1<<(cluster&7))))
			{
				if (!sptr) return false;	// whole tree visited
				node = stack[--sptr];
				continue;
			}
			return true;					// found any visible cluster under headnode
		}

		stack[sptr++] = node->children[0];
		node = node->children[1];
	}

	unguard;
}
