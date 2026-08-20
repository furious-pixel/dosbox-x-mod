
// Tell Mac OS X to shut up about deprecated OpenGL calls
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif

#include <sys/types.h>
#include <assert.h>
#include <math.h>
extern "C" {
#include "ppscale.h"
#include "ppscale.c"
}
#include "control.h"
#include "dosbox.h"
#include "dosbox_python.h"
#include "logging.h"
#include "menudef.h"
#include "mod.h"
#include "../ints/int10.h"
#include <output/output_opengl.h>
#include <output/output_tools.h>
#include <output/output_tools_xbrz.h>
#include <output/ra_glsl.h>

#include <algorithm>

#include "sdlmain.h"
#include "render.h"

using namespace std;

extern Bitu frames;
extern Bitu userResizeWindowWidth;
extern Bitu userResizeWindowHeight;
extern Bitu currentWindowWidth;
extern Bitu currentWindowHeight;

bool setSizeButNotResize();

#if C_OPENGL
PFNGLGENBUFFERSARBPROC glGenBuffersARB = NULL;
PFNGLBINDBUFFERARBPROC glBindBufferARB = NULL;
PFNGLDELETEBUFFERSARBPROC glDeleteBuffersARB = NULL;
PFNGLBUFFERDATAARBPROC glBufferDataARB = NULL;
PFNGLMAPBUFFERARBPROC glMapBufferARB = NULL;
PFNGLUNMAPBUFFERARBPROC glUnmapBufferARB = NULL;
PFNGLACTIVETEXTUREPROC dosbox_glActiveTexture = NULL;
PFNGLBINDFRAMEBUFFERPROC dosbox_glBindFramebuffer = NULL;
PFNGLBINDVERTEXARRAYPROC dosbox_glBindVertexArray = NULL;

/* Apple defines these functions in their GL header (as core functions)
 * so we can't use their names as function pointers. We can't link
 * directly as some platforms may not have them. So they get their own
 * namespace here to keep the official names but avoid collisions.
 */
namespace gl2 {
PFNGLATTACHSHADERPROC glAttachShader = NULL;
PFNGLCOMPILESHADERPROC glCompileShader = NULL;
PFNGLCREATEPROGRAMPROC glCreateProgram = NULL;
PFNGLCREATESHADERPROC glCreateShader = NULL;
PFNGLDELETEPROGRAMPROC glDeleteProgram = NULL;
PFNGLDELETESHADERPROC glDeleteShader = NULL;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = NULL;
PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation = NULL;
PFNGLGETPROGRAMIVPROC glGetProgramiv = NULL;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = NULL;
PFNGLGETSHADERIVPROC glGetShaderiv = NULL;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = NULL;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = NULL;
PFNGLLINKPROGRAMPROC glLinkProgram = NULL;
PFNGLSHADERSOURCEPROC_NP glShaderSource = NULL;
PFNGLUNIFORM2FPROC glUniform2f = NULL;
PFNGLUNIFORM1IPROC glUniform1i = NULL;
PFNGLUSEPROGRAMPROC glUseProgram = NULL;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = NULL;
}

/* "using" is meant to hide identical names declared in outer scope
 * but is unreliable, so just redefine instead.
 */
#define glAttachShader            gl2::glAttachShader
#define glCompileShader           gl2::glCompileShader
#define glCreateProgram           gl2::glCreateProgram
#define glCreateShader            gl2::glCreateShader
#define glDeleteProgram           gl2::glDeleteProgram
#define glDeleteShader            gl2::glDeleteShader
#define glEnableVertexAttribArray gl2::glEnableVertexAttribArray
#define glGetAttribLocation       gl2::glGetAttribLocation
#define glGetProgramiv            gl2::glGetProgramiv
#define glGetProgramInfoLog       gl2::glGetProgramInfoLog
#define glGetShaderiv             gl2::glGetShaderiv
#define glGetShaderInfoLog        gl2::glGetShaderInfoLog
#define glGetUniformLocation      gl2::glGetUniformLocation
#define glLinkProgram             gl2::glLinkProgram
#define glShaderSource            gl2::glShaderSource
#define glUniform2f               gl2::glUniform2f
#define glUniform1i               gl2::glUniform1i
#define glUseProgram              gl2::glUseProgram
#define glVertexAttribPointer     gl2::glVertexAttribPointer

#if C_OPENGL && DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
extern unsigned int SDLDrawGenFontTextureWidth;
extern unsigned int SDLDrawGenFontTextureHeight;
extern GLuint SDLDrawGenFontTexture, SDLDrawGenDBCSFontTexture;
extern bool SDLDrawGenFontTextureInit;
#endif
extern int aspect_ratio_x, aspect_ratio_y;
extern int initgl, lastcp;
extern bool font_16_init;

SDL_OpenGL sdl_opengl = {0};
static ModRenderViewMode mod_render_view_mode = MOD_RENDER_VIEW_GAME_ONLY;
static ModRenderViewMode mod_render_single_view_mode = MOD_RENDER_VIEW_MOD_ONLY;
static bool mod_render_view_initialized = false;
static bool mod_render_saved_window_size_valid = false;
static Bitu mod_render_saved_window_width = 0;
static Bitu mod_render_saved_window_height = 0;
static bool mod_render_was_active = false;

struct ModPresentationMetricsState {
    uint64_t latest_ready_sequence = 0;
    uint64_t last_presented_ready_sequence = 0;
    uint64_t native_count = 0;
    uint64_t mod_count = 0;
    uint64_t presentation_count = 0;
    uint64_t latency_sample_count = 0;
    double latency_sum_ms = 0.0;
    double latency_max_ms = 0.0;
    uint32_t latest_ready_ticks = 0;
    uint32_t interval_start_ticks = 0;
    OpenGLModPresentationMetrics snapshot = {};
};

static ModPresentationMetricsState mod_presentation_metrics = {};

static ModRenderViewMode GetConfiguredModRenderStartView(void)
{
    if (!DOSBoxPython_OpenGLRendererAvailable())
        return MOD_RENDER_VIEW_GAME_ONLY;

    const Section_prop *render_section = static_cast<const Section_prop *>(
            control->GetSection("render"));
    if (!render_section)
        return MOD_RENDER_VIEW_SIDE_BY_SIDE;

    const char *configured =
            render_section->Get_string("mod renderer start view");
    if (configured && !strcmp(configured, "mod-only"))
        return MOD_RENDER_VIEW_MOD_ONLY;
    if (configured && !strcmp(configured, "game-only"))
        return MOD_RENDER_VIEW_GAME_ONLY;
    return MOD_RENDER_VIEW_SIDE_BY_SIDE;
}

// One normal inactive present clears the current back buffer before swapping.
// Two post-swap clears drain the other buffers when drivers are effectively
// triple-buffering behind SDL's back.
static constexpr int MOD_RENDER_INACTIVE_POST_SWAP_CLEARS = 2;

// CRT presets are for full native presentation (game-only). Side-by-side is
// an A/B against the enhanced renderer; do not run the CRT on the native pane
// there. If this nearest blit is not a fair comparison, replace the native
// SBS path with openglpp instead of turning this back on.
static constexpr bool kApplyNativeCrtInSideBySide = false;

static bool NativeCrtLetterboxes(void)
{
    return RA_GLSL_HasPreset();
}

struct GLViewport {
    GLint x = 0;
    GLint y = 0;
    GLsizei w = 0;
    GLsizei h = 0;
};

struct OpenGLPresentationLayout {
    uint32_t backbuffer_width = 0;
    uint32_t backbuffer_height = 0;
    GLViewport natural_game = {};
    GLViewport game = {};
    GLViewport mod = {};
};

static Bitu ClampOpenGLWindowDimension(const Bitu value)
{
    return std::min<Bitu>(std::max<Bitu>(1u, value), 65535u);
}

static bool GetConfiguredWindowSize(Bitu *width, Bitu *height)
{
    if (sdl.desktop.window.width == 0 || sdl.desktop.window.height == 0)
        return false;

    if (width)
        *width = (Bitu)sdl.desktop.window.width;
    if (height)
        *height = (Bitu)sdl.desktop.window.height;
    return true;
}

static bool ParseComparisonPaneSize(const char *configured,
                                    Bitu *width,
                                    Bitu *height)
{
    if (!configured || !*configured || !strcmp(configured, "auto"))
        return false;

    char *width_end = NULL;
    const unsigned long parsed_width = strtoul(configured, &width_end, 10);
    if (width_end == configured || (*width_end != 'x' && *width_end != 'X'))
        return false;

    char *height_end = NULL;
    const unsigned long parsed_height = strtoul(width_end + 1, &height_end, 10);
    if (height_end == width_end + 1 || *height_end != '\0' ||
        parsed_width == 0 || parsed_width > 32767ul ||
        parsed_height == 0 || parsed_height > 65535ul) {
        return false;
    }

    if (width)
        *width = (Bitu)parsed_width;
    if (height)
        *height = (Bitu)parsed_height;
    return true;
}

static bool GetSideBySideBaseWindowSize(Bitu *target_width, Bitu *target_height)
{
    Bitu base_width = 0;
    Bitu base_height = 0;

    const Section_prop *render_section = static_cast<const Section_prop *>(
            control->GetSection("render"));
    const char *configured = render_section
            ? render_section->Get_string("mod renderer comparison resolution")
            : NULL;

    if (!ParseComparisonPaneSize(configured, &base_width, &base_height) &&
        !GetConfiguredWindowSize(&base_width, &base_height)) {
        const Bitu current_width = currentWindowWidth ? currentWindowWidth :
                (sdl.surface ? (Bitu)sdl.surface->w : (Bitu)0u);
        const Bitu current_height = currentWindowHeight ? currentWindowHeight :
                (sdl.surface ? (Bitu)sdl.surface->h : (Bitu)0u);
        base_width = sdl.clip.w > 0 ? (Bitu)sdl.clip.w : current_width;
        base_height = sdl.clip.h > 0 ? (Bitu)sdl.clip.h : current_height;
    }

    if (base_width == 0 || base_height == 0)
        return false;

    if (target_width)
        *target_width = ClampOpenGLWindowDimension(base_width);
    if (target_height)
        *target_height = ClampOpenGLWindowDimension(base_height);
    return true;
}

static bool GetSideBySideWindowSize(Bitu *target_width, Bitu *target_height)
{
    Bitu base_width = 0;
    Bitu base_height = 0;

    if (!GetSideBySideBaseWindowSize(&base_width, &base_height))
        return false;

    if (target_width)
        *target_width = ClampOpenGLWindowDimension(base_width * 2u);
    if (target_height)
        *target_height = base_height;
    return true;
}

int Voodoo_OGL_GetWidth();
int Voodoo_OGL_GetHeight();
bool Voodoo_OGL_Active();
bool InitCodePage();
uint8_t *GetDbcsFont(Bitu code);

