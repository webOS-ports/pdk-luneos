// Minimal EGL + GLES2 probe: does Mesa give us a context and compile a shader
// in this armel sysroot? Prototypes declared inline so no EGL/GL headers needed.
#include <stdio.h>
#include <stdlib.h>

typedef void *EGLDisplay, *EGLContext, *EGLConfig, *EGLSurface;
typedef int EGLBoolean, EGLint;
#define EGL_DEFAULT_DISPLAY   ((void *)0)
#define EGL_NO_CONTEXT        ((void *)0)
#define EGL_NO_SURFACE        ((void *)0)
#define EGL_NONE              0x3038
#define EGL_OPENGL_ES_API     0x30A0
#define EGL_RENDERABLE_TYPE   0x3040
#define EGL_OPENGL_ES2_BIT    0x0004
#define EGL_SURFACE_TYPE      0x3033
#define EGL_PBUFFER_BIT       0x0001
#define EGL_WIDTH             0x3057
#define EGL_HEIGHT            0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

extern EGLDisplay eglGetDisplay(void *);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
extern EGLBoolean eglBindAPI(EGLint);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
extern EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern const char *eglQueryString(EGLDisplay, EGLint);
extern EGLint     eglGetError(void);

#define GL_VERSION        0x1F02
#define GL_RENDERER       0x1F01
#define GL_VERTEX_SHADER  0x8B31
#define GL_COMPILE_STATUS 0x8B81
extern const unsigned char *glGetString(unsigned int);
extern unsigned int glCreateShader(unsigned int);
extern void glShaderSource(unsigned int, int, const char **, const int *);
extern void glCompileShader(unsigned int);
extern void glGetShaderiv(unsigned int, unsigned int, int *);
extern void glGetShaderInfoLog(unsigned int, int, int *, char *);

int main(void)
{
    // glvnd cannot pick a vendor for eglGetDisplay(EGL_DEFAULT_DISPLAY) when there is
    // no X/Wayland session, so ask for the surfaceless platform explicitly.
    #define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
    typedef EGLDisplay (*getplatdisp_fn)(EGLint, void *, const EGLint *);
    extern void *eglGetProcAddress(const char *);

    EGLDisplay dpy = NULL;
    getplatdisp_fn gpd = (getplatdisp_fn)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (gpd) {
        dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
        printf("eglGetPlatformDisplayEXT(surfaceless) -> %p\n", dpy);
    }
    if (!dpy) {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        printf("eglGetDisplay(default) -> %p\n", dpy);
    }
    if (!dpy) { printf("FAIL: no display (err 0x%x)\n", eglGetError()); return 1; }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("FAIL: eglInitialize (err 0x%x)\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d\n", major, minor);
    printf("EGL_VENDOR: %s\n", eglQueryString(dpy, 0x3053));
    printf("EGL_CLIENT_APIS: %s\n", eglQueryString(dpy, 0x308D));

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
        printf("FAIL: eglChooseConfig (err 0x%x)\n", eglGetError());
        return 1;
    }

    EGLint pbattr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbattr);
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if (!ctx) { printf("FAIL: eglCreateContext (err 0x%x)\n", eglGetError()); return 1; }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("FAIL: eglMakeCurrent (err 0x%x)\n", eglGetError());
        return 1;
    }

    printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    const char *src = "attribute vec4 p; void main(){ gl_Position = p; }";
    unsigned int sh = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    int ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    printf("shader compile: %s\n", ok ? "OK" : "FAILED");
    if (!ok) { char log[1024] = {0}; int l = 0;
               glGetShaderInfoLog(sh, sizeof(log) - 1, &l, log);
               printf("log: %s\n", log); }
    return ok ? 0 : 1;
}
