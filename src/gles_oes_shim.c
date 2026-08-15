// libGLES_CM shim - GLES1 extension entry points that Mesa does not export.
//
// Palm's libGLES_CM.so exported the GL_OES_framebuffer_object functions as ordinary
// dynamic symbols, so games link against them directly. Mesa/libglvnd only exposes
// extension entry points through eglGetProcAddress, so those links fail:
//
//     ./tw09: symbol lookup error: undefined symbol: glGenFramebuffersOES
//
// This builds AS libGLES_CM.so with libGLESv1_CM.so.1 as a NEEDED dependency, so
// core GLES1 still resolves through the dependency graph and we only have to supply
// the missing extension symbols. Each is resolved lazily: the OES name first, then
// the unsuffixed core name, which is the same entry point in Mesa.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef int GLint;

extern void *eglGetProcAddress(const char *name);

static int verbose(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PDK_GLES_DEBUG") ? 1 : 0;
    return v;
}

static void *resolve(const char *oes, const char *core)
{
    void *p = eglGetProcAddress(oes);
    if (!p) p = eglGetProcAddress(core);
    if (!p) p = dlsym(RTLD_DEFAULT, core);
    if (!p)
        fprintf(stderr, "[gles-shim] FATAL: cannot resolve %s / %s\n", oes, core);
    else if (verbose())
        fprintf(stderr, "[gles-shim] %s -> %p\n", oes, p);
    return p;
}

// --- upscaling fixed-mode titles to the panel ------------------------------
//
// A title that hardcodes a Palm-era mode (Monopoly asks for 320x480) gets a
// fullscreen window from the SDL shim, but that only makes the WINDOW the size
// of the panel. sdl12-compat scales the 1.2 *surface* it owns; a GL title draws
// straight into the window through its own context, so its glViewport(0,0,320,480)
// lands in one corner of an 800x1280 drawable and the game is a small rectangle.
//
// So scale the viewport here instead: the game keeps drawing in its own
// coordinate space and every viewport it sets is mapped onto the real drawable.
// Sub-viewports (mini-maps, split panes) scale by the same factor rather than
// each being stretched to the full screen, which is why this multiplies through
// the mode rather than substituting the drawable size.
//
// Only when framebuffer 0 is bound. That single test keeps the two cases apart:
// a bound FBO means the app is rendering to a texture (scaling it would render
// at the wrong size into a small target), or that sdl12-compat's own OpenGL
// scaling is active - in which case it already owns the problem and this must
// keep its hands off.

#define GL_FRAMEBUFFER_BINDING_OES 0x8CA6
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH  0x3057
#define EGL_DRAW   0x3059

extern void *eglGetCurrentDisplay(void);
extern void *eglGetCurrentSurface(int readdraw);
extern unsigned int eglQuerySurface(void *dpy, void *surf, int attr, int *value);

// Per-axis, deliberately. The game maps its world onto the viewport with glOrthof,
// and a viewport covering the whole panel stretches 2:3 artwork onto a 5:8 screen.
// Scissor rectangles have to follow exactly that mapping - a uniform scale with
// letterbox offsets would be geometrically prettier and would clip the wrong pixels.
static int scale_ready;      // 0 not yet resolved, 1 scaling, -1 disabled
static double gl_sx = 1.0, gl_sy = 1.0;

static int env_int(const char *name)
{
    const char *s = getenv(name);
    return s ? atoi(s) : 0;
}

static int fbo_bound(void)
{
    static void (*fn)(GLenum, GLint *);
    if (!fn) fn = (void (*)(GLenum, GLint *))dlsym(RTLD_NEXT, "glGetIntegerv");
    if (!fn) return 0;
    GLint fb = 0;
    fn(GL_FRAMEBUFFER_BINDING_OES, &fb);
    return fb != 0;
}

