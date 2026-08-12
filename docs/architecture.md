# Architecture

## The problem

A legacy webOS PDK application is an ARMv7 ELF with `Flags 0x5000002` — EABI5,
soft-float. LuneOS builds everything hard-float, `0x5000400`. The two
cannot be mixed in one process: they disagree about whether floating-point
arguments travel in VFP registers or in the core registers, so every call across
the boundary with a `float` or `double` in it silently corrupts its arguments.

That is only the first layer. The apps also depend on:

* about twenty Palm libraries — `libpdl`, `libnapp`, `libSDL-1.2` (a webOS fork),
  `libSDL_cinema`, `libPiranha`, `libGLES_CM`, `libEGL` …
* rendering directly to `/dev/fb0`
* a PIpc socket to `LunaSysMgr` for window lifecycle and input
* being chrooted with the app's own directory as `/`

None of which LuneOS provides.

## Three findings that shaped the design

### 1. They render to the framebuffer, not through a compositor

The shipped `libSDL-1.2.so.0` contains exactly one real video backend —
`FBCON_bootstrap` — plus `dummy`. There is no compositor video driver anywhere in
it. PIpc carries window lifecycle and input events; it never carries pixels.

So there is nothing to "port" on the display path. The app has to be given a
different SDL, and the natural one is SDL2 (which speaks Wayland) behind
**sdl12-compat**, which provides the SDL 1.2 ABI on top of it.

### 2. The legacy GL stack can never work again

`libEGL.so` in the device image is the proprietary **Qualcomm Adreno** driver. It
needs `/dev/kgsl-3d0`, `/dev/kgsl-2d0`, `/dev/pmem_smipool` and the `kgsl_*`
ioctls from the 2011 msm kernel. Mainline tenderloin (6.18) uses msm DRM +
freedreno; the x86 VM has no kgsl at all. There is no configuration in which that
blob loads.

EGL, GLESv2 and GLES_CM therefore have to be Mesa. That is fine — Mesa builds for
armel, and its GLES1 and GLES2 are complete. The one gap is extension entry
points, which is what `libGLES_CM.so` in this project exists to close.

### 3. Debian armel is our exact ABI

`readelf -h` reports `soft-float ABI` on both PDK output and Debian armel
packages. Debian armel is EABI soft-float — the same ABI, still maintained, with a
full modern archive behind it.

This is the finding that made the project tractable. It means no cross-compiler
had to be built, no libraries had to be ported: a modern userland (glibc 2.36,
Mesa, SDL2, sdl12-compat, libcurl, SDL_image/ttf/mixer) drops in unmodified.

**glibc is backward compatible, never forward compatible.** So the rule is
*modern base plus legacy overlay*, never the reverse. A 2011 library on a 2023
glibc works; a 2023 library on a 2011 glibc does not.

## The resulting model

```
 ┌──────────────────────────────────────────────────────────┐
 │  the game (ARM softfp, unmodified retail binary)         │
 ├──────────────────────────────────────────────────────────┤
 │  libpdl.so        ← rewritten (src/libpdl.c)             │
 │  libSDL-1.2.so.0  ← shim   (src/sdl_webos_shim.c)        │
 │  libGLES_CM.so    ← shim   (src/gles_oes_shim.c)         │
 │  libSDL_cinema.so ← stub   (src/sdl_cinema_stub.c)       │
 ├──────────────────────────────────────────────────────────┤
 │  sdl12-compat → SDL2 → Wayland client libs               │
 │  Mesa (EGL / GLESv1_CM / GLESv2, llvmpipe or freedreno)  │
 │  glibc, libcurl, libSDL_image/ttf/mixer, …               │
 │            ── all of this is ARM softfp ──               │
 └──────────────────────────────────────────────────────────┘
                            │
                   Wayland socket  ← the only ABI boundary
                            │
 ┌──────────────────────────────────────────────────────────┐
 │  luna-surfacemanager (native, hard-float)                │
 └──────────────────────────────────────────────────────────┘
```

