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
