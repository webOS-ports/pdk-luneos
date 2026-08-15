// sdl_webos_shim - the Palm-specific parts of webOS SDL 1.2, on top of sdl12-compat.
//
// Legacy PDK apps link libSDL-1.2.so.0 and expect Palm's additions on top of stock
// SDL 1.2: the SDL_GLES_* entry points, the SDL_OPENGLES video flags, and the
// SDL_WebOsHook* family (which libpdl also calls).
//
// This builds AS libSDL-1.2.so.0. The real sdl12-compat is renamed to
// libSDL12compat.so.0 (patchelf --set-soname) and listed as our NEEDED, so every
// stock SDL_* symbol still resolves through the dependency graph - we only have to
// define the extras plus the few calls we intercept.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define SDL_OPENGL       0x00000002
#define SDL_OPENGLES     0x00000040   // Palm addition
#define SDL_OPENGLESBLIT 0x00000048   // Palm addition (implies OPENGLES)
#define SDL_FULLSCREEN   0x80000000

// Public SDL 1.2 surface head. sdl12-compat keeps this layout for ABI reasons, so
// the first fields can be read and adjusted safely.
typedef struct SDL_Surface {
    uint32_t  flags;        // 0x00
    void     *format;       // 0x04
    int       w, h;         // 0x08, 0x0c
    uint16_t  pitch;        // 0x10
    void     *pixels;       // 0x14
    int       offset;       // 0x18
} SDL_Surface;

// stock SDL 1.2 entry points, resolved from sdl12-compat at load time
extern int   SDL_GL_LoadLibrary(const char *path);
extern void *SDL_GL_GetProcAddress(const char *proc);
extern int   SDL_GL_SetAttribute(int attr, int value);

typedef SDL_Surface *(*setvideomode_fn)(int, int, int, uint32_t);

// defined further down; used by SDL_SetVideoMode to resolve "native resolution"
int SDL_WebOsHookGetDisplayRect(int *x, int *y, int *w, int *h);

// defined with the mouse handling further down; used by the event wrappers to put
// event coordinates back into the game's own space
static int scale_mouse(int *x, int *y);

static int   s_orientation    = 0;
static int   s_gestures       = 1;
static int   s_screen_timeout = 1;
static int   s_banner_msgs    = 1;
static int   s_compass        = 0;
static int   s_pause_ui       = 0;

// The mode the game last asked for. Mouse coordinates arrive from sdl12-compat in
// window pixels and have to be mapped back into this space; see scale_mouse().
int pdkshim_mode_w;
int pdkshim_mode_h;

static void (*s_resize_cb)(int, int);
static void (*s_activate_cb)(int);
static void (*s_paused_cb)(int);

static int verbose(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PDK_SHIM_DEBUG") ? 1 : 0;
    return v;
}


// How many input lines to trace before going quiet. A game polls every frame, so
// this has to be bounded, but 40 is not enough to catch "the button I pressed" in
// a long session - PDK_TRACE_MAX raises it.
static int trace_max(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("PDK_TRACE_MAX");
        v = s ? atoi(s) : 40;
        if (v < 0) v = 0;
    }
    return v;
}