The key property: **the ABI boundary is the Wayland socket, not a function call.**
Nothing softfp ever calls anything hardfp. A socket protocol has no calling
convention, so the mismatch simply does not arise. Everything inside the box is
soft-float, right down to glibc.

On x86 targets the whole box additionally runs inside `qemu-arm` user-mode
emulation. On real ARMv7 hardware it does not need to: an ARMv7 CPU executes
soft-float code natively — softfp and hardfp are the same instruction set, they
differ only in the calling convention — so the emulator drops out and only the
separate softfp library set remains.

## The four shims

### `libpdl.so` — `src/libpdl.c`

A complete reimplementation of Palm's PDK API: 68 `PDL_*` exports. Written against
the original's exported symbol list and behaviour, informed by decompilation of
the shipped binary (see [reverse-engineering.md](reverse-engineering.md)).

Notable implementation choices:

* **Device preferences** are read directly out of
  `/var/luna/preferences/systemprefs.db` with a bounded scan, rather than by
  linking sqlite — the file format is stable and this keeps the dependency
  surface small. Language, region and similar come from there, so
  `PDL_GetLanguage` returns what LuneOS is actually set to.
* **App identity** comes from parsing `appinfo.json`, located by walking up from
  `/proc/self/exe`. No registration step, no service call.
* **Service calls** are routed through `luna-send`. Legacy luna-service2
  (2.0.0-136) is wire-incompatible with LuneOS's, so speaking the protocol
  directly is not an option; shelling out to the current client is.
* **The in-app purchase API exists and politely declines.** `PDL_PurchaseItem`,
  `PDL_GetAvailableItems`, `PDL_GetItemReceiptJSON`, `PDL_GetItemCollectionJSON`
  and `PDL_GetParamJson` cannot work — the payment service has been gone for a
  decade — but a *missing* symbol stops the app loading at all, so they are
  present and report "not available". `PDL_isAppLicensedForDevice` answers yes,
  on the grounds that the app is installed.

It is deliberately linked with `--no-as-needed` against `libcurl`, `libssl`,
`libcrypto` and `libsqlite3`. The original pulled those in, and other Palm
libraries reached their symbols *through* it. Dropping them broke seven-plus
titles, including the entire Hexage catalogue, with
`undefined symbol: curl_easy_init`.

### `libSDL-1.2.so.0` — `src/sdl_webos_shim.c`

Builds *as* `libSDL-1.2.so.0` with `libSDL12compat.so.0` as a NEEDED dependency,
so ordinary SDL 1.2 calls fall through and only the webOS-specific parts are
implemented here:

* all 13 `SDL_WebOsHook*` entry points
* `SDL_GLES_LoadLibrary` / `GetProcAddress` / `SetAttribute`
* `SDL_GetMultiMouseState` / `SDL_GetRelativeMultiMouseState`
* six `SDL_Haptic*` stubs
* **the virtual accelerometer** (below)
* `SDL_SetVideoMode` interception (below)

HP never released their SDL changes. `HP Open Source/libsdl-1.2.tgz` is stock
upstream SDL 1.2.13 — no `SDL_WebOsHook*`, no `PDL_`, and zero GLES references in
`src/video/fbcon/` — while the shipped device binary is `libSDL-1.2.so.0.11.2`
with a GLES-capable fbcon driver. So this file is necessarily a clean-room
reimplementation driven by the exported symbol list and by how `libpdl` calls into
it. (`libsdl-image`/`mixer`/`net`/`ttf` in the same drop are likewise stock.)

#### The virtual accelerometer

webOS exposed the accelerometer as **SDL joystick 0**, and it was always present.
**296 of 583 titles call `SDL_JoystickOpen`** and 213 read its axes. Almost none
check the result — with no joystick attached they take NULL back and segfault. EA's
Sims announces it first: `accel is null!`