// NTS: With high DPI displays (e.g. on Windows 7+ with DPI scaling enabled)
//      this works better with maximized window or full-screen mode and the
//      setting "dpi aware=true".

static void GetOpenGLPPSourceGeometry(int *orig_w,
                                      int *orig_h,
                                      int *min_w,
                                      int *min_h,
                                      double *pixel_aspect_ratio)
{
    int source_w = (int)render.src.width;
    int source_h = (int)render.src.height;
    int minimum_w = source_w;
    int minimum_h = source_h;

    int x = (aspect_ratio_x>0 && aspect_ratio_y>0) ? aspect_ratio_x : ((aspect_ratio_x==-1 && aspect_ratio_y==-1) ? sdl.draw.width : 4);
    int y = (aspect_ratio_x>0 && aspect_ratio_y>0) ? aspect_ratio_y : ((aspect_ratio_x==-1 && aspect_ratio_y==-1) ? sdl.draw.height : 3);
    double par = (double)source_w / source_h * y / x;
    /* HACK: because RENDER_SetSize() does not set dblw and dblh correctly: */
    /* E.g. in 360x360 mode DOSBox-X will wrongly allocate a 720x360 area. I  */
    /* therefore calculate square-pixel proportions par_sq myself:          */
    double par_sq;
         if( par < 0.707 ) { par_sq = 0.5; minimum_w *= 2; }
    else if( par > 1.414 ) { par_sq = 2.0; minimum_h *= 2; }
    else                     par_sq = 1.0;

    if( !render.aspect ) par = par_sq;

    *orig_w = source_w;
    *orig_h = source_h;
    *min_w = minimum_w;
    *min_h = minimum_h;
    *pixel_aspect_ratio = par;
}

static void PPScale (
    uint16_t  fixed_w , uint16_t  fixed_h,
    uint16_t* window_w, uint16_t* window_h,
    SDL_Rect* target_clip,
    bool log_result = true )
{
    int sx, sy, orig_w, orig_h, min_w, min_h;
    double par;

    GetOpenGLPPSourceGeometry(&orig_w,
                              &orig_h,
                              &min_w,
                              &min_h,
                              &par);

    *window_w = fixed_w; *window_h = fixed_h;
    /* Handle non-fixed resolutions and ensure a sufficient window size: */
    if( fixed_w < min_w ) fixed_w = *window_w = min_w;
    if( fixed_h < min_h ) fixed_h = *window_h = min_h;

    pp_getscale(
        orig_w , orig_h , par ,
        fixed_w, fixed_h, 1.14,
        &sx    , &sy         );

    target_clip->w = orig_w * sx;
    target_clip->h = orig_h * sy;
    target_clip->x = (*window_w - target_clip->w) / 2;
    target_clip->y = (*window_h - target_clip->h) / 2;

    if (log_result) {
        LOG_MSG( "OpenGL PP: [%ix%i]: %ix%i (%3.2f) -> [%ix%i] -> %ix%i (%3.2f)",
            fixed_w,         fixed_h,
            orig_w,          orig_h, par,
            sx,              sy,
            target_clip->w,  target_clip->h, (double)sy/sx );
    }
}

static SDL_Rect FitOpenGLPPSourceToBounds(uint16_t bounds_w,
                                          uint16_t bounds_h)
{
    SDL_Rect clip = {0, 0, (int)bounds_w, (int)bounds_h};
    int orig_w = 0;
    int orig_h = 0;
    int min_w = 0;
    int min_h = 0;
    double par = 1.0;
    GetOpenGLPPSourceGeometry(&orig_w,
                              &orig_h,
                              &min_w,
                              &min_h,
                              &par);

    if ((int)bounds_w >= min_w && (int)bounds_h >= min_h) {
        uint16_t logical_window_width = bounds_w;
        uint16_t logical_window_height = bounds_h;
        PPScale(bounds_w,
                bounds_h,
                &logical_window_width,
                &logical_window_height,
                &clip,
                false);
        return clip;
    }

    // Pixel-perfect scaling only magnifies. When a comparison pane is smaller
    // than the source's minimum integer-scaled footprint, fit the same intended
    // display aspect uniformly inside the pane instead of growing past it.
    const double display_aspect = (double)orig_w / ((double)orig_h * par);
    int fitted_w = (int)bounds_w;
    int fitted_h = std::max(1, (int)std::lround(fitted_w / display_aspect));
    if (fitted_h > (int)bounds_h) {
        fitted_h = (int)bounds_h;
        fitted_w = std::max(1, (int)std::lround(fitted_h * display_aspect));
    }

    clip.w = std::min((int)bounds_w, fitted_w);
    clip.h = std::min((int)bounds_h, fitted_h);
    clip.x = ((int)bounds_w - clip.w) / 2;
    clip.y = ((int)bounds_h - clip.h) / 2;
    return clip;
}

static SDL_Surface* SetupSurfaceScaledOpenGL(uint32_t sdl_flags, uint32_t bpp) 
{
    uint16_t fixedWidth;
    uint16_t fixedHeight;
    uint16_t windowWidth;
    uint16_t windowHeight;
    bool side_by_side_resize_override = false;

retry:
#if defined(C_SDL2)
    if (sdl.desktop.want_type == SCREEN_OPENGL)
        sdl_flags |= (unsigned int)SDL_WINDOW_OPENGL;
#else
    if (sdl.desktop.want_type == SCREEN_OPENGL)
        sdl_flags |= (unsigned int)SDL_OPENGL;
#endif

    if (sdl.desktop.fullscreen) 
    {
        fixedWidth = sdl.desktop.full.fixed ? sdl.desktop.full.width : 0;
        fixedHeight = sdl.desktop.full.fixed ? sdl.desktop.full.height : 0;
#if defined(C_SDL2)
        sdl_flags |= (unsigned int)(SDL_WINDOW_FULLSCREEN);
#else
        sdl_flags |= (unsigned int)(SDL_FULLSCREEN | SDL_HWSURFACE);
#endif
    }
    else 
    {
        Bitu side_by_side_base_width = 0;
        Bitu side_by_side_base_height = 0;
        side_by_side_resize_override =
                (mod_render_view_mode == MOD_RENDER_VIEW_SIDE_BY_SIDE ||
                 mod_render_view_mode ==
                         MOD_RENDER_VIEW_SIDE_BY_SIDE_SUPPRESSED) &&
                GetSideBySideBaseWindowSize(&side_by_side_base_width,
                                            &side_by_side_base_height);

        // Side-by-side is the one presentation mode where we intentionally
        // treat windowresolution as one side. DOSBox-X/OpenGLPP computes its
        // usual clip inside that side; we widen the SDL window after scaling.
        fixedWidth = side_by_side_resize_override ?
                (uint16_t)side_by_side_base_width : sdl.desktop.window.width;
        fixedHeight = side_by_side_resize_override ?
                (uint16_t)side_by_side_base_height : sdl.desktop.window.height;
#if !defined(C_SDL2)
        sdl_flags |= (unsigned int)SDL_HWSURFACE;
#endif
    }

    if (fixedWidth == 0 || fixedHeight == 0) 
    {
        Bitu consider_height = menu.maxwindow ? currentWindowHeight : 0;
        Bitu consider_width = menu.maxwindow ? currentWindowWidth : 0;
        int final_height = (int)max(consider_height, userResizeWindowHeight);
        int final_width = (int)max(consider_width, userResizeWindowWidth);

        fixedWidth = final_width;
        fixedHeight = final_height;
    }

#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
    /* scale the menu bar if the window is large enough */
    /* SDL drawn menus cannot coexist with 3Dfx emulation. In fact, there is a serious
     * bug in SDL1 builds that rapidly expands the vertical size of the menu every frame. */
    if (Voodoo_OGL_GetWidth() != 0 && Voodoo_OGL_GetHeight() != 0 && Voodoo_OGL_Active() && sdl.desktop.prevent_fullscreen) {
    }
    else {
        int cw = fixedWidth, ch = fixedHeight;
        int scale = 1;

        if (cw == 0)
            cw = (uint16_t)(sdl.draw.width*sdl.draw.scalex);
        if (ch == 0)
            ch = (uint16_t)(sdl.draw.height*sdl.draw.scaley);

        while ((cw / scale) >= (640 * 2) && (ch / scale) >= (400 * 2))
            scale++;

        LOG_MSG("menuScale=%d", scale);
        mainMenu.setScale((unsigned int)scale);

        if (mainMenu.isVisible() && !sdl.desktop.fullscreen && fixedHeight)
            fixedHeight -= mainMenu.menuBox.h;
    }
#endif

    sdl.clip.x = 0; sdl.clip.y = 0;
    if (Voodoo_OGL_GetWidth() != 0 && Voodoo_OGL_GetHeight() != 0 && Voodoo_OGL_Active() && sdl.desktop.prevent_fullscreen)
    { 
        /* 3Dfx openGL do not allow resize */
        sdl.clip.w = windowWidth = (uint16_t)Voodoo_OGL_GetWidth();
        sdl.clip.h = windowHeight = (uint16_t)Voodoo_OGL_GetHeight();
    } else if (sdl_opengl.kind == GLPerfect ) {
        PPScale( fixedWidth, fixedHeight, &windowWidth, &windowHeight,
                 &sdl.clip );
    } else
        if (fixedWidth && fixedHeight)
        {
            windowWidth  = fixedWidth;
            windowHeight = fixedHeight;
            sdl.clip.w = windowWidth;
            sdl.clip.h = windowHeight;
            if (render.aspect || NativeCrtLetterboxes())
                aspectCorrectFitClip(sdl.clip.w, sdl.clip.h, sdl.clip.x, sdl.clip.y, fixedWidth, fixedHeight);
        }
        else
        {
            windowWidth = (uint16_t)(sdl.draw.width * sdl.draw.scalex);
            windowHeight = (uint16_t)(sdl.draw.height * sdl.draw.scaley);
            if (render.aspect) aspectCorrectExtend(windowWidth, windowHeight);
            sdl.clip.w = windowWidth; sdl.clip.h = windowHeight;
            if (NativeCrtLetterboxes())
                aspectCorrectFitClip(sdl.clip.w, sdl.clip.h, sdl.clip.x, sdl.clip.y, windowWidth, windowHeight);
        }

    if (side_by_side_resize_override)
        windowWidth = (uint16_t)ClampOpenGLWindowDimension((Bitu)windowWidth * 2u);

    LOG(LOG_MISC, LOG_DEBUG)("GFX_SetSize OpenGL window=%ux%u clip=x,y,w,h=%d,%d,%d,%d",
        (unsigned int)windowWidth,
        (unsigned int)windowHeight,
        (unsigned int)sdl.clip.x,
        (unsigned int)sdl.clip.y,
        (unsigned int)sdl.clip.w,
        (unsigned int)sdl.clip.h);

#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
    if (mainMenu.isVisible() && !sdl.desktop.fullscreen) 
    {
        if (Voodoo_OGL_GetWidth() != 0 && Voodoo_OGL_GetHeight() != 0 && Voodoo_OGL_Active() && sdl.desktop.prevent_fullscreen) {
        }
        else {
            windowHeight += mainMenu.menuBox.h;
            sdl.clip.y += mainMenu.menuBox.h;
        }
    }
#endif

#if defined(C_SDL2)
    (void)bpp; // unused param
    sdl.surface = NULL;
    sdl.window = GFX_SetSDLWindowMode(windowWidth, windowHeight, (sdl_flags & SDL_WINDOW_OPENGL) ? SCREEN_OPENGL : SCREEN_SURFACE);
    if (sdl.window != NULL) sdl.surface = SDL_GetWindowSurface(sdl.window);
#elif defined(SDL_DOSBOX_X_SPECIAL)
    sdl.surface = SDL_SetVideoMode(windowWidth, windowHeight, (int)bpp, (unsigned int)sdl_flags | (unsigned int)(setSizeButNotResize() ? SDL_HAX_NORESIZEWINDOW : 0));
#else
    sdl.surface = SDL_SetVideoMode(windowWidth, windowHeight, (int)bpp, (unsigned int)sdl_flags);
#endif
    if (sdl.surface == NULL && sdl.desktop.fullscreen) {
        LOG_MSG("Fullscreen not supported: %s", SDL_GetError());
        sdl.desktop.fullscreen = false;
#if defined(C_SDL2)
        sdl_flags &= ~SDL_WINDOW_FULLSCREEN;
#else
        sdl_flags &= ~SDL_FULLSCREEN;
#endif
        GFX_CaptureMouse();
        goto retry;
    }

    sdl.deferred_resize = false;
    sdl.must_redraw_all = true;

    /* There seems to be a problem with MesaGL in Linux/X11 where
    * the first swap buffer we do is misplaced according to the
    * previous window size.
    *
    * NTS: This seems to have been fixed, which is why this is
    *      commented out. I guess not calling GFX_SetSize()
    *      with a 0x0 widthxheight helps! */
    //    sdl.gfx_force_redraw_count = 2;
    UpdateWindowDimensions();
    GFX_LogSDLState();
    ApplyPreventCap();

#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
    mainMenu.screenWidth = (size_t)(sdl.surface->w);
    mainMenu.screenHeight = (size_t)(sdl.surface->h);
    mainMenu.updateRect();
    mainMenu.setRedraw();
#endif

    return sdl.surface;
}