#define TRACE(...) do { if (verbose()) { \
    fprintf(stderr, "[sdlshim] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---------------------------------------------------------------- SDL_GLES_*
//
// On webOS these were a separate GLES-specific loader. sdl12-compat's SDL_GL_*
// already drives EGL/GLES underneath, so forward straight to it.

int SDL_GLES_LoadLibrary(const char *path)
{
    TRACE("SDL_GLES_LoadLibrary(%s)", path ? path : "(default)");
    return SDL_GL_LoadLibrary(path);
}

void *SDL_GLES_GetProcAddress(const char *proc)
{
    return SDL_GL_GetProcAddress(proc);
}

int SDL_GLES_SetAttribute(int attr, int value)
{
    return SDL_GL_SetAttribute(attr, value);
}

// ------------------------------------------------------------ SDL_SetVideoMode
//
// Translate the Palm-only GLES flags into plain SDL_OPENGL. We define this symbol
// ourselves so it takes precedence over sdl12-compat's copy; the real one is reached
// via the extern declaration, which the linker binds to the NEEDED library.

// SDL2 attribute ids (SDL_GLattr) and profile bits. We reach past sdl12-compat to
// SDL2 directly because SDL 1.2 has no concept of a context profile.
#define SDL2_GL_CONTEXT_MAJOR_VERSION 17
#define SDL2_GL_CONTEXT_MINOR_VERSION 18
#define SDL2_GL_CONTEXT_PROFILE_MASK  21
#define SDL2_GL_CONTEXT_PROFILE_ES    0x0004

// PDK apps are GLES apps: their shaders are GLSL ES and use precision qualifiers
// ("varying highp vec3 ..."), which a desktop GL context rejects outright.
// sdl12-compat always asks SDL2 for desktop GL and exposes no override, so request
// an ES profile ourselves before the context gets created.
static void request_gles_profile(void)
{
    if (getenv("PDK_NO_GLES"))          // escape hatch
        return;

    void *sdl2 = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!sdl2) { TRACE("no libSDL2 handle: %s", dlerror()); return; }

    int (*set_attr)(int, int) = (int (*)(int, int))dlsym(sdl2, "SDL_GL_SetAttribute");
    if (!set_attr) { TRACE("no SDL2 SDL_GL_SetAttribute"); return; }

    // GLES1 titles (the majority - they link libGLES_CM) need an ES 1.1 context,
    // GLES2 ones need 2.0. Detect via a GLES1-only entry point.
    int major = 2;
    const char *forced = getenv("PDK_GLES_VERSION");
    if (forced)
        major = atoi(forced);
    else if (dlsym(RTLD_DEFAULT, "glOrthof"))
        major = 1;

    set_attr(SDL2_GL_CONTEXT_PROFILE_MASK, SDL2_GL_CONTEXT_PROFILE_ES);
    set_attr(SDL2_GL_CONTEXT_MAJOR_VERSION, major);
    set_attr(SDL2_GL_CONTEXT_MINOR_VERSION, major == 1 ? 1 : 0);
    TRACE("requested OpenGL ES %d.%d context", major, major == 1 ? 1 : 0);
}

SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, uint32_t flags)
{
    static setvideomode_fn real;
    if (!real) {
        real = (setvideomode_fn)dlsym(RTLD_NEXT, "SDL_SetVideoMode");
        if (!real) {
            fprintf(stderr, "[sdlshim] FATAL: no SDL_SetVideoMode behind us (%s)\n",
                    dlerror());
            return NULL;
        }
    }

    if (flags & (SDL_OPENGLES | SDL_OPENGLESBLIT)) {
        TRACE("SetVideoMode: mapping Palm GLES flags 0x%x -> SDL_OPENGL", flags);
        flags = (flags & ~(uint32_t)SDL_OPENGLESBLIT) | SDL_OPENGL;
    }
    if (flags & SDL_OPENGL)
        request_gles_profile();

    {
        int sw = 0, sh = 0;
        SDL_WebOsHookGetDisplayRect(NULL, NULL, &sw, &sh);

        // On webOS, 0 width/height meant "give me the native resolution". Stock
        // SDL 1.2 has no such convention and sdl12-compat passes it through to
        // SDL2, which cannot create a 0x0 window - so resolve it here.
        if (w <= 0 || h <= 0) {
            TRACE("SetVideoMode: native-resolution request (%dx%d) -> %dx%d", w, h, sw, sh);
            w = sw;
            h = sh;
        }

        // Some titles then ask for a mode larger than the panel (Make a Scene asks
        // for 1920x1080 on a 1024x768 screen). Palm's SDL clamped to the device;
        // SDL2 fails the request and returns NULL, and callers that do not check
        // dereference it and die. Clamp so they get a usable surface instead.
        if (!getenv("PDK_NO_CLAMP") && sw > 0 && sh > 0 && (w > sw || h > sh)) {
            TRACE("SetVideoMode: clamping oversized %dx%d -> %dx%d", w, h, sw, sh);
            w = sw;
            h = sh;
        }

        // And the opposite case: a title that hardcodes a Palm-era mode (Monopoly
        // asks for its own size, not 0x0) gets a window of exactly that size, which
        // on a 800x1280 panel is a small rectangle in the corner. Palm never had to
        // deal with this - the mode always matched the device.
        //
        // Ask for fullscreen rather than stretching the mode itself: sdl12-compat
        // then keeps the 1.2 surface at the size the game asked for and scales it to
        // the output, preserving aspect ratio, which is what these fixed-layout
        // titles need. Growing w/h instead would hand the game a bigger surface than
        // its artwork and UI hit-boxes were built for.
        //
        // Only for windowed modes that are genuinely smaller than the panel, so a
        // title that already matches the display is untouched.
        if (!getenv("PDK_NO_UPSCALE") && !(flags & SDL_FULLSCREEN)
            && sw > 0 && sh > 0 && w > 0 && h > 0 && (w < sw || h < sh)) {
            TRACE("SetVideoMode: upscaling %dx%d to fill %dx%d (fullscreen)",
                  w, h, sw, sh);
            flags |= SDL_FULLSCREEN;
        }

        // Tell the GLES shim what the game thinks its screen is. A GL title draws
        // into the window through its own context, so sdl12-compat's surface
        // scaling never touches it and libGLES_CM.so has to scale the viewport
        // instead - which it cannot do without knowing this size. Same process,
        // so the environment is the whole channel.
        if (flags & SDL_OPENGL) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", w);
            setenv("PDK_GL_MODE_WIDTH", buf, 1);
            snprintf(buf, sizeof buf, "%d", h);
            setenv("PDK_GL_MODE_HEIGHT", buf, 1);
        }

        // Also kept in-process for scale_mouse(), which needs it for every title
        // rather than only the GL ones.
        pdkshim_mode_w = w;
        pdkshim_mode_h = h;
    }
    if (bpp <= 0)
        bpp = 32;

    // Optional, OFF by default: hand back the existing surface when a mode-set is
    // identical, instead of rebuilding the window. Seemed like a good idea to damp
    // repeated mode-sets, but engines that legitimately re-set every frame expect a
    // real reinit and crash when they get a recycled surface. Opt in per title.
    static SDL_Surface *cached;
    static int c_w, c_h, c_bpp;
    static uint32_t c_flags;

    if (cached && w == c_w && h == c_h && bpp == c_bpp && flags == c_flags
        && getenv("PDK_MODE_CACHE")) {
        TRACE("SetVideoMode(%d,%d,%d,0x%x) - identical, reusing surface %p",
              w, h, bpp, flags, (void *)cached);
        return cached;
    }

    TRACE("SetVideoMode(%d,%d,%d,0x%x)", w, h, bpp, flags);
    SDL_Surface *s = real(w, h, bpp, flags);
    if (s) { cached = s; c_w = w; c_h = h; c_bpp = bpp; c_flags = flags; }

    // Diagnostic only. Checked and ruled out as the cause of the FIFA/NFS SIGFPE:
    // sdl12-compat already reports a correct pitch for GL surfaces (pixels is NULL,
    // which is normal for SDL_OPENGL).
    if (s)
        TRACE("  -> surface %dx%d pitch=%u pixels=%p", s->w, s->h,
              (unsigned)s->pitch, s->pixels);
    else
        TRACE("  -> SetVideoMode FAILED (NULL)");
    return s;
}

// ------------------------------------------------------------- event filtering
//
// webOS PDK apps were always fullscreen at a fixed device resolution, so they were
// never written to handle a resize sensibly. Under a real compositor sdl12-compat
// does emit SDL_VIDEORESIZE, and engines that respond by calling SetVideoMode again
// end up in a feedback loop. Drop those events by default; PDK_ALLOW_RESIZE keeps
// them for anything that genuinely wants to react.

#define SDL12_VIDEORESIZE 16
// SDL 1.2's SDL_EventType, for the input trace below.
#define SDL12_KEYDOWN          2
#define SDL12_KEYUP            3
#define SDL12_MOUSEMOTION      4
#define SDL12_MOUSEBUTTONDOWN  5
#define SDL12_MOUSEBUTTONUP    6

typedef int (*pollevent_fn)(void *);
typedef int (*waitevent_fn)(void *);

// Whether to swallow SDL_VIDEORESIZE. This is genuinely per-engine, not a global
// win: most titles were written for a fixed fullscreen device and mishandle a
// resize (Monopoly, Tiger Woods and Bubble Bash all need these dropped), but
// Haxe/NME re-sets the video mode ~11x a second as normal operation and breaks if
// its resize events disappear. Default on, per-app opt-out via pdk.env:
//     PDK_ALLOW_RESIZE=1
static int drop_resize(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PDK_ALLOW_RESIZE") ? 0 : 1;
    return v;
}