// The drawable, not the panel: what the game is actually rendering into.
static int drawable_size(int *w, int *h)
{
    void *dpy = eglGetCurrentDisplay();
    void *surf = eglGetCurrentSurface(EGL_DRAW);
    if (!dpy || !surf) return 0;
    return eglQuerySurface(dpy, surf, EGL_WIDTH, w)
        && eglQuerySurface(dpy, surf, EGL_HEIGHT, h) && *w > 0 && *h > 0;
}

// vw/vh are the first viewport the game asks for, used as its logical size when
// the SDL shim has not published a mode (a title that never calls SetVideoMode).
static void resolve_scale(int vw, int vh)
{
    int dw = 0, dh = 0;
    int mw = env_int("PDK_GL_MODE_WIDTH"), mh = env_int("PDK_GL_MODE_HEIGHT");

    scale_ready = -1;
    if (getenv("PDK_NO_GL_SCALE")) return;
    if (mw <= 0 || mh <= 0) { mw = vw; mh = vh; }
    if (mw <= 0 || mh <= 0) return;
    if (!drawable_size(&dw, &dh)) {
        if (verbose()) fprintf(stderr, "[gles-shim] no EGL drawable, not scaling\n");
        return;
    }

    if (verbose())
        fprintf(stderr, "[gles-shim] viewport %dx%d, mode %dx%d, drawable %dx%d\n",
                vw, vh, mw, mh, dw, dh);

    if (dw <= mw && dh <= mh) return;   // nothing to gain

    gl_sx = (double)dw / (double)mw;
    gl_sy = (double)dh / (double)mh;
    scale_ready = 1;

    if (verbose())
        fprintf(stderr, "[gles-shim] scaling GL rects by %.3f x %.3f\n", gl_sx, gl_sy);
}

static int scaling_active(int w, int h)
{
    if (fbo_bound()) return 0;
    if (!scale_ready) resolve_scale(w, h);
    return scale_ready == 1;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    static void (*fn)(GLint, GLint, GLsizei, GLsizei);
    if (!fn) fn = (void (*)(GLint, GLint, GLsizei, GLsizei))dlsym(RTLD_NEXT, "glViewport");
    if (!fn) return;

    if (scaling_active(w, h)) {
        GLint nx = (GLint)(x * gl_sx), ny = (GLint)(y * gl_sy);
        GLsizei nw = (GLsizei)(w * gl_sx), nh = (GLsizei)(h * gl_sy);
        if (verbose())
            fprintf(stderr, "[gles-shim] glViewport(%d,%d,%d,%d) -> (%d,%d,%d,%d)\n",
                    x, y, w, h, nx, ny, nw, nh);
        fn(nx, ny, nw, nh);
        return;
    }
    fn(x, y, w, h);
}

// Scissor rectangles are in the same window space as the viewport, so they have
// to move with it or the game clips away everything it just drew.
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    static void (*fn)(GLint, GLint, GLsizei, GLsizei);
    if (!fn) fn = (void (*)(GLint, GLint, GLsizei, GLsizei))dlsym(RTLD_NEXT, "glScissor");
    if (!fn) return;

    // Resolve from the published mode only (0,0 as the fallback size, so nothing
    // happens without it): a scissor rectangle is a sub-region of the game's
    // screen, not its size, so it cannot stand in for the mode the way a viewport
    // can. This is the wrapper that matters for titles which never set a viewport.
    if (!fbo_bound() && (scale_ready ? scale_ready == 1 : (resolve_scale(0, 0), scale_ready == 1))) {
        int mw = env_int("PDK_GL_MODE_WIDTH"), mh = env_int("PDK_GL_MODE_HEIGHT");

        // Only rectangles expressed in the game's own coordinate space get scaled.
        // A title that computes its scissor from glGetIntegerv(GL_VIEWPORT) - and
        // the viewport already covers the whole panel - hands us window-space
        // numbers, and scaling those again pushes whatever it was clipping to
        // clean off the screen. That looks like missing menus rather than a
        // clipping bug, so it is worth being conservative here: anything that does
        // not fit inside the mode is assumed to be window space already.
        int in_mode_space = (mw <= 0 || mh <= 0) ||
                            ((long)x + w <= mw && (long)y + h <= mh);

        if (verbose()) {
            static int seen;
            if (seen < 20) {
                fprintf(stderr, "[gles-shim] glScissor(%d,%d,%d,%d) %s\n", x, y, w, h,
                        in_mode_space ? "-> scaled" : "-> passed through (window space)");
                seen++;
            }
        }

        if (in_mode_space) {
            fn((GLint)(x * gl_sx), (GLint)(y * gl_sy),
               (GLsizei)(w * gl_sx), (GLsizei)(h * gl_sy));
            return;
        }
    }
    fn(x, y, w, h);
}