// output API below

void OUTPUT_OPENGL_Initialize()
{
    memset(&sdl_opengl, 0, sizeof(sdl_opengl));
    mod_render_view_mode = MOD_RENDER_VIEW_GAME_ONLY;
    mod_render_single_view_mode = MOD_RENDER_VIEW_MOD_ONLY;
    mod_render_view_initialized = false;
    MOD_SetFramePacingViewEligible(false);
}

void OUTPUT_OPENGL_Select( GLKind kind )
{
    if (!mod_render_view_initialized) {
        mod_render_view_mode = GetConfiguredModRenderStartView();
        if (mod_render_view_mode == MOD_RENDER_VIEW_GAME_ONLY ||
            mod_render_view_mode == MOD_RENDER_VIEW_MOD_ONLY) {
            mod_render_single_view_mode = mod_render_view_mode;
        }
        mod_render_view_initialized = true;
    }
    MOD_SetFramePacingViewEligible(
            mod_render_view_mode == MOD_RENDER_VIEW_MOD_ONLY);

    sdl.desktop.want_type = SCREEN_OPENGL;
    render.aspectOffload = true;

#if defined(WIN32) && !defined(C_SDL2)
    SDL1_hax_inhibit_WM_PAINT = 0;
#endif

    sdl_opengl.use_shader = false;
    initgl=0;
#if defined(C_SDL2)
#if defined(MACOSX)
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "opengl");// setting this to "1" caused crashes on macOS
#else
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1"); // setting this to "opengl" caused crashes in certain situations (#3661). can still be overridden via environment variable
#endif

    void GFX_SetResizeable(bool enable);
    GFX_SetResizeable(true);
    sdl.window = GFX_SetSDLWindowMode(640,400, SCREEN_OPENGL);
    if (sdl.window) {
        if(sdl_opengl.context) {
            RA_GLSL_Release();
            SDL_GL_DeleteContext(sdl_opengl.context);
            sdl_opengl.context = nullptr;
        }
        sdl_opengl.context = SDL_GL_CreateContext(sdl.window);
        if (sdl_opengl.context && SDL_GL_MakeCurrent(sdl.window, sdl_opengl.context) != 0)
            LOG_MSG("WARNING: SDL2 unable to make current GL context");
        if (sdl_opengl.context) {
            sdl_opengl.context_generation++;
            sdl_opengl.mod_present_count = 0;
            DOSBoxPython_NotifyOpenGLContextCreated(sdl_opengl.context_generation);
        }
        sdl.surface = SDL_GetWindowSurface(sdl.window);

        LOG_MSG( "OpenGL Version : %s", glGetString( GL_VERSION ));
    }
    if (!sdl.window || !sdl_opengl.context || sdl.surface == NULL) {
#else
    sdl.surface = SDL_SetVideoMode(640,400,0,SDL_OPENGL);
    if (sdl.surface == NULL) {
#endif
        LOG_MSG("Could not initialize OpenGL, switching back to surface");
        sdl.desktop.want_type = SCREEN_SURFACE;
    } else if (initgl!=2) {
        initgl = 1;
        sdl_opengl.kind = kind;
        sdl.desktop.isperfect = kind == GLPerfect;
        sdl_opengl.program_object = 0;
        glAttachShader = (PFNGLATTACHSHADERPROC)SDL_GL_GetProcAddress("glAttachShader");
        glCompileShader = (PFNGLCOMPILESHADERPROC)SDL_GL_GetProcAddress("glCompileShader");
        glCreateProgram = (PFNGLCREATEPROGRAMPROC)SDL_GL_GetProcAddress("glCreateProgram");
        glCreateShader = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
        glDeleteProgram = (PFNGLDELETEPROGRAMPROC)SDL_GL_GetProcAddress("glDeleteProgram");
        glDeleteShader = (PFNGLDELETESHADERPROC)SDL_GL_GetProcAddress("glDeleteShader");
        glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
        glGetAttribLocation = (PFNGLGETATTRIBLOCATIONPROC)SDL_GL_GetProcAddress("glGetAttribLocation");
        glGetProgramiv = (PFNGLGETPROGRAMIVPROC)SDL_GL_GetProcAddress("glGetProgramiv");
        glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)SDL_GL_GetProcAddress("glGetProgramInfoLog");
        glGetShaderiv = (PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
        glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)SDL_GL_GetProcAddress("glGetShaderInfoLog");
        glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)SDL_GL_GetProcAddress("glGetUniformLocation");
        glLinkProgram = (PFNGLLINKPROGRAMPROC)SDL_GL_GetProcAddress("glLinkProgram");
        glShaderSource = (PFNGLSHADERSOURCEPROC_NP)SDL_GL_GetProcAddress("glShaderSource");
        glUniform2f = (PFNGLUNIFORM2FPROC)SDL_GL_GetProcAddress("glUniform2f");
        glUniform1i = (PFNGLUNIFORM1IPROC)SDL_GL_GetProcAddress("glUniform1i");
        glUseProgram = (PFNGLUSEPROGRAMPROC)SDL_GL_GetProcAddress("glUseProgram");
        glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)SDL_GL_GetProcAddress("glVertexAttribPointer");
        sdl_opengl.use_shader = (glAttachShader && glCompileShader && glCreateProgram && glDeleteProgram && glDeleteShader && \
            glEnableVertexAttribArray && glGetAttribLocation && glGetProgramiv && glGetProgramInfoLog && \
            glGetShaderiv && glGetShaderInfoLog && glGetUniformLocation && glLinkProgram && glShaderSource && \
            glUniform2f && glUniform1i && glUseProgram && glVertexAttribPointer);
        if (sdl_opengl.use_shader) initgl = 2;
        sdl_opengl.buffer=0;
        sdl_opengl.framebuf = nullptr;
        sdl_opengl.texture=0;
        sdl_opengl.displaylist=0;
        glGetIntegerv (GL_MAX_TEXTURE_SIZE, &sdl_opengl.max_texsize);
        glGenBuffersARB = (PFNGLGENBUFFERSARBPROC)SDL_GL_GetProcAddress("glGenBuffersARB");
        glBindBufferARB = (PFNGLBINDBUFFERARBPROC)SDL_GL_GetProcAddress("glBindBufferARB");
        glDeleteBuffersARB = (PFNGLDELETEBUFFERSARBPROC)SDL_GL_GetProcAddress("glDeleteBuffersARB");
        glBufferDataARB = (PFNGLBUFFERDATAARBPROC)SDL_GL_GetProcAddress("glBufferDataARB");
        glMapBufferARB = (PFNGLMAPBUFFERARBPROC)SDL_GL_GetProcAddress("glMapBufferARB");
        glUnmapBufferARB = (PFNGLUNMAPBUFFERARBPROC)SDL_GL_GetProcAddress("glUnmapBufferARB");
        dosbox_glActiveTexture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
        dosbox_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
        dosbox_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
        const char * gl_ext = (const char *)glGetString (GL_EXTENSIONS);
        if(gl_ext && *gl_ext){
            sdl_opengl.packed_pixel=(strstr(gl_ext,"EXT_packed_pixels") != NULL);
            sdl_opengl.paletted_texture=(strstr(gl_ext,"EXT_paletted_texture") != NULL);
            //sdl_opengl.pixel_buffer_object=(strstr(gl_ext,"GL_ARB_pixel_buffer_object") != NULL ) && glGenBuffersARB && glBindBufferARB && glDeleteBuffersARB && glBufferDataARB && glMapBufferARB && glUnmapBufferARB;
        } else {
            sdl_opengl.packed_pixel = false;
            sdl_opengl.paletted_texture = false;
            //sdl_opengl.pixel_buffer_object = false;
        }
#ifdef DB_DISABLE_DBO
        sdl_opengl.pixel_buffer_object = false;
#endif
        //LOG_MSG("OpenGL extension: pixel_buffer_object %d",sdl_opengl.pixel_buffer_object);
	} /* OPENGL is requested end */
    ApplyPreventCap();
}

Bitu OUTPUT_OPENGL_GetBestMode(Bitu flags)
{
    if (!(flags & GFX_CAN_32)) return 0; // OpenGL requires 32-bit output mode
    flags |= GFX_SCALING;
    flags &= ~(GFX_CAN_8 | GFX_CAN_15 | GFX_CAN_16);
    return flags;
}