// SDL 1.2's SDL_Event has the type in its first byte.
static int is_resize(const void *ev)
{
    return ev && *(const unsigned char *)ev == SDL12_VIDEORESIZE;
}

// What actually reaches the game, when PDK_SHIM_DEBUG is on. Touch arriving at
// the client (wl_touch.down in WAYLAND_DEBUG) says nothing about whether it
// survives SDL2's synthetic-mouse step and sdl12-compat's translation, and those
// are three different bugs in three different places. Capped so a game polling
// every frame cannot flood the journal.
static void trace_event(const void *ev, const char *from)
{
    static int seen;
    if (!ev || seen >= trace_max()) return;
    unsigned char type = *(const unsigned char *)ev;
    if (type == SDL12_MOUSEMOTION || type == SDL12_MOUSEBUTTONDOWN ||
        type == SDL12_MOUSEBUTTONUP) {
        // Both SDL 1.2 mouse event structs put Uint16 x,y at offsets 4 and 6:
        // three Uint8 fields, then the Uint16 pair lands on its alignment.
        const unsigned char *b = ev;
        unsigned x = (unsigned)b[4] | ((unsigned)b[5] << 8);
        unsigned y = (unsigned)b[6] | ((unsigned)b[7] << 8);
        TRACE("%s: type=%u at %u,%u", from, type, x, y);
        seen++;
    } else if (type == SDL12_KEYDOWN || type == SDL12_KEYUP) {
        TRACE("%s: key event type=%u", from, type);
        seen++;
    }
}

// Mouse events carry the same unscaled, clamped position as the polled state, so
// a title that reads events instead of polling gets its clicks in the wrong place
// - which is how a game can feel responsive everywhere and still have its OK and
// Back buttons do nothing. Rewrite the coordinates in place.
//
// The event's own x,y cannot be repaired (they are already clamped), so this uses
// the pointer position SDL2 has right now. For a tap that is the position the
// event refers to; a fast drag can be a frame stale, which is the trade for not
// having to reimplement sdl12-compat's event translation.
static void fix_event_coords(void *ev)
{
    if (!ev) return;
    unsigned char *b = ev;
    unsigned char type = b[0];
    if (type != SDL12_MOUSEMOTION && type != SDL12_MOUSEBUTTONDOWN &&
        type != SDL12_MOUSEBUTTONUP)
        return;

    int x = 0, y = 0;
    if (!scale_mouse(&x, &y)) return;

    if (type == SDL12_MOUSEMOTION) {
        // xrel,yrel follow x,y as Sint16; scale them by the same ratio so drags
        // move by the distance the game expects.
        int sw = 0, sh = 0;
        SDL_WebOsHookGetDisplayRect(NULL, NULL, &sw, &sh);
        short xrel = (short)((unsigned)b[8] | ((unsigned)b[9] << 8));
        short yrel = (short)((unsigned)b[10] | ((unsigned)b[11] << 8));
        if (sw > 0 && sh > 0) {
            xrel = (short)((long)xrel * pdkshim_mode_w / sw);
            yrel = (short)((long)yrel * pdkshim_mode_h / sh);
        }
        b[8] = (unsigned char)(xrel & 0xff);
        b[9] = (unsigned char)((xrel >> 8) & 0xff);
        b[10] = (unsigned char)(yrel & 0xff);
        b[11] = (unsigned char)((yrel >> 8) & 0xff);
    }

    b[4] = (unsigned char)(x & 0xff);
    b[5] = (unsigned char)((x >> 8) & 0xff);
    b[6] = (unsigned char)(y & 0xff);
    b[7] = (unsigned char)((y >> 8) & 0xff);
}

int SDL_PollEvent(void *event)
{
    static pollevent_fn real;
    if (!real) real = (pollevent_fn)dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!real) return 0;

    for (;;) {
        int r = real(event);
        if (r > 0) { fix_event_coords(event); trace_event(event, "PollEvent"); }
        if (r <= 0 || !drop_resize() || !is_resize(event))
            return r;
        TRACE("dropping SDL_VIDEORESIZE");
        if (!event) return r;      // caller only asked whether an event exists
    }
}

