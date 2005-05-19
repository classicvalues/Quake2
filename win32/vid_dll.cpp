#include "winquake.h"
#include "resource.h"

#include "../client/client.h"


#define DEFAULT_RENDERER	"gl"		// for vid_ref cvar

cvar_t *win_noalttab;
cvar_t *win_priorityBoost;


static UINT MSH_MOUSEWHEEL = 0xDEAD0000; // initialized to something unused

// Console variables that we need to access from this module
static cvar_t	*vid_xpos;				// X coordinate of window position
static cvar_t	*vid_ypos;				// Y coordinate of window position

#ifndef SINGLE_RENDERER

static cvar_t	*vid_ref;
static HINSTANCE refLibrary;			// handle to renderer DLL
// Imports from main engine for renderer DLL
#include "../client/engine_exp.h"

// Structure containing functions exported from renderer DLL
refExport_t	re;

#endif // SINGLE_RENDERER

// Global variables used internally by this module
viddef_t viddef;						// global video state; used by other modules
static bool refActive = false;

static bool needRestart;

HWND	cl_hwnd;						// main window handle

static bool alttab_disabled;

/*
** WIN32 helper functions
*/
static bool s_win95;

static void DisableAltTab (bool enable)
{
	if (alttab_disabled == enable) return;
	alttab_disabled = enable;

	if (s_win95)
	{
		BOOL old;
		SystemParametersInfo (SPI_SCREENSAVERRUNNING, enable, &old, 0);
	}
	else
	{
		if (enable)
		{
			RegisterHotKey (0, 0, 1/*MOD_ALT*/, VK_TAB);	// MOD_ALT is redefined, so use winuser.h const directly
//			RegisterHotKey (0, 1, 1/*MOD_ALT*/, VK_RETURN);	// disable Alt+Enter too (do not allow fullscreen noggle)
			// really, should disable <Win>, <Ctrl+Esc>, <Alt+Esc> key combinations too; see Q226359 in MSDN
		}
		else
		{
			UnregisterHotKey (0, 0);
//			UnregisterHotKey (0, 1);
		}
	}
}