/* Create a GLSL shader object, load the shader source, and compile the shader. */
static GLuint BuildShader ( GLenum type, const char *shaderSrc ) {
	GLuint shader;
	GLint compiled;
	const char* src_strings[2];
	std::string top;

	// look for "#version" because it has to occur first
	const char *ver = strstr(shaderSrc, "#version ");
	if (ver) {
		const char *endline = strchr(ver+9, '\n');
		if (endline) {
			top.assign(shaderSrc, endline-shaderSrc+1);
			shaderSrc = endline+1;
		}
	}

	top += (type==GL_VERTEX_SHADER) ? "#define VERTEX 1\n":"#define FRAGMENT 1\n";
	if (sdl_opengl.kind == GLNearest || sdl_opengl.kind == GLPerfect)
		top += "#define OPENGLNB 1\n";

	src_strings[0] = top.c_str();
	src_strings[1] = shaderSrc;

	// Create the shader object
	shader = glCreateShader(type);
	if (shader == 0) return 0;

	// Load the shader source
	glShaderSource(shader, 2, src_strings, NULL);

	// Compile the shader
	glCompileShader(shader);

	// Check the compile status
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

	if (!compiled) {
		char* infoLog = NULL;
		GLint infoLen = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);

		if (infoLen>1) infoLog = (char*)malloc(infoLen);
		if (infoLog) {
			glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
			LOG_MSG("Error compiling shader: %s", infoLog);
			free(infoLog);
		} else LOG_MSG("Error getting shader compilation log");

		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

static bool LoadGLShaders(const char *src, GLuint *vertex, GLuint *fragment) {
	GLuint s = BuildShader(GL_VERTEX_SHADER, src);
	if (s) {
		*vertex = s;
		s = BuildShader(GL_FRAGMENT_SHADER, src);
		if (s) {
			*fragment = s;
			return true;
		}
		glDeleteShader(*vertex);
	}
	return false;
}

#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
void UpdateSDLDrawTexture() {
    glBindTexture(GL_TEXTURE_2D, SDLDrawGenFontTexture);

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0);

    // No borders
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)SDLDrawGenFontTextureWidth, (int)SDLDrawGenFontTextureHeight, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);

    /* load the font */
    {
        int cp = dos.loaded_codepage;
        if (!cp) InitCodePage();
        uint32_t tmp[8 * 16];
        unsigned int x, y, c;

        for (c = 0; c < 256; c++)
        {
            unsigned char *bmp;
            if (font_16_init&&dos.loaded_codepage&&dos.loaded_codepage!=437)
                bmp = int10_font_16_init + (c * 16);
            else
                bmp = int10_font_16 + (c * 16);
            for (y = 0; y < 16; y++)
                for (x = 0; x < 8; x++)
                    tmp[(y * 8) + x] = (bmp[y] & (0x80 >> x)) ? 0xFFFFFFFFUL : 0x00000000UL;

            glTexSubImage2D(GL_TEXTURE_2D, /*level*/0, /*x*/(int)((c % 16) * 8), /*y*/(int)((c / 16) * 16),
                8, 16, GL_BGRA_EXT, GL_UNSIGNED_INT_8_8_8_8_REV, (void*)tmp);
        }
        lastcp = dos.loaded_codepage;
        dos.loaded_codepage = cp;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

void UpdateSDLDrawDBCSTexture(Bitu code) {
    glBindTexture(GL_TEXTURE_2D, SDLDrawGenDBCSFontTexture);

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0);

    // No borders
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (int)SDLDrawGenFontTextureWidth, (int)SDLDrawGenFontTextureHeight, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);

    /* load the font */
    {
        int cp = dos.loaded_codepage;
        if (!cp) InitCodePage();
        uint32_t tmp[8 * 16];
        unsigned int x, y, c;

        for (c = 0; c < (code?2:1); c++)
        {
            unsigned char *bmp = code?GetDbcsFont(code):(int10_font_16 + 0xFB * 16);
            for (y = 0; y < 16; y++)
                for (x = 0; x < 8; x++)
                    tmp[(y * 8) + x] = (bmp[code?(y*2+c):y] & (0x80 >> x)) ? 0xFFFFFFFFUL : 0x00000000UL;

            glTexSubImage2D(GL_TEXTURE_2D, /*level*/0, /*x*/(int)(c * 8), /*y*/0,
                8, 16, GL_BGRA_EXT, GL_UNSIGNED_INT_8_8_8_8_REV, (void*)tmp);
        }
        lastcp = dos.loaded_codepage;
        dos.loaded_codepage = cp;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}
#endif

Bitu OUTPUT_OPENGL_SetSize()
{
    Bitu retFlags = 0;

    /* NTS: Apparently calling glFinish/glFlush before setup causes a segfault within
    *      the OpenGL library on Mac OS X. */
    if (sdl_opengl.inited)
    {
        glFinish();
        glFlush();
    }

    if (sdl_opengl.pixel_buffer_object)
    {
	    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, 0);
	    if (sdl_opengl.buffer) glDeleteBuffersARB(1, &sdl_opengl.buffer);
	    sdl_opengl.buffer = 0;
    }
    if (sdl_opengl.framebuf != NULL) {
	    free(sdl_opengl.framebuf);
	    sdl_opengl.framebuf = NULL;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    Section_prop* sec = static_cast<Section_prop*>(control->GetSection("vsync"));
    if (sec) {
#if defined(C_SDL2)
        SDL_GL_SetSwapInterval((!strcmp(sec->Get_string("vsyncmode"), "host")) ? 1 : 0);
#elif SDL_VERSION_ATLEAST(1, 2, 11)
        SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, (!strcmp(sec->Get_string("vsyncmode"), "host")) ? 1 : 0);
#endif
    }

    // try 32 bits first then 16
#if defined(C_SDL2)
    if (SetupSurfaceScaledOpenGL(SDL_WINDOW_RESIZABLE,32)==NULL) SetupSurfaceScaledOpenGL(SDL_WINDOW_RESIZABLE,16);
#else
    if (SetupSurfaceScaledOpenGL(SDL_RESIZABLE,32)==NULL) SetupSurfaceScaledOpenGL(SDL_RESIZABLE,16);
#endif
    if (!sdl.surface || sdl.surface->format->BitsPerPixel < 15)
    {
        LOG_MSG("SDL:OPENGL:Can't open drawing surface, are you running in 16bpp(or higher) mode?");
        return 0;
    }

    glFinish();
    glFlush();

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &sdl_opengl.max_texsize);

    Bitu adjTexWidth = sdl.draw.width;
    Bitu adjTexHeight = sdl.draw.height;
#if C_XBRZ
    // we do the same as with Direct3D: precreate pixel buffer adjusted for xBRZ
    if (sdl_xbrz.enable && xBRZ_SetScaleParameters((int)adjTexWidth, (int)adjTexHeight, (int)sdl.clip.w, (int)sdl.clip.h))
    {
        adjTexWidth = adjTexWidth * (unsigned int)sdl_xbrz.scale_factor;
        adjTexHeight = adjTexHeight * (unsigned int)sdl_xbrz.scale_factor;
    }
#endif

    int texsize = 2 << int_log2((int)(adjTexWidth > adjTexHeight ? adjTexWidth : adjTexHeight));
    if (texsize > sdl_opengl.max_texsize) 
    {
        LOG_MSG("SDL:OPENGL:No support for texturesize of %d (max size is %d), falling back to surface", texsize, sdl_opengl.max_texsize);
        return 0;
    }

    if (sdl_opengl.use_shader && sdl_opengl.shader_src == NULL && !sdl_opengl.shader_def) sdl_opengl.use_shader = false;
    if (sdl_opengl.use_shader) {
        GLuint prog=0;
        // reset error
        glGetError();
        glGetIntegerv(GL_CURRENT_PROGRAM, (GLint*)&prog);
        // if there was an error this context doesn't support shaders
        if (glGetError()==GL_NO_ERROR && (sdl_opengl.program_object==0 || prog!=sdl_opengl.program_object)) {
            // check if existing program is valid
            if (sdl_opengl.program_object) {
                glUseProgram(sdl_opengl.program_object);
                if (glGetError() != GL_NO_ERROR) {
                    // program is not usable (probably new context), purge it
                    glDeleteProgram(sdl_opengl.program_object);
                    sdl_opengl.program_object = 0;
                }
            }

            // does program need to be rebuilt?
            if (sdl_opengl.program_object == 0) {
                GLuint vertexShader, fragmentShader;
                const char *src = sdl_opengl.shader_src;
                if (src && !LoadGLShaders(src, &vertexShader, &fragmentShader)) {
                    LOG_MSG("SDL:OPENGL:Failed to compile shader, falling back to default");
                    src = NULL;
                }
                if (src == NULL && !LoadGLShaders(shader_src_default, &vertexShader, &fragmentShader)) {
                    LOG_MSG("SDL:OPENGL:Failed to compile default shader!");
                    return 0;
                }

                sdl_opengl.program_object = glCreateProgram();
                if (!sdl_opengl.program_object) {
                    glDeleteShader(vertexShader);
                    glDeleteShader(fragmentShader);
                    LOG_MSG("SDL:OPENGL:Can't create program object, falling back to surface");
                    return 0;
                }
#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
                // Todo: Make SDL-drawn menu work with custom GLSL shaders
                if (!sdl.desktop.prevent_fullscreen) {
                    menu.toggle=false;
                    mainMenu.showMenu(false);
                    mainMenu.get_item("mapper_togmenu").check(!menu.toggle).refresh_item(mainMenu);
                }
#endif
                glAttachShader(sdl_opengl.program_object, vertexShader);
                glAttachShader(sdl_opengl.program_object, fragmentShader);
                // Link the program
                glLinkProgram(sdl_opengl.program_object);
                // Even if we *are* successful, we may delete the shader objects
                glDeleteShader(vertexShader);
                glDeleteShader(fragmentShader);

                // Check the link status
                GLint isProgramLinked;
                glGetProgramiv(sdl_opengl.program_object, GL_LINK_STATUS, &isProgramLinked);
                if (!isProgramLinked) {
                    char * infoLog = NULL;
                    GLint infoLen = 0;

                    glGetProgramiv(sdl_opengl.program_object, GL_INFO_LOG_LENGTH, &infoLen);
                    if (infoLen>1) infoLog = (char*)malloc(infoLen);
                    if (infoLog) {
                        glGetProgramInfoLog(sdl_opengl.program_object, infoLen, NULL, infoLog);
                        LOG_MSG("SDL:OPENGL:Error linking program:\n %s", infoLog);
                        free(infoLog);
                    } else LOG_MSG("SDL:OPENGL:Failed to retrieve program link log");

                    glDeleteProgram(sdl_opengl.program_object);
                    sdl_opengl.program_object = 0;
                    return 0;
                }

                glUseProgram(sdl_opengl.program_object);

                GLint u = glGetAttribLocation(sdl_opengl.program_object, "a_position");
                sdl_opengl.position_attrib = u;
                // NTS: This is now a triangle strip (GL_TRIANGLE_STRIP)
                // upper left
                sdl_opengl.vertex_data[0] = -1.0f;
                sdl_opengl.vertex_data[1] =  1.0f;
                // lower left
                sdl_opengl.vertex_data[2] = -1.0f;
                sdl_opengl.vertex_data[3] = -1.0f;
                // upper right
                sdl_opengl.vertex_data[4] =  1.0f;
                sdl_opengl.vertex_data[5] =  1.0f;
                // lower right
                sdl_opengl.vertex_data[6] =  1.0f;
                sdl_opengl.vertex_data[7] = -1.0f;
                // Load the vertex positions
                glVertexAttribPointer(u, 2, GL_FLOAT, GL_FALSE, 0, sdl_opengl.vertex_data);
                glEnableVertexAttribArray(u);

                u = glGetUniformLocation(sdl_opengl.program_object, "rubyTexture");
                glUniform1i(u, 0);

                sdl_opengl.ruby.texture_size = glGetUniformLocation(sdl_opengl.program_object, "rubyTextureSize");
                sdl_opengl.ruby.input_size = glGetUniformLocation(sdl_opengl.program_object, "rubyInputSize");
                sdl_opengl.ruby.output_size = glGetUniformLocation(sdl_opengl.program_object, "rubyOutputSize");
                sdl_opengl.ruby.frame_count = glGetUniformLocation(sdl_opengl.program_object, "rubyFrameCount");
                // Don't force updating unless a shader depends on frame_count
                RENDER_SetForceUpdate(sdl_opengl.ruby.frame_count != (GLint)-1);
            }
        }
    }

    /* Create the texture and display list */
    if (sdl_opengl.pixel_buffer_object) 
    {
        glGenBuffersARB(1, &sdl_opengl.buffer);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, sdl_opengl.buffer);
        glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_EXT, (int)(adjTexWidth*adjTexHeight * 4), NULL, GL_STREAM_DRAW_ARB);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, 0);
    }
    else
    {
        /* NTS: Allocate an additional 4K on top because modern MesaGL / libgallium likes to use SSE/AVX
	 *      instructions to memcpy on texture update and that can EASILY read a little bit past the buffer. */
        sdl_opengl.framebuf = calloc((adjTexWidth*adjTexHeight) + (4096/4), 4); //32 bit color
    }
    sdl_opengl.pitch = adjTexWidth * 4;
    sdl_opengl.input_width = adjTexWidth;
    sdl_opengl.input_height = adjTexHeight;
    sdl_opengl.texture_size = texsize;

    glBindTexture(GL_TEXTURE_2D, 0);