// Most of these titles never call glViewport at all: they set up a projection with
// glOrthof in their own coordinate space and take whatever viewport the context
// came up with. That is why the wrapper above can be correct and still never fire.
//
// So enforce the viewport where drawing actually happens. glClear is called at the
// top of every frame by all of them, which makes it the one reliable hook. If the
// context handed the game a viewport smaller than the drawable, widen it to the
// whole drawable: the glOrthof projection then maps the game's world onto the full
// panel, which is precisely the scaling that is wanted.
//
// Bounded to the first frames because glGetIntegerv forces a pipeline sync, and at
// the frame rates these titles get on a software rasteriser that is not free.
#define GL_VIEWPORT 0x0BA2
#define VIEWPORT_CHECK_FRAMES 60

void glClear(unsigned int mask)
{
    static void (*fn)(unsigned int);
    static int frames;
    if (!fn) fn = (void (*)(unsigned int))dlsym(RTLD_NEXT, "glClear");
    if (!fn) return;

    if (frames < VIEWPORT_CHECK_FRAMES && !fbo_bound()) {
        static void (*getiv)(GLenum, GLint *);
        if (!getiv) getiv = (void (*)(GLenum, GLint *))dlsym(RTLD_NEXT, "glGetIntegerv");
        int dw = 0, dh = 0;
        GLint vp[4] = { 0, 0, 0, 0 };
        if (getiv) getiv(GL_VIEWPORT, vp);
        if (drawable_size(&dw, &dh)) {
            if (!frames && verbose())
                fprintf(stderr, "[gles-shim] first frame: viewport %dx%d+%d+%d, drawable %dx%d\n",
                        vp[2], vp[3], vp[0], vp[1], dw, dh);
            if ((vp[2] < dw || vp[3] < dh) && !getenv("PDK_NO_GL_SCALE")) {
                static void (*setvp)(GLint, GLint, GLsizei, GLsizei);
                if (!setvp)
                    setvp = (void (*)(GLint, GLint, GLsizei, GLsizei))dlsym(RTLD_NEXT, "glViewport");
                if (setvp) {
                    if (!frames && verbose())
                        fprintf(stderr, "[gles-shim] widening viewport to %dx%d\n", dw, dh);
                    setvp(0, 0, dw, dh);
                }
            }
        }
        frames++;
    }

    fn(mask);
}