The shim presents one always-available three-axis device reading level (0 on every
axis — the neutral position for tilt steering). Real joysticks take precedence;
`PDK_NO_VJOY=1` disables it. Thirteen `SDL_Joystick*` functions are overridden.

By number of titles touched this is the single widest-reaching fix in the project.

#### `SDL_SetVideoMode` interception

```c
SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, uint32_t flags)
```

does four things:

1. Maps Palm's `SDL_OPENGLES` (0x40) and `SDL_OPENGLESBLIT` (0x48) onto
   `SDL_OPENGL`.
2. Calls `request_gles_profile()`, which reaches *past* sdl12-compat to SDL2's own
   `SDL_GL_SetAttribute` and sets `SDL_GL_CONTEXT_PROFILE_MASK` to ES. See
   [graphics.md](graphics.md#sdl12-compat-only-ever-asks-for-desktop-gl).
3. Resolves `0x0` — "native resolution" — to the actual display size, and clamps
   oversized requests (`PDK_NO_CLAMP=1` to disable).
4. Optionally reuses the existing surface for an identical mode-set
   (`PDK_MODE_CACHE=1`; off by default).

Alongside it, `drop_resize()` swallows `SDL_VIDEORESIZE` events by default. webOS
apps were always fullscreen at a fixed resolution and never had to handle a
resize; under a real compositor sdl12-compat emits one, and engines answer with
another `SetVideoMode` — forever. Dropping the events breaks that loop.
`PDK_ALLOW_RESIZE=1` restores them, which the Haxe/NME titles need.

### `libGLES_CM.so` — `src/gles_oes_shim.c`

Palm exported the `GL_OES_framebuffer_object` entry points as ordinary dynamic
symbols. Mesa and libglvnd only offer extension entry points through
`eglGetProcAddress`, so games that link them directly fail with
`undefined symbol: glGenFramebuffersOES`.

This builds as `libGLES_CM.so` with `libGLESv1_CM.so.1` and `libEGL.so.1` as
NEEDED, so core GLES1 resolves through the normal dependency graph and only the
extension symbols are supplied here. Each is resolved lazily: the OES name first,
then the unsuffixed core name, which in Mesa is the same entry point. Covered:
`GL_OES_framebuffer_object`, `GL_OES_matrix_palette` (13 titles) and
`GL_OES_draw_texture`.

Where Mesa does not implement an extension at all, the wrapper becomes a no-op
after logging. The model renders wrong rather than the app failing to load, which
is the better failure.

### `libSDL_cinema.so` — `src/sdl_cinema_stub.c`

Palm's video-playback library talks to the webOS media service through
`libmedia-api`, so the real one cannot initialise here.

That is not cosmetic. NFS Undercover's `midp::RuntimePalm::initRuntimePalm()`
bails out **silently** when `CIN_Init()` fails, skipping the `DisplayPalm`
construction further down; `midp::Display::getDisplay()` then returns NULL and
`MonkeyApp::startApp()` makes a virtual call through it. A failed *video* init
surfaces as a null-pointer crash somewhere else entirely.

Mind the polarity: **`CIN_Init()` returns non-zero for success.** A stub that
returns 0 reproduces the exact failure it was written to prevent. (The decompiled
`movne r2,#0 / moveq r2,#1` is a logical NOT — easy to misread, and I did.)

## The sysroot

`tools/mk-sysroot.sh` assembles a hybrid tree, unprivileged, with no `chroot` and
no root:

* **base** — Debian bookworm armel packages: glibc, Mesa, SDL2, sdl12-compat,
  SDL_image/ttf/mixer, libcurl, and their dependencies
* **overlay** — the Palm libraries that have no modern equivalent, taken from a
  device image the user supplies (`LEGACY_ROOTFS=`). Not redistributable; the
  build works without it, with fewer titles running.
* **our shims**, installed over the top so they win
* **soname redirection** — the Palm GL libraries' sonames point at Mesa's
* **unversioned `.so` symlinks** — Palm's device carried them and plenty of
  packages link those names directly: `libSDL_image.so`, `libSDL_ttf.so`,
  `libz.so`, `libstdc++.so`, even `libPDL.so` with a capital PDL. Without them
  the loader gives up before the app runs.
* **webOS system fonts** — 12 apps crashed at the same address in `libSDL_ttf`
  because they hardcode `/usr/share/fonts/PreludeCondensed-Medium.ttf` and never
  check `TTF_OpenFont` for NULL.
* `patchelf --set-soname` renames sdl12-compat so it owns `libSDL-1.2.so.0`'s
  place in the graph while our shim takes the name.

## The Wayland shell (was a pin, now fixed)

luna-surfacemanager advertised `wl_shell` and `wl_webos_shell` and nothing else —
`weboscorecompositor.cpp` only ever constructed `QWaylandWlShell`. SDL2 removed
its `wl_shell` backend in 2.0.16, so anything newer connected, bound
`wl_compositor`, and then had no way to give a surface a role. It never attached
a buffer, `hasContent()` stayed false, no `WebOSSurfaceItem` was mapped, and the
window silently never appeared. That is why the sysroot used to pin SDL2 2.0.14.

The compositor now also constructs a `QWaylandXdgShell` and answers the initial
configure with `sendFullscreen(outputSize)` — xdg_shell requires a configure
before the client may attach its first buffer. Surface handling on the webOS side
was already role-agnostic (items come from `QWaylandCompositor::surfaceCreated`
regardless of shell), so that plus mapping `xdg_toplevel.app_id` onto the item
was the whole change.

Verified on the emulator with `WAYLAND_DEBUG=1`:

```
wl_registry@2.global(17, "xdg_wm_base", 1)
 -> xdg_wm_base@9.get_xdg_surface(new id xdg_surface@20, wl_surface@15)
 -> xdg_surface@20.get_toplevel(new id xdg_toplevel@21)
 -> xdg_toplevel@21.set_app_id("giddy3")
    xdg_toplevel@21.configure(1920, 1080, array[4])
 -> wl_surface@15.attach(wl_buffer@17, 0, 0)
 -> wl_surface@15.commit()
```

The patch lives in meta-luneos, not here:
`recipes-webos-ose/luna-surfacemanager/luna-surfacemanager/0016-Advertise-xdg_wm_base-so-modern-toolkits-can-map-wind.patch`.

## The jail

PDK apps ran chrooted with their own directory as `/` — that is what
`jailerType = "pdk"` does in upstream luna-sysmgr. Retail games therefore use
absolute paths into their own package: FIFA opens `/vfs/data.vfs` while shipping
`vfs/data.vfs` inside the IPK.

`pdk-run` currently emulates this with a symlink, since only one game runs at a
time:

```sh
ln -sfn /path/to/app/vfs /vfs
```

The real fix is to launch each app in a mount namespace (`unshare -m` +
`mount --bind`) so each gets its own view. Under `qemu-arm` on a dev box no root
is needed at all — qemu-user tries `<sysroot>/<path>` before `<path>`, so the jail
can live inside the sysroot.

## What is not here

* **luna-sysmgr's PDK hosting path.** Upstream `openwebos/luna-sysmgr` still has
  all of it (`Src/remote/`, `Type_PDK`, the pdk jailer) — 459 files / 5.4 MB
  against 24 files / 564 KB in the LuneOS fork, which is now only `Src/base` and
  `Src/core`. It could be restored; the `luna-sysmgr-ipc` library is still a live
  LuneOS recipe, already patched for gcc-11/glibc-2.34. This project took the
  other route and made apps not need it — see [pipc.md](pipc.md).
* **Any modification to appinstalld2.** `tools/install-games.sh` bypasses the
  install service entirely, so nothing here validates it.