#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
    if (SDLDrawGenFontTextureInit)
    {
        glDeleteTextures(1, &SDLDrawGenFontTexture);
        SDLDrawGenFontTexture = (GLuint)(~0UL);
        SDLDrawGenFontTextureInit = 0;
    }
#endif

    if (sdl_opengl.use_shader)
        glViewport((sdl.surface->w-sdl.clip.w)/2,(sdl.surface->h-sdl.clip.h)/2,sdl.clip.w,sdl.clip.h);
    else
        glViewport(0, 0, sdl.surface->w, sdl.surface->h);
    if (sdl_opengl.texture > 0) glDeleteTextures(1, &sdl_opengl.texture);
    glGenTextures(1, &sdl_opengl.texture);
    glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0);

    // No borders
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

    GLint interp;
    if( sdl_opengl.kind == GLNearest || sdl_opengl.kind == GLPerfect )
        interp = GL_NEAREST; else
        interp = GL_LINEAR ;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, interp );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, interp );

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texsize, texsize, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);

    // NTS: I'm told that nVidia hardware seems to triple buffer despite our
    //      request to double buffer (according to @pixelmusement), therefore
    //      the next 3 frames, instead of 2, need to be cleared.
    sdl_opengl.menudraw_countdown = 3; // two GL buffers with possible triple buffering behind our back
    sdl_opengl.clear_countdown = 3; // two GL buffers with possible triple buffering behind our back

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // SDL_GL_SwapBuffers();
    // glClear(GL_COLOR_BUFFER_BIT);
    //glShadeModel(GL_FLAT);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_TEXTURE_2D);

    if (sdl_opengl.program_object) {
        // Set shader variables
        glUniform2f(sdl_opengl.ruby.texture_size, (float)texsize, (float)texsize);
        glUniform2f(sdl_opengl.ruby.input_size, (float)adjTexWidth, (float)adjTexHeight);
        glUniform2f(sdl_opengl.ruby.output_size, sdl.clip.w, sdl.clip.h);
        // The following uniform is *not* set right now
        sdl_opengl.actual_frame_count = 0;
    } else {
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, sdl.surface->w, sdl.surface->h, 0, -1, 1);

        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glScaled(1.0 / texsize, 1.0 / texsize, 1.0);

        // if (glIsList(sdl_opengl.displaylist))
        //   glDeleteLists(sdl_opengl.displaylist, 1);
        // sdl_opengl.displaylist = glGenLists(1);
        sdl_opengl.displaylist = 1;

        glNewList(sdl_opengl.displaylist, GL_COMPILE);
        glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);

        glBegin(GL_QUADS);

        glTexCoord2i(0, 0); glVertex2i((GLint)sdl.clip.x, (GLint)sdl.clip.y); // lower left
        glTexCoord2i((GLint)adjTexWidth, 0); glVertex2i((GLint)sdl.clip.x + (GLint)sdl.clip.w, (GLint)sdl.clip.y); // lower right
        glTexCoord2i((GLint)adjTexWidth, (GLint)adjTexHeight); glVertex2i((GLint)sdl.clip.x + (GLint)sdl.clip.w, (GLint)sdl.clip.y + (GLint)sdl.clip.h); // upper right
        glTexCoord2i(0, (GLint)adjTexHeight); glVertex2i((GLint)sdl.clip.x, (GLint)sdl.clip.y + (GLint)sdl.clip.h); // upper left

        glEnd();
        glEndList();

        glBindTexture(GL_TEXTURE_2D, 0);
    }

#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
    void GFX_DrawSDLMenu(DOSBoxMenu &menu, DOSBoxMenu::displaylist &dl);
    mainMenu.setRedraw();
    GFX_DrawSDLMenu(mainMenu, mainMenu.display_list);

    // FIXME: Why do we have to reinitialize the font texture?
    // if (!SDLDrawGenFontTextureInit) {
    GLuint err = 0;

    glGetError(); /* read and discard last error */

    SDLDrawGenFontTexture = (GLuint)(~0UL);
    glGenTextures(1, &SDLDrawGenFontTexture);
    if (SDLDrawGenFontTexture == (GLuint)(~0UL) || (err = glGetError()) != 0)
    {
        LOG_MSG("WARNING: Unable to make font texture. id=%llu err=%lu",
            (unsigned long long)SDLDrawGenFontTexture, (unsigned long)err);
    }
    else
    {
        LOG_MSG("font texture id=%lu will make %u x %u",
            (unsigned long)SDLDrawGenFontTexture,
            (unsigned int)SDLDrawGenFontTextureWidth,
            (unsigned int)SDLDrawGenFontTextureHeight);

        SDLDrawGenFontTextureInit = 1;

        UpdateSDLDrawTexture();
    }

    err = 0;
    glGetError(); /* read and discard last error */

    SDLDrawGenDBCSFontTexture = (GLuint)(~0UL);
    glGenTextures(1, &SDLDrawGenDBCSFontTexture);
#endif

    glFinish();
    glFlush();

    sdl_opengl.inited = true;
    retFlags = GFX_CAN_32 | GFX_SCALING;

    if (sdl_opengl.pixel_buffer_object)
        retFlags |= GFX_HARDWARE;

    return retFlags;
}

bool OUTPUT_OPENGL_StartUpdate(uint8_t* &pixels, Bitu &pitch)
{
#if C_XBRZ    
    if (sdl_xbrz.enable && sdl_xbrz.scale_on) 
    {
        sdl_xbrz.renderbuf.resize(sdl.draw.width * sdl.draw.height);
        pixels = sdl_xbrz.renderbuf.empty() ? nullptr : reinterpret_cast<uint8_t*>(&sdl_xbrz.renderbuf[0]);
        pitch = sdl.draw.width * sizeof(uint32_t);
    }
    else
#endif
    {
        if (sdl_opengl.pixel_buffer_object)
        {
            glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, sdl_opengl.buffer);
            pixels = (uint8_t *)glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, GL_WRITE_ONLY);
        }
        else
        {
            pixels = (uint8_t *)sdl_opengl.framebuf;
        }
        pitch = sdl_opengl.pitch;
    }

    sdl.updating = true;
    return true;
}

static void CheckClearing(void) {
        if (sdl_opengl.clear_countdown > 0)
        {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (sdl_opengl.menudraw_countdown == 0)
                sdl_opengl.menudraw_countdown++;
        }
}

static void CheckMenuDrawing(void) {
        if (sdl_opengl.menudraw_countdown > 0) 
        {
            sdl_opengl.menudraw_countdown--;
#if DOSBOXMENU_TYPE == DOSBOXMENU_SDLDRAW
            mainMenu.setRedraw();
            GFX_DrawSDLMenu(mainMenu, mainMenu.display_list);
#endif
        }
}

static void CheckManagement(void) {
	CheckClearing();
	CheckMenuDrawing();
}

static bool ViewportIsEmpty(const GLViewport &viewport)
{
    return viewport.w <= 0 || viewport.h <= 0;
}

static uint32_t ScaleSurfaceCoordinate(uint32_t value,
                                       uint32_t surface_extent,
                                       uint32_t backbuffer_extent)
{
    if (surface_extent == 0u || backbuffer_extent == 0u)
        return value;

    return (uint32_t)(((uint64_t)value * (uint64_t)backbuffer_extent +
                       (uint64_t)(surface_extent / 2u)) /
                      (uint64_t)surface_extent);
}

static const char *GetModRenderViewModeNameInternal(const ModRenderViewMode mode)
{
    switch (mode) {
    case MOD_RENDER_VIEW_MOD_ONLY:
        return "mod-only";
    case MOD_RENDER_VIEW_SIDE_BY_SIDE:
        return "side-by-side";
    case MOD_RENDER_VIEW_SIDE_BY_SIDE_SUPPRESSED:
        return "side-by-side (allow scene suppression)";
    case MOD_RENDER_VIEW_GAME_ONLY:
    default:
        return "game-only";
    }
}