static void SetHighPriority (bool enable)
{
	HANDLE hProcess = GetCurrentProcess ();
	SetPriorityClass (hProcess, enable ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
}


//==========================================================================


#define UNK		255

static const byte vkToKey[256] = {
//			08			19			2A			3B			4C			5D			6E			7F
/*00*/		0,			0,			0,			0,			0,	 		UNK,		UNK,		UNK,
/*08*/		K_BACKSPACE,K_TAB,		UNK,		UNK,		UNK,		K_ENTER,	UNK,		UNK,
/*10*/		K_SHIFT,	K_CTRL,		K_ALT,		K_PAUSE,	K_CAPSLOCK,	UNK,		UNK,		UNK,
/*18*/		UNK,		UNK,		UNK,		K_ESCAPE,	UNK,		UNK,		UNK,		UNK,
/*20*/		K_SPACE,	K_PGUP,		K_PGDN,		K_END,		K_HOME,		K_LEFTARROW,K_UPARROW,	K_RIGHTARROW,
/*28*/		K_DOWNARROW,0,			0,			0,			K_PRINTSCRN,K_INS,		K_DEL,		0,
/*30*/		'0',		'1',		'2',		'3',		'4',		'5',		'6',		'7',
/*38*/		'8',		'9',		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*40*/		UNK,		'a',		'b',		'c',		'd',		'e',		'f',		'g',
/*48*/		'h',		'i',		'j',		'k',		'l',		'm',		'n',		'o',
/*50*/		'p',		'q',		'r',		's',		't',		'u',		'v',		'w',
/*58*/		'x',		'y',		'z',		UNK,		UNK,		UNK,		UNK,		UNK,
/*60*/		K_KP_INS,	K_KP_END,	K_KP_DOWNARROW,K_KP_PGDN,K_KP_LEFTARROW,K_KP_5,	K_KP_RIGHTARROW,K_KP_HOME,
/*68*/		K_KP_UPARROW,K_KP_PGUP,	K_KP_STAR,	K_KP_PLUS,	0,			K_KP_MINUS,	K_KP_DEL,	K_KP_SLASH,
/*70*/		K_F1,		K_F2,		K_F3,		K_F4,		K_F5,		K_F6,		K_F7,		K_F8,
/*78*/		K_F9,		K_F10,		K_F11,		K_F12,		K_F13,		K_F14,		K_F15,		K_F16,
/*80*/		K_F17,		K_F18,		K_F19,		K_F20,		K_F21,		K_F22,		K_F23,		K_F24,
/*88*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*90*/		K_NUMLOCK,	K_SCRLOCK,	UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*98*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*A0*/		K_SHIFT,	K_SHIFT,	K_CTRL,		K_CTRL,		K_ALT,		K_ALT,		UNK,		UNK,
/*A8*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*B0*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*B8*/		UNK,		UNK,		';',		'=',		',',		'-',		'.',		'/',
/*C0*/		'`',		0,			0,			0,			0,			0,			0,			0,
/*C8*/		0,			0,			0,			0,			0,			0,			0,			0,
/*D0*/		0,			0,			0,			0,			0,			0,			0,			0,
/*D8*/		UNK,		UNK,		UNK,		'[',		'\\',		']',		'\'',		UNK,
/*E0*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*E8*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*F0*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,
/*F8*/		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		UNK,		0
};


static int MapKey (int vkCode, bool extended)
{
	if (!extended)
	{

		switch (vkCode)
		{
		case VK_PRIOR:	return K_KP_PGUP;
		case VK_NEXT:	return K_KP_PGDN;
		case VK_END:	return K_KP_END;
		case VK_HOME:	return K_KP_HOME;
		case VK_LEFT:	return K_KP_LEFTARROW;
		case VK_UP:		return K_KP_UPARROW;
		case VK_RIGHT:	return K_KP_RIGHTARROW;
		case VK_DOWN:	return K_KP_DOWNARROW;
		case VK_CLEAR:	return K_KP_5;
		case VK_DELETE:	return K_KP_DEL;
		case VK_INSERT: return K_KP_INS;
		}
	}
	else
	{
		switch (vkCode)
		{
		case VK_RETURN:	return K_KP_ENTER;
		case VK_CONTROL:return K_RCTRL;
		case VK_MENU:	return K_RALT;
		}
	}
	int key = vkToKey[vkCode];
	if (key == UNK) key = vkCode + 256;
	return key;
}


static void AppActivate (bool active, bool minimized)
{
//	Com_Printf("act:%d min:%d\n", active, minimized);//!!
	MinimizedApp = minimized;
	if (ActiveApp != active)
	{
		ActiveApp = active;

		Key_ClearStates ();
		IN_Activate (active);
		CDAudio_Activate (active);
		S_Activate (active);
		if (refActive) RE_AppActivate (active);

		if (win_noalttab->integer)		DisableAltTab (active);
		if (win_priorityBoost->integer)	SetHighPriority (active);
	}
}


/*-----------------------------------------------------------------------------
	Message processing
-----------------------------------------------------------------------------*/

static MSGHOOK_FUNC hooks[8];

void AddMsgHook (MSGHOOK_FUNC func)
{
	int idx = -1;
	for (int i = 0; i < ARRAY_COUNT(hooks); i++)
	{
		if (hooks[i] == func) return;	// already hooked
		if (!hooks[i] && idx < 0) idx = i;
	}
	if (idx < 0) Com_FatalError ("max msg hooks");
	hooks[idx] = func;
}

void RemoveMsgHook (MSGHOOK_FUNC func)
{
	for (int i = 0; i < ARRAY_COUNT(hooks); i++)
	{
		if (hooks[i] == func)
		{
			hooks[i] = NULL;
			return;
		}
	}
	Com_DPrintf ("RemoveMsgHook: hook not installed\n");
}


static LONG WINAPI MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	guard(MainWndProc);

//	DebugPrintf("msg=%X w=%X l=%X\n",uMsg,wParam,lParam);//!!
	if (uMsg == MSH_MOUSEWHEEL) uMsg = WM_MOUSEWHEEL;

	guard(callHook);
	for (int i = 0; i < ARRAY_COUNT(hooks); i++)
	{
		if (hooks[i])
		{
			if (hooks[i](uMsg, wParam, lParam)) return 0;
		}
	}
	unguard;

	switch (uMsg)
	{
	case WM_HOTKEY:
		return 0;

	case WM_SIZE:
		if (FullscreenApp)
		{
			static DEVMODE dm;
			static bool desktopMode = false;

//			Com_Printf("WM_SIZE: %d (act=%d min=%d)\n",wParam, ActiveApp, MinimizedApp);//!!
			if (wParam == SIZE_MINIMIZED && !desktopMode)
			{
				Com_DPrintf ("Setting desktop resolution\n");
				// save screen parameters
				HDC dc = GetDC (NULL);
				dm.dmSize = sizeof(dm);
				dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
				dm.dmPelsWidth = GetDeviceCaps (dc, HORZRES);
				dm.dmPelsHeight = GetDeviceCaps (dc, VERTRES);
				dm.dmBitsPerPel = GetDeviceCaps (dc, BITSPIXEL);
				// restore mode
				ChangeDisplaySettings (NULL, 0);
				desktopMode = true;
			}
			else if (wParam == SIZE_RESTORED && MinimizedApp && desktopMode)
			{
				Com_DPrintf ("Setting game resolution\n");
				ChangeDisplaySettings (&dm, CDS_FULLSCREEN);
				SetWindowPos (hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE);
				SetForegroundWindow (hWnd);
				desktopMode = false;
			}
		}
		break;

	case WM_ACTIVATE:
		{
			bool active = LOWORD(wParam) != WA_INACTIVE;
			bool minimized = HIWORD(wParam) != 0;
//			Com_Printf("WM_ACTIVATE: a=%d m=%d (act=%d min=%d)\n",active, minimized, ActiveApp, MinimizedApp);//!!
			AppActivate (active, minimized);
		}
		break;

	case WM_MOVE:
		if (!FullscreenApp)
		{
			int xPos = (short) LOWORD(lParam);    // horizontal position
			int yPos = (short) HIWORD(lParam);    // vertical position

			RECT r;
			r.left   = 0;
			r.top    = 0;
			r.right  = 1;
			r.bottom = 1;

			int style = GetWindowLong (hWnd, GWL_STYLE);
			AdjustWindowRect (&r, style, FALSE);

			Cvar_SetInteger ("vid_xpos", xPos + r.left);
			Cvar_SetInteger ("vid_ypos", yPos + r.top);
			vid_xpos->modified = false;
			vid_ypos->modified = false;
			if (ActiveApp) IN_Activate (true);
		}
		break;

	case WM_SYSCOMMAND:
		switch (wParam)
		{
			case SC_SCREENSAVE:		// disable screensaver
			case SC_MONITORPOWER:	// disable monitor power managemment
			case SC_KEYMENU:		// disable activating window menu with keyboard
				return 0;
		}
		break;

	case WM_SYSKEYDOWN:
		if (wParam == VK_RETURN)	// Alt+Enter
		{
			Cvar_SetInteger ("r_fullscreen", !FullscreenApp);
			return 0;
		}
		// fall through
	case WM_KEYDOWN:
		{
			int k = MapKey (wParam, (lParam >> 24) & 1);
			if (k) Key_Event (k, true);
		}
		return 0;

	case WM_SYSKEYUP:
	case WM_KEYUP:
		{
			int k = MapKey (wParam, (lParam >> 24) & 1);
			if (k) Key_Event (k, false);
		}
		return 0;

	case WM_POWERBROADCAST:
		if (wParam == PBT_APMSUSPEND)	// will minimize window when in fullscreen mode
			RE_AppActivate (false);
		//?? can also disconnect all remote (not local) clients from server (if one)
		break;

	case WM_CLOSE:
		Com_Quit ();
		break;							// should not return here

	case WM_ERASEBKGND:
		return 1;
	}

	return DefWindowProc (hWnd, uMsg, wParam, lParam);

	unguardf(("msg=%X", uMsg));
}


