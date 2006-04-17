// Autogenerated file: do not modify

struct refExport_t
{
	int		struc_size;
	bool	(*Init) ();
	void	(*Shutdown) (bool complete);
	void	(*LoadNewWorld) (const char *map);
	void	(*FinishLoadingWorld) ();
	CRenderModel*	(*RegisterModel) (const char *name);
	CBasicImage*	(*RegisterSkin) (const char *name, bool force);
	CBasicImage*	(*RegisterPic) (const char *name);
	void	(*SetSky) (const char *name, float rotate, const CVec3 &axis);
	void	(*BeginFrame) (double time);
	void	(*RenderFrame) (refdef_t *fd);
	void	(*EndFrame) ();
	void	(*DrawPic) (int x, int y, const char *pic, int anchor, int color);
	void	(*DrawStretchPic) (int x, int y, int w, int h, const char *name);
	void	(*DrawDetailedPic) (int x, int y, int w, int h, const char *name);
	void	(*DrawChar) (int x, int y, int c, int color);
	void	(*TileClear) (int x, int y, int w, int h, const char *name);
	void	(*Fill) (int x, int y, int w, int h, unsigned rgba);
	void	(*Fill8) (int x, int y, int w, int h, int c);
	void	(*DrawStretchRaw8) (int x, int y, int w, int h, int cols, int rows, byte *data, unsigned *palette);
	void	(*AppActivate) (bool activate);
	void	(*Screenshot) (int flags, const char *name);
	void	(*DrawTextPos) (int x, int y, const char *text, unsigned rgba);
	void	(*DrawTextLeft) (const char *text, unsigned rgba);
	void	(*DrawTextRight) (const char *text, unsigned rgba);
	void	(*DrawText3D) (const CVec3 &pos, const char *text, unsigned rgba);
	float	(*GetClientLight) ();
#if !NO_DEBUG
	void	(*DrawBrush) (const CBrush *brush, const char *label, int num);
#endif
};

#if !IS_RENDERER
#	if !SINGLE_RENDERER

extern refExport_t re;

inline bool RE_Init ()
{
	return re.Init ();
}
inline void RE_Shutdown (bool complete = false)
{
	re.Shutdown (complete);
}
inline void RE_LoadNewWorld (const char *map)
{
	re.LoadNewWorld (map);
}
inline void RE_FinishLoadingWorld ()
{
	re.FinishLoadingWorld ();
}
inline CRenderModel* RE_RegisterModel (const char *name)
{
	return re.RegisterModel (name);
}
inline CBasicImage* RE_RegisterSkin (const char *name, bool force = false)
{
	return re.RegisterSkin (name, force);
}
inline CBasicImage* RE_RegisterPic (const char *name)
{
	return re.RegisterPic (name);
}
inline void RE_SetSky (const char *name, float rotate, const CVec3 &axis)
{
	re.SetSky (name, rotate, axis);
}
inline void RE_BeginFrame (double time)
{
	re.BeginFrame (time);
}
inline void RE_RenderFrame (refdef_t *fd)
{
	re.RenderFrame (fd);
}
inline void RE_EndFrame ()
{
	re.EndFrame ();
}
inline void RE_DrawPic (int x, int y, const char *pic, int anchor = ANCHOR_TOP_LEFT, int color = C_WHITE)
{
	re.DrawPic (x, y, pic, anchor, color);
}
inline void RE_DrawStretchPic (int x, int y, int w, int h, const char *name)
{
	re.DrawStretchPic (x, y, w, h, name);
}
inline void RE_DrawDetailedPic (int x, int y, int w, int h, const char *name)
{
	re.DrawDetailedPic (x, y, w, h, name);
}
inline void RE_DrawChar (int x, int y, int c, int color = C_WHITE)
{
	re.DrawChar (x, y, c, color);
}
inline void RE_TileClear (int x, int y, int w, int h, const char *name)
{
	re.TileClear (x, y, w, h, name);
}
inline void RE_Fill (int x, int y, int w, int h, unsigned rgba)
{
	re.Fill (x, y, w, h, rgba);
}
inline void RE_Fill8 (int x, int y, int w, int h, int c)
{
	re.Fill8 (x, y, w, h, c);
}
inline void RE_DrawStretchRaw8 (int x, int y, int w, int h, int cols, int rows, byte *data, unsigned *palette)
{
	re.DrawStretchRaw8 (x, y, w, h, cols, rows, data, palette);
}
inline void RE_AppActivate (bool activate)
{
	re.AppActivate (activate);
}
inline void RE_Screenshot (int flags, const char *name)
{
	re.Screenshot (flags, name);
}
inline void RE_DrawTextPos (int x, int y, const char *text, unsigned rgba = 0xFFFFFFFF)
{
	re.DrawTextPos (x, y, text, rgba);
}
inline void RE_DrawTextLeft (const char *text, unsigned rgba = 0xFFFFFFFF)
{
	re.DrawTextLeft (text, rgba);
}
inline void RE_DrawTextRight (const char *text, unsigned rgba = 0xFFFFFFFF)
{
	re.DrawTextRight (text, rgba);
}
inline void RE_DrawText3D (const CVec3 &pos, const char *text, unsigned rgba = 0xFFFFFFFF)
{
	re.DrawText3D (pos, text, rgba);
}
inline float RE_GetClientLight ()
{
	return re.GetClientLight ();
}
#if !NO_DEBUG
inline void RE_DrawBrush (const CBrush *brush, const char *label, int num)
{
	re.DrawBrush (brush, label, num);
}
#endif

#	else // SINGLE_RENDERER

namespace OpenGLDrv
{
#include "renderer.h"
};

#define RE_Init	OpenGLDrv::Init
#define RE_Shutdown	OpenGLDrv::Shutdown
#define RE_LoadNewWorld	OpenGLDrv::LoadNewWorld
#define RE_FinishLoadingWorld	OpenGLDrv::FinishLoadingWorld
#define RE_RegisterModel	OpenGLDrv::RegisterModel
#define RE_RegisterSkin	OpenGLDrv::RegisterSkin
#define RE_RegisterPic	OpenGLDrv::RegisterPic
#define RE_SetSky	OpenGLDrv::SetSky
#define RE_BeginFrame	OpenGLDrv::BeginFrame
#define RE_RenderFrame	OpenGLDrv::RenderFrame
#define RE_EndFrame	OpenGLDrv::EndFrame
#define RE_DrawPic	OpenGLDrv::DrawPic
#define RE_DrawStretchPic	OpenGLDrv::DrawStretchPic
#define RE_DrawDetailedPic	OpenGLDrv::DrawDetailedPic
#define RE_DrawChar	OpenGLDrv::DrawChar
#define RE_TileClear	OpenGLDrv::TileClear
#define RE_Fill	OpenGLDrv::Fill
#define RE_Fill8	OpenGLDrv::Fill8
#define RE_DrawStretchRaw8	OpenGLDrv::DrawStretchRaw8
#define RE_AppActivate	OpenGLDrv::AppActivate
#define RE_Screenshot	OpenGLDrv::Screenshot
#define RE_DrawTextPos	OpenGLDrv::DrawTextPos
#define RE_DrawTextLeft	OpenGLDrv::DrawTextLeft
#define RE_DrawTextRight	OpenGLDrv::DrawTextRight
#define RE_DrawText3D	OpenGLDrv::DrawText3D
#define RE_GetClientLight	OpenGLDrv::GetClientLight
#if !NO_DEBUG
#define RE_DrawBrush	OpenGLDrv::DrawBrush
#endif

#	endif // SINGLE_RENDERER
#endif // IS_RENDERER