static const char *GetModRenderViewModeTitleLabelInternal(
        const ModRenderViewMode mode)
{
    ModSceneRasterSuppressionStats suppression = {};
    const bool scene_suppressed =
            MOD_GetSceneRasterSuppressionStats(&suppression) &&
            suppression.requested;

    switch (mode) {
    case MOD_RENDER_VIEW_MOD_ONLY:
        return scene_suppressed ? "mod; scene render suppressed" : "mod";
    case MOD_RENDER_VIEW_SIDE_BY_SIDE:
        return "orig+mod";
    case MOD_RENDER_VIEW_SIDE_BY_SIDE_SUPPRESSED:
        return scene_suppressed
                ? "orig+mod; scene render suppressed"
                : "orig+mod";
    case MOD_RENDER_VIEW_GAME_ONLY:
    default:
        return "orig";
    }
}

static bool IsModRenderSideBySideMode(const ModRenderViewMode mode)
{
    return mode == MOD_RENDER_VIEW_SIDE_BY_SIDE ||
           mode == MOD_RENDER_VIEW_SIDE_BY_SIDE_SUPPRESSED;
}

static bool SetModRenderViewMode(const ModRenderViewMode mode,
                                 Bitu *target_width,
                                 Bitu *target_height)
{
    if (target_width)
        *target_width = 0;
    if (target_height)
        *target_height = 0;

    MOD_SetFramePacingViewEligible(mode == MOD_RENDER_VIEW_MOD_ONLY);
    if (mode == mod_render_view_mode)
        return false;

    const bool was_side_by_side = IsModRenderSideBySideMode(mod_render_view_mode);
    const bool will_be_side_by_side = IsModRenderSideBySideMode(mode);
    mod_render_view_mode = mode;

    // Non-suppressed presentation choices fail open immediately. Entering a
    // suppression-capable view still waits for Python to publish a fresh frame.
    if (mode == MOD_RENDER_VIEW_GAME_ONLY ||
        mode == MOD_RENDER_VIEW_SIDE_BY_SIDE) {
        MOD_DisableSceneRasterSuppression();
    }

    if (!was_side_by_side && will_be_side_by_side) {
        if (sdl.desktop.fullscreen)
            return false;

        const Bitu current_width = currentWindowWidth ? currentWindowWidth :
                (sdl.surface ? (Bitu)sdl.surface->w : (Bitu)0u);
        const Bitu current_height = currentWindowHeight ? currentWindowHeight :
                (sdl.surface ? (Bitu)sdl.surface->h : (Bitu)0u);
        Bitu side_by_side_width = 0;
        Bitu side_by_side_height = 0;
        const bool have_side_by_side_size =
                GetSideBySideWindowSize(&side_by_side_width, &side_by_side_height);

        mod_render_saved_window_width = current_width;
        mod_render_saved_window_height = current_height;
        mod_render_saved_window_size_valid =
                (mod_render_saved_window_width > 0u &&
                 mod_render_saved_window_height > 0u);

        if (target_width)
            *target_width = side_by_side_width;
        if (target_height)
            *target_height = side_by_side_height;
        return mod_render_saved_window_size_valid && have_side_by_side_size;
    }

    if (was_side_by_side && !will_be_side_by_side) {
        if (sdl.desktop.fullscreen) {
            mod_render_saved_window_size_valid = false;
            return false;
        }

        Bitu single_width = mod_render_saved_window_width;
        Bitu single_height = mod_render_saved_window_height;
        const bool have_single_size = mod_render_saved_window_size_valid ||
                GetSideBySideBaseWindowSize(&single_width, &single_height);
        mod_render_saved_window_size_valid = false;
        if (!have_single_size)
            return false;

        if (target_width)
            *target_width = single_width;
        if (target_height)
            *target_height = single_height;
        return true;
    }

    return false;
}

bool OUTPUT_OPENGL_ToggleModRenderSingleView(Bitu *target_width,
                                              Bitu *target_height)
{
    if (!OUTPUT_OPENGL_ModRendererAvailable()) {
        if (target_width)
            *target_width = 0;
        if (target_height)
            *target_height = 0;
        return false;
    }

    mod_render_single_view_mode =
            mod_render_single_view_mode == MOD_RENDER_VIEW_MOD_ONLY
                    ? MOD_RENDER_VIEW_GAME_ONLY
                    : MOD_RENDER_VIEW_MOD_ONLY;
    return SetModRenderViewMode(mod_render_single_view_mode,
                                target_width,
                                target_height);
}

bool OUTPUT_OPENGL_ToggleModRenderComparisonView(bool suppress_native_scene,
                                                  Bitu *target_width,
                                                  Bitu *target_height)
{
    if (!OUTPUT_OPENGL_ModRendererAvailable()) {
        if (target_width)
            *target_width = 0;
        if (target_height)
            *target_height = 0;
        return false;
    }

    const ModRenderViewMode comparison_mode = suppress_native_scene
            ? MOD_RENDER_VIEW_SIDE_BY_SIDE_SUPPRESSED
            : MOD_RENDER_VIEW_SIDE_BY_SIDE;
    const ModRenderViewMode next_mode = mod_render_view_mode == comparison_mode
            ? mod_render_single_view_mode
            : comparison_mode;
    return SetModRenderViewMode(next_mode, target_width, target_height);
}

const char *OUTPUT_OPENGL_GetModRenderViewModeName(void)
{
    return GetModRenderViewModeNameInternal(mod_render_view_mode);
}

const char *OUTPUT_OPENGL_GetModRenderViewModeTitleLabel(void)
{
    if (!OUTPUT_OPENGL_ModRendererAvailable())
        return "orig";
    return GetModRenderViewModeTitleLabelInternal(mod_render_view_mode);
}

bool OUTPUT_OPENGL_ModRendererAvailable(void)
{
    return DOSBoxPython_OpenGLRendererAvailable();
}

uint64_t OUTPUT_OPENGL_NotifyModFrameReady(void)
{
    ModPresentationMetricsState &metrics = mod_presentation_metrics;
    const uint32_t now = SDL_GetTicks();
    if (metrics.interval_start_ticks == 0)
        metrics.interval_start_ticks = now;

    metrics.latest_ready_sequence++;
    metrics.latest_ready_ticks = now;
    metrics.mod_count++;
    MOD_TimingCountModFrameReady();
    MOD_FramePacingNotifyReady(metrics.latest_ready_sequence);
    return metrics.latest_ready_sequence;
}

static void RecordNativeFrame(void)
{
    ModPresentationMetricsState &metrics = mod_presentation_metrics;
    const uint32_t now = SDL_GetTicks();
    if (metrics.interval_start_ticks == 0)
        metrics.interval_start_ticks = now;
    metrics.native_count++;
    MOD_TimingCountNativeFrame();
}

void OUTPUT_OPENGL_GetModPresentationMetrics(
        OpenGLModPresentationMetrics *result)
{
    if (!result)
        return;

    ModPresentationMetricsState &metrics = mod_presentation_metrics;
    const uint32_t now = SDL_GetTicks();
    if (metrics.interval_start_ticks == 0)
        metrics.interval_start_ticks = now;

    const uint32_t elapsed_ms = now - metrics.interval_start_ticks;
    if (elapsed_ms >= 400u) {
        const uint64_t rounding = elapsed_ms / 2u;
        metrics.snapshot.native_fps = (uint32_t)(
                (metrics.native_count * 1000u + rounding) / elapsed_ms);
        metrics.snapshot.mod_fps = (uint32_t)(
                (metrics.mod_count * 1000u + rounding) / elapsed_ms);
        metrics.snapshot.presentation_fps = (uint32_t)(
                (metrics.presentation_count * 1000u + rounding) / elapsed_ms);
        metrics.snapshot.latency_valid = metrics.latency_sample_count != 0;
        if (metrics.snapshot.latency_valid) {
            metrics.snapshot.average_latency_ms =
                    metrics.latency_sum_ms / (double)metrics.latency_sample_count;
            metrics.snapshot.maximum_latency_ms = metrics.latency_max_ms;
        } else {
            metrics.snapshot.average_latency_ms = 0.0;
            metrics.snapshot.maximum_latency_ms = 0.0;
        }

        metrics.native_count = 0;
        metrics.mod_count = 0;
        metrics.presentation_count = 0;
        metrics.latency_sample_count = 0;
        metrics.latency_sum_ms = 0.0;
        metrics.latency_max_ms = 0.0;
        metrics.interval_start_ticks = now;
    }

    *result = metrics.snapshot;
}

static bool RecordOpenGLPresentation(const bool compositor_invoked)
{
    ModPresentationMetricsState &metrics = mod_presentation_metrics;
    const uint32_t now = SDL_GetTicks();
    if (metrics.interval_start_ticks == 0)
        metrics.interval_start_ticks = now;

    metrics.presentation_count++;
    if (!compositor_invoked || metrics.latest_ready_sequence == 0 ||
        metrics.latest_ready_sequence == metrics.last_presented_ready_sequence) {
        return false;
    }

    metrics.last_presented_ready_sequence = metrics.latest_ready_sequence;
    const double latency_ms = (double)(now - metrics.latest_ready_ticks);
    metrics.latency_sum_ms += latency_ms;
    metrics.latency_sample_count++;
    metrics.latency_max_ms = std::max(metrics.latency_max_ms, latency_ms);
    return true;
}