int SDL_WaitEvent(void *event)
{
    static waitevent_fn real;
    if (!real) real = (waitevent_fn)dlsym(RTLD_NEXT, "SDL_WaitEvent");
    if (!real) return 0;

    for (;;) {
        int r = real(event);
        if (r > 0) { fix_event_coords(event); trace_event(event, "WaitEvent"); }
        if (r <= 0 || !drop_resize() || !is_resize(event))
            return r;
        TRACE("dropping SDL_VIDEORESIZE (wait)");
    }
}

// -------------------------------------------------------------- SDL_WebOsHook*
//
// libpdl calls these to push window properties into the compositor. On LuneOS the
// PIpc host owns that, so record the state and report success; the values are
// observable through the getters below for anything that wants them.

int SDL_WebOsHookSetOrientation(int orientation)
{
    TRACE("SetOrientation(%d)", orientation);
    s_orientation = orientation;
    return 0;
}

int SDL_WebOsHookSetGesturesEnable(int enable)
{
    TRACE("SetGesturesEnable(%d)", enable);
    s_gestures = enable;
    return 0;
}

int SDL_WebOsHookScreenTimeoutEnable(int enable)
{
    TRACE("ScreenTimeoutEnable(%d)", enable);
    s_screen_timeout = enable;
    return 0;
}

int SDL_WebOsHookBannerMessagesEnable(int enable)
{
    TRACE("BannerMessagesEnable(%d)", enable);
    s_banner_msgs = enable;
    return 0;
}

int SDL_WebOsHookCustomPauseUiEnable(int enable)
{
    TRACE("CustomPauseUiEnable(%d)", enable);
    s_pause_ui = enable;
    return 0;
}

int SDL_WebOsHookEnableCompass(int enable)
{
    TRACE("EnableCompass(%d)", enable);
    s_compass = enable;
    return 0;
}

int SDL_WebOsHookIsComponent(void)
{
    return 0;   // never launched as an embedded plugin here
}