/*-----------------------------------------------------------------------------
	Window creation/destroying
-----------------------------------------------------------------------------*/

void *Vid_CreateWindow (int width, int height, bool fullscreen)
{
	WNDCLASS wc;
	RECT	r;
	int		stylebits;
	int		x, y, w, h;
	int		exstyle;

	guard(Vid_CreateWindow);

	if (fullscreen)
	{
		exstyle = WS_EX_TOPMOST;
		stylebits = WS_POPUP|WS_SYSMENU;
	}
	else
	{
		exstyle = 0;
		stylebits = WS_SYSMENU|WS_CAPTION;
	}

	// if size=0 -- invisible (fake) window
//	if (width || height)		// if enable this, window will be created without taskbar button!
//		stylebits |= WS_VISIBLE;

	r.left = 0;
	r.top = 0;
	r.right  = width;
	r.bottom = height;

	AdjustWindowRect (&r, stylebits, FALSE);

	w = r.right - r.left;
	h = r.bottom - r.top;

	FullscreenApp = fullscreen;
	if (fullscreen)
	{
		x = 0;
		y = 0;
	}
	else
	{
		x = vid_xpos->integer;
		y = vid_ypos->integer;
	}

	if (cl_hwnd)
	{
		SetWindowLong (cl_hwnd, GWL_STYLE, stylebits);
//		SetWindowLong (cl_hwnd, GWL_EXSTYLE, exstyle); -- require to change WS_EX_TOPMOST only, do it with SetWindowPos() (better effect)
		ShowWindow (cl_hwnd, SW_SHOW);
		SetWindowPos (cl_hwnd, fullscreen ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, w, h, 0);
	}
	else
	{
		// Register the frame class
		wc.style			= 0;
		wc.lpfnWndProc		= MainWndProc;
		wc.cbClsExtra		= 0;
		wc.cbWndExtra		= 0;
		wc.hInstance		= global_hInstance;
		wc.hIcon			= LoadIcon (global_hInstance, MAKEINTRESOURCE(IDI_ICON1));
		wc.hCursor			= LoadCursor (NULL,IDC_ARROW);
//		wc.hbrBackground	= (HBRUSH) COLOR_GRAYTEXT;
		wc.hbrBackground	= NULL;
		wc.lpszMenuName 	= NULL;
		wc.lpszClassName	= APPNAME;
		if (!RegisterClass (&wc)) Com_FatalError ("Couldn't register window class");

		cl_hwnd = CreateWindowEx (exstyle, APPNAME, APPNAME, stylebits, x, y, w, h, NULL, NULL, global_hInstance, NULL);
		if (!cl_hwnd) Com_FatalError ("Couldn't create window");

		if (width || height) ShowWindow (cl_hwnd, SW_SHOW);
		// let the sound and input subsystems know about the new window
		cl.force_refdef = true;		// can't use a paused refdef
		in_needRestart = true;
	}

	viddef.width  = width;
	viddef.height = height;

	UpdateWindow (cl_hwnd);
	SetForegroundWindow (cl_hwnd);
	SetFocus (cl_hwnd);		//?? capture keyboard; is it really necessary ?

	return cl_hwnd;

	unguard;
}