static OpenGLPresentationLayout BuildOpenGLPresentationLayout(void)
{
    OpenGLPresentationLayout layout = {};
    uint32_t surface_width = sdl.surface ? (uint32_t)sdl.surface->w : 0u;
    uint32_t surface_height = sdl.surface ? (uint32_t)sdl.surface->h : 0u;

    layout.backbuffer_width = surface_width;
    layout.backbuffer_height = surface_height;
#if defined(C_SDL2)
    if (sdl.window) {
        int drawable_w = 0;
        int drawable_h = 0;
        SDL_GL_GetDrawableSize(sdl.window, &drawable_w, &drawable_h);
        if (drawable_w > 0 && drawable_h > 0) {
            layout.backbuffer_width = (uint32_t)drawable_w;
            layout.backbuffer_height = (uint32_t)drawable_h;
        }
    }
#endif

    if (layout.backbuffer_width == 0u)
        layout.backbuffer_width = surface_width;
    if (layout.backbuffer_height == 0u)
        layout.backbuffer_height = surface_height;

    if (surface_width == 0u)
        surface_width = layout.backbuffer_width;
    if (surface_height == 0u)
        surface_height = layout.backbuffer_height;

    layout.natural_game.x = (GLint)ScaleSurfaceCoordinate((uint32_t)std::max<int>(0, sdl.clip.x),
                                                          surface_width,
                                                          layout.backbuffer_width);
    layout.natural_game.y = (GLint)ScaleSurfaceCoordinate((uint32_t)std::max<int>(0, sdl.clip.y),
                                                          surface_height,
                                                          layout.backbuffer_height);
    layout.natural_game.w = (GLsizei)ScaleSurfaceCoordinate((uint32_t)std::max<int>(0, sdl.clip.w),
                                                            surface_width,
                                                            layout.backbuffer_width);
    layout.natural_game.h = (GLsizei)ScaleSurfaceCoordinate((uint32_t)std::max<int>(0, sdl.clip.h),
                                                            surface_height,
                                                            layout.backbuffer_height);

    if (ViewportIsEmpty(layout.natural_game)) {
        layout.natural_game.x = 0;
        layout.natural_game.y = 0;
        layout.natural_game.w = (GLsizei)layout.backbuffer_width;
        layout.natural_game.h = (GLsizei)layout.backbuffer_height;
    }

    layout.mod.x = 0;
    layout.mod.y = 0;
    layout.mod.w = (GLsizei)layout.backbuffer_width;
    layout.mod.h = (GLsizei)layout.backbuffer_height;
    layout.game = layout.natural_game;

    switch (mod_render_view_mode) {
    case MOD_RENDER_VIEW_MOD_ONLY:
        layout.game = GLViewport();
        break;
    case MOD_RENDER_VIEW_SIDE_BY_SIDE:
    case MOD_RENDER_VIEW_SIDE_BY_SIDE_SUPPRESSED: {
        Bitu configured_width = 0;
        Bitu configured_height = 0;
        if (!GetSideBySideBaseWindowSize(&configured_width,
                                         &configured_height)) {
            configured_width = std::max<uint32_t>(1u,
                    layout.backbuffer_width / 2u);
            configured_height = std::max<uint32_t>(1u,
                    layout.backbuffer_height);
        }

        const uint32_t pane_width = (uint32_t)configured_width;
        const uint32_t pane_height = (uint32_t)configured_height;
        const uint32_t comparison_width = pane_width * 2u;
        const double canvas_scale = std::min(
                1.0,
                std::min((double)layout.backbuffer_width /
                                 (double)comparison_width,
                         (double)layout.backbuffer_height /
                                 (double)pane_height));
        const uint32_t presented_pane_width = std::max<uint32_t>(
                1u, (uint32_t)std::lround((double)pane_width * canvas_scale));
        const uint32_t presented_pane_height = std::max<uint32_t>(
                1u, (uint32_t)std::lround((double)pane_height * canvas_scale));
        const uint32_t presented_comparison_width = presented_pane_width * 2u;
        const uint32_t canvas_x =
                (layout.backbuffer_width - presented_comparison_width) / 2u;
        const uint32_t canvas_y =
                (layout.backbuffer_height - presented_pane_height) / 2u;

        SDL_Rect logical_game = {0, 0, (int)pane_width, (int)pane_height};
        if (sdl_opengl.kind == GLPerfect) {
            logical_game = FitOpenGLPPSourceToBounds((uint16_t)pane_width,
                                                     (uint16_t)pane_height);
        } else if (render.aspect || NativeCrtLetterboxes()) {
            aspectCorrectFitClip(logical_game.w,
                                 logical_game.h,
                                 logical_game.x,
                                 logical_game.y,
                                 (int)pane_width,
                                 (int)pane_height);
        }

        layout.game.x = (GLint)(canvas_x + (uint32_t)std::lround(
                (double)logical_game.x * canvas_scale));
        layout.game.y = (GLint)(canvas_y + (uint32_t)std::lround(
                (double)logical_game.y * canvas_scale));
        layout.game.w = (GLsizei)std::max<uint32_t>(1u,
                (uint32_t)std::lround((double)logical_game.w * canvas_scale));
        layout.game.h = (GLsizei)std::max<uint32_t>(1u,
                (uint32_t)std::lround((double)logical_game.h * canvas_scale));

        layout.mod.x = (GLint)(canvas_x + presented_pane_width);
        layout.mod.y = (GLint)canvas_y;
        layout.mod.w = (GLsizei)presented_pane_width;
        layout.mod.h = (GLsizei)presented_pane_height;
        break;
    }
    case MOD_RENDER_VIEW_GAME_ONLY:
    default:
        break;
    }

    return layout;
}

static GLViewport BuildInactiveModFallbackViewport(
        const OpenGLPresentationLayout &layout)
{
    if (IsModRenderSideBySideMode(mod_render_view_mode)) {
        GLViewport viewport = layout.game;
        viewport.x += layout.mod.w;
        return viewport;
    }
    // Mod-only deliberately has no game presentation viewport, but until the
    // Python renderer publishes its first usable frame it must still fail open
    // to the native game clip rather than presenting a cleared black buffer.
    return layout.natural_game;
}

static void PrepareOpenGLPresentationState(const OpenGLPresentationLayout &layout,
                                           const GLViewport &viewport)
{
    if (dosbox_glBindFramebuffer)
        dosbox_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (dosbox_glActiveTexture)
        dosbox_glActiveTexture(GL_TEXTURE0);
    if (dosbox_glBindVertexArray)
        dosbox_glBindVertexArray(0);
    if (glBindBufferARB) {
        glBindBufferARB(GL_ARRAY_BUFFER, 0);
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, 0);
    }

    if (glUseProgram)
        glUseProgram(sdl_opengl.program_object);

    if (sdl_opengl.use_shader && !ViewportIsEmpty(viewport))
        glViewport(viewport.x, viewport.y, viewport.w, viewport.h);
    else
        glViewport(0, 0, (GLsizei)layout.backbuffer_width, (GLsizei)layout.backbuffer_height);

    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);

    if (sdl_opengl.program_object) {
        if (sdl_opengl.position_attrib >= 0) {
            glVertexAttribPointer((GLuint)sdl_opengl.position_attrib, 2, GL_FLOAT,
                                  GL_FALSE, 0, sdl_opengl.vertex_data);
            glEnableVertexAttribArray((GLuint)sdl_opengl.position_attrib);
        }
        glUniform2f(sdl_opengl.ruby.texture_size,
                    (float)sdl_opengl.texture_size,
                    (float)sdl_opengl.texture_size);
        glUniform2f(sdl_opengl.ruby.input_size,
                    (float)sdl_opengl.input_width,
                    (float)sdl_opengl.input_height);
        glUniform2f(sdl_opengl.ruby.output_size,
                    (GLfloat)std::max<GLsizei>(1, viewport.w),
                    (GLfloat)std::max<GLsizei>(1, viewport.h));
    } else {
        if (glUseProgram)
            glUseProgram(0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0,
                (GLdouble)layout.backbuffer_width,
                (GLdouble)layout.backbuffer_height,
                0,
                -1,
                1);

        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        if (sdl_opengl.texture_size != 0)
            glScaled(1.0 / sdl_opengl.texture_size,
                     1.0 / sdl_opengl.texture_size,
                     1.0);

        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0);
    }
}

static void RestoreOpenGLPresentationState(const OpenGLPresentationLayout &layout)
{
    PrepareOpenGLPresentationState(layout, layout.natural_game);
}

static ModOpenGLState BuildModOpenGLState(const OpenGLPresentationLayout &layout)
{
    ModOpenGLState state = {};
    state.context_generation = sdl_opengl.context_generation;
    state.backbuffer_width = layout.backbuffer_width;
    state.backbuffer_height = layout.backbuffer_height;
    state.backbuffer_framebuffer = 0;
    state.draw_width = (uint32_t)sdl.draw.width;
    state.draw_height = (uint32_t)sdl.draw.height;
    state.view_mode = (uint32_t)mod_render_view_mode;
    state.clip_x = (uint32_t)std::max(0, layout.natural_game.x);
    state.clip_y = (uint32_t)std::max(0, layout.natural_game.y);
    state.clip_w = (uint32_t)std::max(0, layout.natural_game.w);
    state.clip_h = (uint32_t)std::max(0, layout.natural_game.h);
    state.game_viewport_x = (uint32_t)std::max(0, layout.game.x);
    state.game_viewport_y = (uint32_t)std::max(0, layout.game.y);
    state.game_viewport_w = (uint32_t)std::max(0, layout.game.w);
    state.game_viewport_h = (uint32_t)std::max(0, layout.game.h);
    state.mod_viewport_x = (uint32_t)std::max(0, layout.mod.x);
    state.mod_viewport_y = (uint32_t)std::max(0, layout.mod.y);
    state.mod_viewport_w = (uint32_t)std::max(0, layout.mod.w);
    state.mod_viewport_h = (uint32_t)std::max(0, layout.mod.h);
    return state;
}

static bool ShouldDrawNativeCrt(void)
{
    if (!RA_GLSL_HasPreset())
        return false;
    if (IsModRenderSideBySideMode(mod_render_view_mode))
        return kApplyNativeCrtInSideBySide;
    return true;
}

static void DrawDOSBoxTextureToViewport(const OpenGLPresentationLayout &layout,
                                        const GLViewport &viewport)
{
    if (ViewportIsEmpty(viewport))
        return;

    if (ShouldDrawNativeCrt() &&
        RA_GLSL_Draw((unsigned int)sdl_opengl.texture,
                     (int)sdl_opengl.texture_size,
                     (int)sdl_opengl.input_width,
                     (int)sdl_opengl.input_height,
                     (int)viewport.x,
                     (int)viewport.y,
                     (int)viewport.w,
                     (int)viewport.h,
                     (int)sdl_opengl.actual_frame_count++,
                     (unsigned long long)sdl_opengl.context_generation))
        return;

    PrepareOpenGLPresentationState(layout, viewport);
    if (sdl_opengl.program_object) {
        glUniform1i(sdl_opengl.ruby.frame_count, sdl_opengl.actual_frame_count++);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        const GLint right = viewport.x + viewport.w;
        const GLint bottom = viewport.y + viewport.h;
        glBegin(GL_QUADS);
        glTexCoord2i(0, 0); glVertex2i(viewport.x, viewport.y);
        glTexCoord2i((GLint)sdl_opengl.input_width, 0); glVertex2i(right, viewport.y);
        glTexCoord2i((GLint)sdl_opengl.input_width, (GLint)sdl_opengl.input_height); glVertex2i(right, bottom);
        glTexCoord2i(0, (GLint)sdl_opengl.input_height); glVertex2i(viewport.x, bottom);
        glEnd();
    }
}

