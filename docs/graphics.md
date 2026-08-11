# Graphics

## Why the legacy stack is unrecoverable

`libEGL.so` in the webOS 3.0.5 image is the proprietary **Qualcomm Adreno**
driver. It opens `/dev/kgsl-3d0`, `/dev/kgsl-2d0` and `/dev/pmem_smipool` and
issues `kgsl_*` ioctls from the 2011 msm kernel.

* mainline tenderloin (6.18) uses msm DRM + freedreno — no kgsl nodes
* the x86 VM has no kgsl at all

There is no configuration in which that blob loads. EGL, GLESv2 and GLES_CM are
therefore Mesa, and that is settled rather than provisional.

There is also no software-only subset to ship first: **8 of 8 native games in the
retail catalogue need GL.** Seven link `libGLES_CM` (GLES1), one links
`libGLESv2`. The three Haxe/NME titles look dependency-free at the top level but
`dlopen` a bundled `nme.so` that pulls in both.

## Two traps that both looked like "EGL is broken"

Getting `make egl` to this output took clearing two unrelated problems that
presented identically:

```
eglGetPlatformDisplayEXT(surfaceless) -> 0x400388d0
EGL 1.5   EGL_VENDOR: Mesa Project
GL_VERSION:  OpenGL ES 3.2 Mesa 22.3.6
GL_RENDERER: llvmpipe (LLVM 15.0.6, 128 bits)
shader compile: OK
```

### 1. Silently missing `dlopen` dependencies

`libEGL_mesa.so.0` needs `libxcb-randr.so.0`. `swrast_dri.so` needs
`libatomic.so.1` and `libz3.so.4`.

Mesa `dlopen`s both at runtime, so a missing dependency produces **no link error
and no message**. libglvnd simply ends up with zero EGL vendors, and every
`eglGetDisplay` / `eglGetPlatformDisplayEXT` returns `EGL_BAD_PARAMETER` before any
driver load is even attempted.

This is very easy to misread as a libglvnd bug, or as something qemu is doing. It
is neither. The rule that falls out:

```sh
tools/resolve-deps.py sysroot/usr/lib/libEGL_mesa.so.0 sysroot/usr/lib/dri/swrast_dri.so
```

**Always run the dependency resolver against the *dlopened* libraries, not just
the linked ones.** `ldd` will not find these for you.

### 2. sdl12-compat only ever asks for desktop GL

sdl12-compat has no GLES option at all — there is nothing in `SDL12COMPAT_*` for
it. It always requests a desktop OpenGL context.

PDK shaders are GLSL ES and use precision qualifiers:

```glsl
varying highp vec3 NormVec;
```

Desktop GLSL rejects those, so **every shader fails to compile even though the
context is perfectly valid** — which looks like a driver problem and is not.

`request_gles_profile()` in `src/sdl_webos_shim.c` fixes it by reaching past
sdl12-compat to SDL2's own `SDL_GL_SetAttribute` and setting
`SDL_GL_CONTEXT_PROFILE_MASK = SDL_GL_CONTEXT_PROFILE_ES`. It picks:

* **ES 1.1** for GLES1 titles, detected by probing for `glOrthof` — seven of the
  eight retail games link `libGLES_CM`
* **ES 2.0** otherwise

Override with `PDK_GLES_VERSION=1|2`; disable with `PDK_NO_GLES=1`.

### Harmless noise

```
libEGL warning: egl: failed to create dri2 screen
radeon: Failed to get PCI ID
```

Both are Mesa probing DRI2/X11 and hardware drivers before falling back to
llvmpipe. `EGL_PLATFORM=surfaceless` skips the probing.

## Missing GLES1 extension entry points

Palm exported the `GL_OES_framebuffer_object` functions as ordinary dynamic
symbols. Mesa and libglvnd only expose extension entry points through
`eglGetProcAddress`, so a game that links them directly dies at load:

```
./tw09: symbol lookup error: undefined symbol: glGenFramebuffersOES
```

`src/gles_oes_shim.c` builds as `libGLES_CM.so` and supplies them, chaining to
`libGLESv1_CM.so.1` for core GLES1. Each symbol resolves lazily — OES name first,
then the unsuffixed core name, which is the same entry point in Mesa. It covers
`GL_OES_framebuffer_object`, `GL_OES_matrix_palette` and `GL_OES_draw_texture`.

## Performance on the x86 VM

Measured with Quake HD, counting composited frames via `WAYLAND_DEBUG=1`:

| vCPUs | resolution | `LP_NUM_THREADS` | frames / 30 s | fps | vs base |
|---|---|---|---|---|---|
| 2  | 1280×800 | default (2)  | 9  | 0.30 | 1.0× |
| 32 | 1280×800 | default (32) | 7  | 0.23 | **0.8×** |
| 32 | 1280×800 | 4            | 15 | 0.50 | 1.7× |
| 32 | 1280×800 | 8            | 20 | 0.67 | 2.2× |
| 32 | 1280×800 | 16           | 16 | 0.53 | 1.8× |
| 32 | 640×480  | 8            | 39 | 1.30 | **4.3×** |

Two counter-intuitive results.

**More cores alone made it slower.** Going from 2 to 32 vCPUs dropped throughput to
0.8×. llvmpipe spawns one rasteriser thread per core, and 32 guest threads
synchronising under `qemu-user` costs more than the parallelism returns — load
average hit 31.85 while frames fell.

> **Always pin `LP_NUM_THREADS`** (8 is the sweet spot here). Never let llvmpipe
> size itself from the core count under emulation.

**Resolution scaling depends on the thread count.** At 2 vCPUs, 3.3× fewer pixels
bought only 1.4×. With threads tuned it buys about 2×, because rasterisation is
now a larger share of what is left.

### Things that did not help, all measured rather than assumed

| change | result |
|---|---|
| qemu 7.2.22 → 8.2.2 (Ubuntu 24.04 static) | 44 vs 45 frames / 30 s — no gain |
| `GALLIUM_DRIVER=softpipe` (avoids the LLVM JIT) | 6 frames / 30 s — 7× **worse** |
| VirtualBox 3D acceleration | cannot help: the guest DRM is `vmwgfx` (VMSVGA), and armel Mesa ships no `svga`/`vmwgfx` gallium driver |

The qemu upgrade was the obvious candidate — emulation dominates and TCG has had
years of work — but it produced nothing measurable. Whatever the bottleneck is, it
is not TCG translation throughput.

softpipe was worth testing because llvmpipe JITs shaders to ARM which qemu then
re-translates to x86 — a double JIT. It loses badly anyway: llvmpipe's compiled
code beats an interpreter even after paying for translation twice.

Run-to-run variance on a live VM is around **12 %** (the same 7.2 config measured
39 and 44 on separate runs), so treat anything smaller as noise.

### Best known VM configuration

```sh
SDL_VIDEODRIVER=wayland XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 \
PDK_SCREEN_WIDTH=640 PDK_SCREEN_HEIGHT=480 LP_NUM_THREADS=8 \
LIBGL_ALWAYS_SOFTWARE=1
```

## On real hardware

None of the above applies. There is no emulation — an ARMv7 CPU runs soft-float
code natively — and there is a real GPU path: armel Mesa does ship `msm_dri.so`
and `kgsl_dri.so`, so tenderloin should get **freedreno** on its Adreno 220 (a2xx
covers it) against msm DRM, rather than llvmpipe.

This is the largest untested claim in the project. It needs `libgl1-mesa-dri` to
carry the freedreno gallium driver for armel, and it has not been tried.