void Vid_DestroyWindow (bool force)
{
//force = true; //???
	if (!force)	//?? add cvar "win_singleWindow"
	{
//		ShowWindow (cl_hwnd, SW_HIDE);
		return;
	}

	Com_DPrintf ("...destroying window\n");
	if (cl_hwnd)
	{
//		ShowWindow (cl_hwnd, SW_HIDE);	-- this will force to CDS(0,0) when vid_restart, because Activate(0)->Minimize()->WM_SIZE
		DestroyWindow (cl_hwnd);
		cl_hwnd = 0;
	}

	UnregisterClass (APPNAME, global_hInstance);
}


/*-------------------------------------------*/

void Vid_Restart (void)
{
	needRestart = true;
}


/*------------- Vid_GetModeInfo -------------*/

static const struct {
	short width, height;
} vid_modes[] = {
	{320,	240},
	{400,	300},
	{512,	384},
	{640,	480},
	{800,	600},
	{960,	720},
	{1024,	768},
	{1152,	864},
	{1280,	960},
	{1600,	1200},
	{2048,	1536}
};


bool Vid_GetModeInfo (int *width, int *height, int mode)
{
	if (mode < 0 || mode >= ARRAY_COUNT(vid_modes))
		return false;

	*width  = vid_modes[mode].width;
	*height = vid_modes[mode].height;

	return true;
}


static void Vid_UpdateWindowPosAndSize (int x, int y)
{
	RECT	r;
	r.left   = 0;
	r.top    = 0;
	r.right  = viddef.width;
	r.bottom = viddef.height;

	int style = GetWindowLong (cl_hwnd, GWL_STYLE);
	AdjustWindowRect (&r, style, FALSE);

	int w = r.right - r.left;
	int h = r.bottom - r.top;

	MoveWindow (cl_hwnd, vid_xpos->integer, vid_ypos->integer, w, h, TRUE);
}



/*-------- Dummy functions for console-only mode ---------*/

//!! not works with SINGLE_RENDERER
/*!! not works now; can replace gl_console_only with cl_console_only, switch on-fly (w/o vid_restart),
 *    + remove RE_DrawConChar() -- use RE_DrawChar(); remove renderer.flags; console_only will differ
 *    from normal mode by visualization; all models/textures/sounds will be loaded into memory, but
 *    not used; when switch back to normal mode -- used as always; needs special path for console_only
 *    in cl_scrn.cpp -- display console and exit; can remove check for console_only from most other
 *    places (check qmenu.cpp!)
 */