static void ClearOpenGLBackbuffer(void)
{
    if (dosbox_glBindFramebuffer)
        dosbox_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void ClearOpenGLViewport(const GLViewport &viewport)
{
    if (ViewportIsEmpty(viewport))
        return;

    if (dosbox_glBindFramebuffer)
        dosbox_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor(viewport.x, viewport.y, viewport.w, viewport.h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

static void DrainInactiveModRenderBuffers(const OpenGLPresentationLayout &layout)
{
    const GLViewport fallback = BuildInactiveModFallbackViewport(layout);
    for (int i = 0; i < MOD_RENDER_INACTIVE_POST_SWAP_CLEARS; ++i) {
        ClearOpenGLViewport(layout.mod);
        DrawDOSBoxTextureToViewport(layout, layout.game);
        DrawDOSBoxTextureToViewport(layout, fallback);
        RestoreOpenGLPresentationState(layout);

        if (i + 1 < MOD_RENDER_INACTIVE_POST_SWAP_CLEARS)
            SDL_GL_SwapBuffers();
    }
}

static void FinishOpenGLPresentation(const char *source)
{
    // Guest-call execution re-enters the DOSBox loop without allowing Python
    // compositor callbacks. Never clear or swap a mod backbuffer from that
    // nested context, even if a future caller misses its entry-point guard.
    if (MOD_GuestCallActive())
        return;

    const bool mod_render_active = MOD_RenderActive();
    const OpenGLPresentationLayout layout = BuildOpenGLPresentationLayout();
    const GLViewport fallback = BuildInactiveModFallbackViewport(layout);
    ModOpenGLState mod_state = BuildModOpenGLState(layout);
    mod_state.present_count = ++sdl_opengl.mod_present_count;
    const bool drain_inactive_buffers =
            mod_render_view_mode != MOD_RENDER_VIEW_GAME_ONLY &&
            mod_render_was_active && !mod_render_active;
    bool compositor_invoked = false;
    uint64_t compositor_ns = 0;

    if (mod_render_active)
        DOSBoxPython_InvokeOpenGLInitCallback(mod_state);

    if (mod_render_view_mode != MOD_RENDER_VIEW_GAME_ONLY || NativeCrtLetterboxes())
        ClearOpenGLBackbuffer();

    CheckManagement();
    DrawDOSBoxTextureToViewport(layout, layout.game);

    if (mod_render_view_mode != MOD_RENDER_VIEW_GAME_ONLY) {
        if (mod_render_active) {
            const uint64_t timing_started = MOD_TimingBegin();
            compositor_invoked =
                    DOSBoxPython_InvokeOpenGLCompositorCallback(mod_state);
            compositor_ns = MOD_TimingEnd(MOD_TIMING_COMPOSITOR,
                                          timing_started);
        }

        // MOD_RenderActive reports that the configured executable is running,
        // not that Python has a working compositor. Fail open to the native
        // texture if the callback is absent, disabled, or raises an exception.
        if (!compositor_invoked)
            DrawDOSBoxTextureToViewport(layout, fallback);
    }

    RestoreOpenGLPresentationState(layout);
    const uint64_t swap_started = MOD_TimingBegin();
    SDL_GL_SwapBuffers();
    const uint64_t swap_ns = MOD_TimingEnd(MOD_TIMING_SWAP, swap_started);
    const bool new_mod_frame = RecordOpenGLPresentation(compositor_invoked);
    MOD_TimingPresentationBoundary(compositor_invoked,
                                   new_mod_frame,
                                   compositor_ns,
                                   swap_ns,
                                   source);

    if (drain_inactive_buffers)
        DrainInactiveModRenderBuffers(layout);

    mod_render_was_active = mod_render_active;
}

bool OUTPUT_OPENGL_ModPresentationRequired(void)
{
    return !MOD_GuestCallActive() &&
           !MOD_FramePacingOwnsPresentation() &&
           mod_render_view_mode != MOD_RENDER_VIEW_GAME_ONLY &&
           (MOD_RenderActive() || mod_render_was_active);
}

void OUTPUT_OPENGL_PresentModFrame(void)
{
    if (!OUTPUT_OPENGL_ModPresentationRequired())
        return;

    FinishOpenGLPresentation("vga");
    if (!menu.hidecycles && !sdl.desktop.fullscreen)
        frames++;
}

bool OUTPUT_OPENGL_PresentReadyModFrame(void)
{
    if (MOD_GuestCallActive())
        return false;

    uint64_t ready_sequence = 0;
    if (!MOD_FramePacingTakePresentation(&ready_sequence))
        return false;

    FinishOpenGLPresentation("mod-ready");
    MOD_FramePacingPresented(ready_sequence);
    if (!menu.hidecycles && !sdl.desktop.fullscreen)
        frames++;
    return true;
}

void OUTPUT_OPENGL_EndUpdate(const uint16_t *changedLines)
{
    if (!(sdl.must_redraw_all && changedLines == NULL)) 
    {
#if C_XBRZ
        if (sdl_xbrz.enable && sdl_xbrz.scale_on)
        {
            // OpenGL pixel buffer is precreated for direct xBRZ output, while xBRZ render buffer is used for rendering
            const uint32_t srcWidth = sdl.draw.width;
            const uint32_t srcHeight = sdl.draw.height;

            if (sdl_xbrz.renderbuf.size() == (unsigned int)srcWidth * (unsigned int)srcHeight && srcWidth > 0 && srcHeight > 0)
            {
                // we assume render buffer is *not* scaled!
                const uint32_t* renderBuf = &sdl_xbrz.renderbuf[0]; // help VS compiler a little + support capture by value
                uint32_t* trgTex;
                if (sdl_opengl.pixel_buffer_object) 
                {
                    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, sdl_opengl.buffer);
                    trgTex = (uint32_t *)glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, GL_WRITE_ONLY);
                }
                else
                {
                    trgTex = reinterpret_cast<uint32_t*>(sdl_opengl.framebuf);
                }

                if (trgTex)
                    xBRZ_Render(renderBuf, trgTex, changedLines, (int)srcWidth, (int)srcHeight, sdl_xbrz.scale_factor);
            }

            // and here we go repeating some stuff with xBRZ related modifications
            if (sdl_opengl.pixel_buffer_object)
            {
                glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT);
                glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    (int)(sdl.draw.width * (unsigned int)sdl_xbrz.scale_factor), (int)(sdl.draw.height * (unsigned int)sdl_xbrz.scale_factor), GL_BGRA_EXT,
#if defined (MACOSX) && !defined(C_SDL2)
                    // needed for proper looking graphics on macOS 10.12, 10.13
                    GL_UNSIGNED_INT_8_8_8_8,
#else
                    GL_UNSIGNED_INT_8_8_8_8_REV,
#endif
                    nullptr);
                glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, 0);
            }
            else
            {
                glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    (int)(sdl.draw.width * (unsigned int)sdl_xbrz.scale_factor), (int)(sdl.draw.height * (unsigned int)sdl_xbrz.scale_factor), GL_BGRA_EXT,
#if defined (MACOSX) && !defined(C_SDL2)
                    // needed for proper looking graphics on macOS 10.12, 10.13
                    GL_UNSIGNED_INT_8_8_8_8,
#else
                    // works on Linux
                    GL_UNSIGNED_INT_8_8_8_8_REV,
#endif
                    (uint8_t *)sdl_opengl.framebuf);
            }
            RecordNativeFrame();
        }
        else
#endif /*C_XBRZ*/
        if (sdl_opengl.pixel_buffer_object) 
        {
            if (changedLines && (changedLines[0] == sdl.draw.height))
                return;

            glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT);
            glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                (int)sdl.draw.width, (int)sdl.draw.height, GL_BGRA_EXT,
#if defined (MACOSX)
                // needed for proper looking graphics on macOS 10.12, 10.13
                GL_UNSIGNED_INT_8_8_8_8,
#else
                // works on Linux
                GL_UNSIGNED_INT_8_8_8_8_REV,
#endif
                (void*)0);
            glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, 0);
            RecordNativeFrame();
            //glCallList(sdl_opengl.displaylist);
            //SDL_GL_SwapBuffers();
        }
        else if (changedLines) 
        {
            if (changedLines[0] == sdl.draw.height)
                return;

            Bitu y = 0, index = 0;
            glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);
            while (y < sdl.draw.height) 
            {
                if (!(index & 1)) 
                {
                    y += changedLines[index];
                }
                else 
                {
                    uint8_t *pixels = (uint8_t *)sdl_opengl.framebuf + y * sdl_opengl.pitch;
                    Bitu height = changedLines[index];

                    // FIXME: This is causing libgallium crashes by sometimes getting y+height > sdl.draw.height.
                    //        It seems to happen on VGA mode changes.
                    //        Figure out what is causing that.
                    //        I have to see any crash from y >= sdl.draw.height though.
                    if ((y+height) > sdl.draw.height) {
                        LOG(LOG_MISC,LOG_WARN)("OpenGL: Changed lines extend past texture (y=%u h=%u drawheight=%u y+h=%u y+h>drawheight)",
                            (unsigned int)y,(unsigned int)height,(unsigned int)(y+height),(unsigned int)sdl.draw.height);
                        height = sdl.draw.height - y;
                    }

                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (int)y,
                        (int)sdl.draw.width, (int)height, GL_BGRA_EXT,
#if defined (MACOSX) && !defined(C_SDL2)
                        // needed for proper looking graphics on macOS 10.12, 10.13
                        GL_UNSIGNED_INT_8_8_8_8,
#else
                        // works on Linux
                        GL_UNSIGNED_INT_8_8_8_8_REV,
#endif
                        (void*)pixels);
                    y += height;
                }
                index++;
            }
            RecordNativeFrame();
        } else
            return;

        if (!MOD_GuestCallActive() &&
            !MOD_FramePacingOwnsPresentation())
            FinishOpenGLPresentation("vga");

#if 0 /* DEBUG Prove to me that you're drawing the damn texture */
        glBindTexture(GL_TEXTURE_2D, SDLDrawGenFontTexture);

	glPushMatrix();

	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glScaled(1.0 / SDLDrawGenFontTextureWidth, 1.0 / SDLDrawGenFontTextureHeight, 1.0);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_ALPHA_TEST);
	glEnable(GL_BLEND);

	glBegin(GL_QUADS);

	glTexCoord2i(0, 0); glVertex2i(0, 0); // lower left
	glTexCoord2i(SDLDrawGenFontTextureWidth, 0); glVertex2i(SDLDrawGenFontTextureWidth, 0); // lower right
	glTexCoord2i(SDLDrawGenFontTextureWidth, SDLDrawGenFontTextureHeight); glVertex2i(SDLDrawGenFontTextureWidth, SDLDrawGenFontTextureHeight); // upper right
	glTexCoord2i(0, SDLDrawGenFontTextureHeight); glVertex2i(0, SDLDrawGenFontTextureHeight); // upper left

	glEnd();

	glBlendFunc(GL_ONE, GL_ZERO);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_TEXTURE_2D);

	glPopMatrix();

	glBindTexture(GL_TEXTURE_2D, sdl_opengl.texture);
#endif

        if (!MOD_GuestCallActive() &&
            !MOD_FramePacingOwnsPresentation() &&
            !menu.hidecycles && !sdl.desktop.fullscreen) {
            frames++;
        }
    }
}

void OUTPUT_OPENGL_Shutdown()
{
	RA_GLSL_Release();
	MOD_SetFramePacingViewEligible(false);
	if (sdl_opengl.pixel_buffer_object)
	{
		glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_EXT, 0);
		if (sdl_opengl.buffer) glDeleteBuffersARB(1, &sdl_opengl.buffer);
		sdl_opengl.buffer = 0;
	}
	if (sdl_opengl.framebuf != NULL) {
		free(sdl_opengl.framebuf);
		sdl_opengl.framebuf = NULL;
	}
}

#endif