// Each wrapper caches its target on first use.
#define FWD(ret, oesname, corename, params, args)                      \
    ret oesname params {                                               \
        static ret (*fn) params;                                       \
        if (!fn) fn = (ret (*) params)resolve(#oesname, #corename);    \
        if (!fn) return (ret)0;                                        \
        return fn args;                                                \
    }

#define FWD_VOID(oesname, corename, params, args)                      \
    void oesname params {                                              \
        static void (*fn) params;                                      \
        if (!fn) fn = (void (*) params)resolve(#oesname, #corename);   \
        if (fn) fn args;                                               \
    }

FWD_VOID(glGenFramebuffersOES,  glGenFramebuffers,
         (GLsizei n, GLuint *fb), (n, fb))
FWD_VOID(glDeleteFramebuffersOES, glDeleteFramebuffers,
         (GLsizei n, const GLuint *fb), (n, fb))
FWD_VOID(glBindFramebufferOES, glBindFramebuffer,
         (GLenum target, GLuint fb), (target, fb))
FWD_VOID(glGenRenderbuffersOES, glGenRenderbuffers,
         (GLsizei n, GLuint *rb), (n, rb))
FWD_VOID(glDeleteRenderbuffersOES, glDeleteRenderbuffers,
         (GLsizei n, const GLuint *rb), (n, rb))
FWD_VOID(glBindRenderbufferOES, glBindRenderbuffer,
         (GLenum target, GLuint rb), (target, rb))
FWD_VOID(glRenderbufferStorageOES, glRenderbufferStorage,
         (GLenum target, GLenum fmt, GLsizei w, GLsizei h), (target, fmt, w, h))
FWD_VOID(glFramebufferRenderbufferOES, glFramebufferRenderbuffer,
         (GLenum target, GLenum att, GLenum rbtarget, GLuint rb),
         (target, att, rbtarget, rb))
FWD_VOID(glFramebufferTexture2DOES, glFramebufferTexture2D,
         (GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level),
         (target, att, textarget, tex, level))
FWD(GLenum, glCheckFramebufferStatusOES, glCheckFramebufferStatus,
    (GLenum target), (target))
FWD_VOID(glGetFramebufferAttachmentParameterivOES, glGetFramebufferAttachmentParameteriv,
         (GLenum target, GLenum att, GLenum pname, GLint *params),
         (target, att, pname, params))
FWD_VOID(glGetRenderbufferParameterivOES, glGetRenderbufferParameteriv,
         (GLenum target, GLenum pname, GLint *params), (target, pname, params))
FWD_VOID(glGenerateMipmapOES, glGenerateMipmap, (GLenum target), (target))

// --- GL_OES_matrix_palette (13 titles link these) --------------------------
// Skeletal animation via a matrix palette. Mesa may not implement the extension
// at all, in which case resolve() logs and the call becomes a no-op: the model
// renders wrong rather than the app failing to load, which is the better failure.
FWD_VOID(glCurrentPaletteMatrixOES, glCurrentPaletteMatrixARB,
         (GLuint index), (index))
FWD_VOID(glMatrixIndexPointerOES, glMatrixIndexPointerARB,
         (GLint size, GLenum type, GLsizei stride, const void *p), (size, type, stride, p))
FWD_VOID(glWeightPointerOES, glWeightPointerARB,
         (GLint size, GLenum type, GLsizei stride, const void *p), (size, type, stride, p))
FWD_VOID(glLoadPaletteFromModelViewMatrixOES, glLoadPaletteFromModelViewMatrixOES,
         (void), ())

// --- remaining framebuffer-object queries ----------------------------------
FWD(unsigned char, glIsFramebufferOES,  glIsFramebuffer,  (GLuint fb), (fb))
FWD(unsigned char, glIsRenderbufferOES, glIsRenderbuffer, (GLuint rb), (rb))

// --- GL_OES_draw_texture ---------------------------------------------------
FWD_VOID(glDrawTexiOES,  glDrawTexiOES,  (GLint x, GLint y, GLint z, GLint w, GLint h), (x,y,z,w,h))
FWD_VOID(glDrawTexfOES,  glDrawTexfOES,  (float x, float y, float z, float w, float h), (x,y,z,w,h))
FWD_VOID(glDrawTexsOES,  glDrawTexsOES,  (short x, short y, short z, short w, short h), (x,y,z,w,h))
FWD_VOID(glDrawTexxOES,  glDrawTexxOES,  (GLint x, GLint y, GLint z, GLint w, GLint h), (x,y,z,w,h))
FWD_VOID(glDrawTexivOES, glDrawTexivOES, (const GLint *c), (c))
FWD_VOID(glDrawTexfvOES, glDrawTexfvOES, (const float *c), (c))
FWD_VOID(glDrawTexsvOES, glDrawTexsvOES, (const short *c), (c))
FWD_VOID(glDrawTexxvOES, glDrawTexxvOES, (const GLint *c), (c))