#if 0
static void	D_RenderFrame (refdef_t *fd) {}
static void	D_BeginRegistration (const char *map) {}
static CRenderModel *D_RegisterModel (const char *name) { return NULL; }
static CBasicImage *D_RegisterSkin (const char *name) { return NULL; }
static CBasicImage *D_FindPic (const char *name) { return NULL; }
static void D_SetSky (const char *name, float rotate, vec3_t axis) {}
static void	D_EndRegistration (void) {}

static void D_Screenshot (int flags, const char *name)
{
	Com_WPrintf ("Screenshots are unsupported by this renderer\n");
}

static void	D_DrawPic (int x, int y, const char *name, int anchor, int color) {}
static void	D_DrawStretchPic (int x, int y, int w, int h, const char *pic) {}
static void	D_DrawChar (int x, int y, int c, int color) {}
static void	D_TileClear (int x, int y, int w, int h, const char *name) {}
static void	D_Fill8 (int x, int y, int w, int h, int c) {}
static void D_Fill (int x, int y, int w, int h, unsigned rgba) {}
static void	D_DrawTextPos (int x, int y, const char *text, unsigned rgba) {}
static void	D_DrawTextSide (const char *text, unsigned rgba) {}
static void	D_DrawStretchRaw8 (int x, int y, int w, int h, int cols, int rows, byte *data, unsigned *palette) {}
static float D_GetClientLight (void) { return 0; }		// normal value is 150
#endif


static void FreeRenderer (void)
{
	refActive = false;

#ifndef SINGLE_RENDERER
	if (refLibrary)		// if false - statically linked
	{
		if (!FreeLibrary (refLibrary))
			Com_FatalError ("Renderer FreeLibrary() failed");
		refLibrary = NULL;
	}
	memset (&re, 0, sizeof(re));
#endif
}


#if defined(STATIC_BUILD) && !defined(SINGLE_RENDERER)
// externs
namespace OpenGLDrv {
	extern const refExport_t re;
}
#endif

#ifndef SINGLE_RENDERER
static bool LoadRenderer (const char *name)
#else
static bool LoadRenderer (void)
#endif
{
	guard(LoadRenderer);

	if (refActive)
	{
		RE_Shutdown ();
		FreeRenderer ();
	}

#ifndef SINGLE_RENDERER

	Com_Printf ("Loading %s\n", name);

#ifdef STATIC_BUILD
	refLibrary = NULL;

	if (!strcmp (name, "gl"))
		re = OpenGLDrv::re;
	else
#endif
	{
		char	dllName[MAX_OSPATH];
		appSprintf (ARRAY_ARG(dllName), "ref_%s.dll", name);
		if (!(refLibrary = LoadLibrary (dllName)))
		{
			Com_WPrintf ("LoadLibrary(\"%s\") failed\n", dllName);
			return false;
		}
		CreateDynRenderer_t pCreateRenderer;
		if (!(pCreateRenderer = (CreateDynRenderer_t) GetProcAddress (refLibrary, "CreateRenderer")))
		{
			Com_WPrintf ("GetProcAddress() failed on %s\n", dllName);
			FreeRenderer ();
			return false;
		}

		re.struc_size = sizeof(refExport_t);
		if (!pCreateRenderer (&ri, &re))
		{
			Com_WPrintf ("%s has incompatible renderer\n", dllName);
			FreeRenderer ();
			return false;
		}
	}
#endif // SINGLE_RENDERER

	if (!RE_Init ())
	{
		RE_Shutdown ();
		FreeRenderer ();
		return false;
	}

#if 0
	if (RE_GetCaps() & REF_CONSOLE_ONLY)
	{
		re.RenderFrame =	D_RenderFrame;
		re.Screenshot =		D_Screenshot;
		re.BeginRegistration = D_BeginRegistration;
		re.RegisterModel =	D_RegisterModel;
		re.RegisterSkin =	D_RegisterSkin;
		re.RegisterPic =	D_FindPic;
		re.SetSky =			D_SetSky;
		re.EndRegistration = D_EndRegistration;

		re.DrawPic =		D_DrawPic;
		re.DrawStretchPic =	D_DrawStretchPic;
		re.DrawDetailedPic = D_DrawStretchPic;
		re.DrawChar =		D_DrawChar;
		re.TileClear =		D_TileClear;
		re.Fill8 =			D_Fill8;
		re.Fill =			D_Fill;
		re.DrawStretchRaw8 = D_DrawStretchRaw8;

		re.DrawTextPos =	D_DrawTextPos;
		re.DrawTextLeft =	D_DrawTextSide;
		re.DrawTextRight =	D_DrawTextSide;

		re.GetClientLight = D_GetClientLight;
	}
#endif

	Com_Printf ("------------------------------------\n");
	refActive = true;

	return true;

	unguard;
}