// The size the game is actually drawing into, asked of SDL2 rather than assumed.
//
// pdk-run reads the panel from DRM, which is the *physical* mode and never
// changes. LSM rotates by putting a QML Rotation transform around the whole
// shell and swapping the shell's logical size, so on rotation a card is
// reconfigured from 800x1280 to landscape - and every client, including us, is
// simply given a different surface size. Nothing tells us the panel is now
// "sideways", and there is no wl_output transform to read: as far as the client
// is concerned the screen just changed shape.
//
// So trust the window, not the panel. Everything the shim derives from the
// screen size - whether a mode is undersized and should go fullscreen, and how
// touch coordinates map back into the game's space - then follows rotation for
// free, because the window follows it.
static int sdl2_window_size(int *w, int *h)
{
    static void *(*get_current_gl_window)(void);
    static void *(*get_mouse_focus)(void);
    static void (*get_window_size)(void *, int *, int *);
    static int probed;

    if (!probed) {
        probed = 1;
        void *sdl2 = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (sdl2) {
            get_current_gl_window = (void *(*)(void))dlsym(sdl2, "SDL_GL_GetCurrentWindow");
            get_mouse_focus = (void *(*)(void))dlsym(sdl2, "SDL_GetMouseFocus");
            get_window_size = (void (*)(void *, int *, int *))dlsym(sdl2, "SDL_GetWindowSize");
        }
        if (!get_window_size)
            TRACE("no SDL2 SDL_GetWindowSize; falling back to the panel size");
    }
    if (!get_window_size)
        return 0;

    // GL titles have a current context; the rest are found through mouse focus.
    void *win = get_current_gl_window ? get_current_gl_window() : NULL;
    if (!win && get_mouse_focus)
        win = get_mouse_focus();
    if (!win)
        return 0;

    int ww = 0, wh = 0;
    get_window_size(win, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return 0;

    if (w) *w = ww;
    if (h) *h = wh;
    return 1;
}

int SDL_WebOsHookGetDisplayRect(int *x, int *y, int *w, int *h)
{
    if (x) *x = 0;
    if (y) *y = 0;

    // The live window wins once there is one. PDK_SCREEN_* stays the fallback:
    // SDL_SetVideoMode asks for this before any window exists, and that is
    // exactly when the panel size is the right answer.
    int ww = 0, wh = 0;
    if (!getenv("PDK_NO_WINDOW_SIZE") && sdl2_window_size(&ww, &wh)) {
        if (w) *w = ww;
        if (h) *h = wh;
        return 0;
    }

    // Reported by the PIpc host at window creation; env lets it be overridden.
    const char *ws = getenv("PDK_SCREEN_WIDTH");
    const char *hs = getenv("PDK_SCREEN_HEIGHT");
    if (w) *w = ws ? atoi(ws) : 1024;
    if (h) *h = hs ? atoi(hs) : 768;
    return 0;
}

const char *SDL_WebOsHookGetAlsaDeviceName(void)
{
    const char *dev = getenv("PDK_ALSA_DEVICE");
    return dev ? dev : "default";
}

int SDL_WebOsHookLSCall(const char *uri, const char *payload)
{
    // libpdl owns the real luna-service2 path now; keep this as a no-op so any
    // direct caller degrades instead of crashing.
    TRACE("LSCall(%s, %s) - ignored, use libpdl", uri ? uri : "", payload ? payload : "");
    return 0;
}

int SDL_WebOsHookRegisterResizeCallback(void (*cb)(int, int))
{
    s_resize_cb = cb;
    return 0;
}

int SDL_WebOsHookRegisterActivateCallback(void (*cb)(int))
{
    s_activate_cb = cb;
    return 0;
}

int SDL_WebOsHookRegisterPausedCallback(void (*cb)(int))
{
    s_paused_cb = cb;
    return 0;
}

// ------------------------------------------------- Palm multi-touch extension
//
// Palm added a "multi mouse" API so games could read several touch points.
// sdl12-compat has no equivalent, so report point 0 from the normal mouse state
// and nothing for the rest. Good enough for single-touch play; Azada HD and
// Bubble Bash link these and will not start without them.

typedef unsigned char Uint8;
typedef Uint8 (*mousestate_fn)(int *, int *);

// Mouse position, mapped back into the coordinate space the game believes in.
//
// A title that hardcodes 320x480 gets a fullscreen 800x1280 window, and
// sdl12-compat hands its 1.2 surface the pointer position in *window* pixels,
// clamped to the surface bounds. So a tap anywhere past x=320 or y=480 - which is
// most of the panel - arrives as exactly (320,480), and the game reads every touch
// as the bottom-right corner. This is the input half of the same problem the GLES
// shim fixes for pixels: sdl12-compat is not scaling this title, so nothing maps.
//
// Ask SDL2 directly for the unclamped window position and scale it ourselves. The
// clamped value cannot be repaired after the fact - the information is gone by the
// time it reaches us - which is why this reaches past sdl12-compat rather than
// adjusting what it returns.
static int sdl2_mouse_state(int *wx, int *wy)
{
    static unsigned (*fn)(int *, int *);
    static int probed;
    if (!probed) {
        probed = 1;
        void *sdl2 = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (sdl2) fn = (unsigned (*)(int *, int *))dlsym(sdl2, "SDL_GetMouseState");
        if (!fn) TRACE("no SDL2 SDL_GetMouseState; mouse stays unscaled");
    }
    if (!fn) return -1;
    return (int)fn(wx, wy);
}

// The mode the game asked for, recorded by SDL_SetVideoMode (see below).
extern int pdkshim_mode_w;
extern int pdkshim_mode_h;


static int scale_mouse(int *x, int *y)
{
    int sw = 0, sh = 0;
    if (getenv("PDK_NO_INPUT_SCALE")) return 0;
    if (pdkshim_mode_w <= 0 || pdkshim_mode_h <= 0) return 0;
    SDL_WebOsHookGetDisplayRect(NULL, NULL, &sw, &sh);
    if (sw <= 0 || sh <= 0) return 0;
    if (sw == pdkshim_mode_w && sh == pdkshim_mode_h) return 0;  // nothing to map

    int wx = 0, wy = 0;
    if (sdl2_mouse_state(&wx, &wy) < 0) return 0;

    int mw = pdkshim_mode_w, mh = pdkshim_mode_h;
    int lx = (int)((long)wx * mw / sw);
    int ly = (int)((long)wy * mh / sh);


    if (x) *x = lx < 0 ? 0 : (lx > mw - 1 ? mw - 1 : lx);
    if (y) *y = ly < 0 ? 0 : (ly > mh - 1 ? mh - 1 : ly);
    return 1;
}

Uint8 SDL_GetMouseState(int *x, int *y)
{
    static mousestate_fn real;
    if (!real) real = (mousestate_fn)dlsym(RTLD_NEXT, "SDL_GetMouseState");
    if (!real) { if (x) *x = 0; if (y) *y = 0; return 0; }
    Uint8 r = real(x, y);
    int scaled = scale_mouse(x, y);
    if (verbose()) {
        static int seen;
        static int lx = -1, ly = -1;
        int cx = x ? *x : -1, cy = y ? *y : -1;
        if (seen < trace_max() && (r || cx != lx || cy != ly)) {
            TRACE("GetMouseState -> %d,%d buttons=0x%x%s", cx, cy, (unsigned)r,
                  scaled ? " (scaled)" : "");
            lx = cx; ly = cy; seen++;
        }
    }
    return r;
}

Uint8 SDL_GetMultiMouseState(int index, int *x, int *y)
{
    static mousestate_fn real;
    if (!real) real = (mousestate_fn)dlsym(RTLD_NEXT, "SDL_GetMouseState");
    if (index != 0 || !real) { if (x) *x = 0; if (y) *y = 0; return 0; }
    Uint8 r = real(x, y);
    if (verbose()) {
        static int seen;
        if (seen < 20) {
            TRACE("GetMultiMouseState(%d) -> %d,%d buttons=0x%x",
                  index, x ? *x : -1, y ? *y : -1, (unsigned)r);
            seen++;
        }
    }
    return r;
}

Uint8 SDL_GetRelativeMultiMouseState(int index, int *x, int *y)
{
    static mousestate_fn real;
    if (!real) real = (mousestate_fn)dlsym(RTLD_NEXT, "SDL_GetRelativeMouseState");
    if (index != 0 || !real) { if (x) *x = 0; if (y) *y = 0; return 0; }
    return real(x, y);
}

// ------------------------------------------------------- Palm haptics stubs
//
// SDL 1.2 has no haptics; Palm backported the SDL2 API for the vibrator. Report
// "no haptic device" - callers already handle that (NFS logs a PVibraControl
// error and carries on). Games still need the symbols to resolve at load time.
// PDL_Vibrate remains the working path for rumble.

void *SDL_HapticOpen(int device_index)          { (void)device_index; return NULL; }
void  SDL_HapticClose(void *h)                  { (void)h; }
int   SDL_HapticNewEffect(void *h, void *e)     { (void)h; (void)e; return -1; }
int   SDL_HapticRunEffect(void *h, int e, unsigned int it) { (void)h; (void)e; (void)it; return -1; }
int   SDL_HapticStopEffect(void *h, int e)      { (void)h; (void)e; return -1; }
int   SDL_HapticUpdateEffect(void *h, int e, void *d) { (void)h; (void)e; (void)d; return -1; }
int   SDL_HapticIndex(void *h)                  { (void)h; return -1; }
int   SDL_HapticQuery(void *h)                  { (void)h; return 0; }
int   SDL_NumHaptics(void)                      { return 0; }

// Entry points the PIpc host side can drive once wired up.
void PDKSHIM_NotifyResize(int w, int h)   { if (s_resize_cb)   s_resize_cb(w, h); }
void PDKSHIM_NotifyActivate(int active)   { if (s_activate_cb) s_activate_cb(active); }
void PDKSHIM_NotifyPaused(int paused)     { if (s_paused_cb)   s_paused_cb(paused); }

// ------------------------------------------------ virtual accelerometer joystick
//
// webOS exposed the accelerometer as SDL joystick 0, and it was always present.
// 296 titles in the shipped catalogue call SDL_JoystickOpen and 213 read its axes;
// most do not check the result, so on a machine with no joystick they take NULL
// back and segfault ("accel is null!" is EA's version of the message).
//
// Present one always-available 3-axis device reading level (all axes 0), which is
// the neutral position for tilt steering. Real joysticks, if any are ever present,
// take precedence. PDK_NO_VJOY disables this.

#define VJOY_AXES 3
static int   s_vjoy_opened;
static char  s_vjoy_handle[64];          // opaque; games only pass it back to us

typedef int (*numjoy_fn)(void);

static int real_numjoysticks(void)
{
    static numjoy_fn real;
    static int probed;
    if (!probed) { real = (numjoy_fn)dlsym(RTLD_NEXT, "SDL_NumJoysticks"); probed = 1; }
    return real ? real() : 0;
}

static int vjoy_active(void)
{
    static int v = -1;
    if (v < 0) v = (getenv("PDK_NO_VJOY") || real_numjoysticks() > 0) ? 0 : 1;
    return v;
}

int SDL_NumJoysticks(void)
{
    if (vjoy_active()) return 1;
    return real_numjoysticks();
}

const char *SDL_JoystickName(int i)
{
    if (vjoy_active()) return i == 0 ? "webOS accelerometer" : NULL;
    const char *(*real)(int) = (const char *(*)(int))dlsym(RTLD_NEXT, "SDL_JoystickName");
    return real ? real(i) : NULL;
}

void *SDL_JoystickOpen(int i)
{
    if (vjoy_active()) {
        if (i != 0) return NULL;
        s_vjoy_opened = 1;
        TRACE("SDL_JoystickOpen(0) -> virtual accelerometer");
        return s_vjoy_handle;
    }
    void *(*real)(int) = (void *(*)(int))dlsym(RTLD_NEXT, "SDL_JoystickOpen");
    return real ? real(i) : NULL;
}

static int is_vjoy(void *j) { return vjoy_active() && j == (void *)s_vjoy_handle; }

#define VJOY_PASSTHRU(ret, name, params, args, dflt)                     \
    ret name params {                                                    \
        if (!is_vjoy((void *)j)) {                                       \
            ret (*real) params = (ret (*) params)dlsym(RTLD_NEXT, #name);\
            if (real) return real args;                                  \
        }                                                                \
        return dflt;                                                     \
    }

VJOY_PASSTHRU(int, SDL_JoystickNumAxes,    (void *j), (j), VJOY_AXES)
VJOY_PASSTHRU(int, SDL_JoystickNumButtons, (void *j), (j), 0)
VJOY_PASSTHRU(int, SDL_JoystickNumHats,    (void *j), (j), 0)
VJOY_PASSTHRU(int, SDL_JoystickNumBalls,   (void *j), (j), 0)
VJOY_PASSTHRU(int, SDL_JoystickIndex,      (void *j), (j), 0)

// Level device: no tilt on any axis. That is the neutral reading for tilt steering.
short SDL_JoystickGetAxis(void *j, int axis)
{
    if (!is_vjoy(j)) {
        short (*real)(void *, int) = (short (*)(void *, int))dlsym(RTLD_NEXT, "SDL_JoystickGetAxis");
        if (real) return real(j, axis);
    }
    (void)axis;
    return 0;
}

unsigned char SDL_JoystickGetButton(void *j, int b)
{
    if (!is_vjoy(j)) {
        unsigned char (*real)(void *, int) =
            (unsigned char (*)(void *, int))dlsym(RTLD_NEXT, "SDL_JoystickGetButton");
        if (real) return real(j, b);
    }
    (void)b;
    return 0;
}

unsigned char SDL_JoystickGetHat(void *j, int h)
{
    if (!is_vjoy(j)) {
        unsigned char (*real)(void *, int) =
            (unsigned char (*)(void *, int))dlsym(RTLD_NEXT, "SDL_JoystickGetHat");
        if (real) return real(j, h);
    }
    (void)h;
    return 0;
}

int SDL_JoystickOpened(int i)
{
    if (vjoy_active()) return (i == 0) ? s_vjoy_opened : 0;
    int (*real)(int) = (int (*)(int))dlsym(RTLD_NEXT, "SDL_JoystickOpened");
    return real ? real(i) : 0;
}

void SDL_JoystickClose(void *j)
{
    if (is_vjoy(j)) { s_vjoy_opened = 0; return; }
    void (*real)(void *) = (void (*)(void *))dlsym(RTLD_NEXT, "SDL_JoystickClose");
    if (real) real(j);
}

void SDL_JoystickUpdate(void)
{
    void (*real)(void) = (void (*)(void))dlsym(RTLD_NEXT, "SDL_JoystickUpdate");
    if (real) real();
}