void Vid_Tick (void)
{
	if (win_noalttab->modified)
	{
		DisableAltTab (win_noalttab->integer != 0);
		win_noalttab->modified = false;
	}

	if (win_priorityBoost->modified)
	{
		SetHighPriority (win_priorityBoost->integer != 0);
		win_priorityBoost->modified = false;
	}

#ifndef SINGLE_RENDERER
	if (vid_ref->modified)
	{
		needRestart = true;
		vid_ref->modified = false;
	}
#endif

	if (needRestart)
	{
		// refresh has changed
		cl.force_refdef = true;		// can't use a paused refdef
		S_StopAllSounds_f ();
		cl.refresh_prepped = false;

#ifndef SINGLE_RENDERER
		static char lastRenderer[MAX_QPATH];

		bool loaded = LoadRenderer (vid_ref->string);
		if (!loaded && lastRenderer[0])
		{
			if (LoadRenderer (lastRenderer))
			{
				Cvar_Set ("vid_ref", lastRenderer);
				vid_ref->modified = false;
				loaded = true;
			}
		}
		if (!loaded) Com_FatalError ("Couldn't load renderer");
		strcpy (lastRenderer, vid_ref->string);
#else
		if (!LoadRenderer ()) Com_FatalError ("Couldn't load renderer");
#endif
		needRestart = false;
	}

	// update our window position
	if (vid_xpos->modified || vid_ypos->modified)
	{
		if (!FullscreenApp)
			Vid_UpdateWindowPosAndSize (vid_xpos->integer, vid_ypos->integer);

		vid_xpos->modified = false;
		vid_ypos->modified = false;
	}
}


void Vid_Init (void)
{
CVAR_BEGIN(vars)
#ifndef SINGLE_RENDERER
	CVAR_FULL(&vid_ref, "vid_ref", DEFAULT_RENDERER, CVAR_ARCHIVE),
#endif
	CVAR_VAR(vid_xpos, 0, CVAR_ARCHIVE),
	CVAR_VAR(vid_ypos, 0, CVAR_ARCHIVE),
	CVAR_VAR(win_noalttab, 0, CVAR_ARCHIVE),
	CVAR_VAR(win_priorityBoost, 0, CVAR_ARCHIVE)
CVAR_END

	guard(Vid_Init);

	needRestart = true;		// should init renderer on startup
	Cvar_GetVars (ARRAY_ARG(vars));
	InitRendererVars ();

	// add some console commands that we want to handle
	RegisterCommand ("vid_restart", Vid_Restart);

	OSVERSIONINFO vinfo;
	vinfo.dwOSVersionInfoSize = sizeof(vinfo);
	GetVersionEx (&vinfo);
	if (vinfo.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
	{
		s_win95 = true;
		MSH_MOUSEWHEEL = RegisterWindowMessage ("MSWHEEL_ROLLMSG");
	}

#if 0
	// this is a gross hack but necessary to clamp the mode for 3Dfx
	{
		cvar_t *gl_driver = Cvar_Get ("gl_driver", "opengl32", 0);
		cvar_t *gl_mode = Cvar_Get ("gl_mode", "3", 0);

		if (stricmp (gl_driver->string, "3dfxgl") == 0)
		{
			Cvar_SetInteger ("gl_mode", 3);
			viddef.width  = 640;
			viddef.height = 480;
		}
	}
#endif
	// disable the 3Dfx splash screen
	putenv ("FX_GLIDE_NO_SPLASH=0");

	// create invisible (fake) window to capture Win32 focus
	Vid_CreateWindow (0, 0, false);

	// start the graphics mode and load refresh DLL
	Vid_Tick ();

	unguard;
}

/*
============
Vid_Shutdown
============
*/
void Vid_Shutdown (void)
{
	if (refActive)
	{
		// perform shutdown
		RE_Shutdown (true);
		Vid_DestroyWindow (true);
		FreeRenderer ();
	}
}
