# Building

Two independent things get built: the **shims** (four small ARM softfp shared
libraries plus debugging aids) and the **sysroot** (the modern armel userland they
sit on).

For building the whole thing as part of a LuneOS image instead, see
[yocto.md](yocto.md). This page covers the standalone development build.

## Prerequisites

| | |
|---|---|
| `clang` | with the ARM target enabled — every distro clang has it |
| `lld` | `ld.lld` specifically |
| `patchelf` | for soname rewriting in the sysroot |
| `qemu-arm-static` | to run anything on an x86 box |
| ~1.5 GB disk | sysroot plus package cache |

**No ARM cross-gcc is needed.** clang targets `arm-linux-gnueabi` directly against
the Debian armel sysroot, and lld links it. This removes the single most annoying
prerequisite the project would otherwise have.

If the box has no system `lld`/`patchelf` and no passwordless sudo, drop static
builds into `tools/host/` — the Makefile prefers those over the system copies:

```
tools/host/qemu-arm-static
tools/host/ld.lld          # must be the ld.lld name; lld dispatches on argv[0]
tools/host/patchelf
tools/host/gdb-multiarch
```

Debian 12's `qemu-arm-static` (62 MB) is the last genuinely static build; later
ones are dynamically linked and will not work from a bare directory.

## Build the sysroot

```sh
make sysroot
```

Runs `tools/mk-sysroot.sh`, which needs no root and no `chroot`. It downloads
Debian bookworm **armel** packages into `pkgcache/`, unpacks them into `sysroot/`,
and then does the webOS-specific work described in
[architecture.md](architecture.md#the-sysroot): soname redirection, unversioned
`.so` symlinks, font installation, and the SDL2 pin.

Override the location with `SYSROOT=`:

```sh
make sysroot SYSROOT=/srv/pdk/sysroot
```

### The legacy overlay (optional, not redistributable)

Some titles need Palm libraries that have no modern equivalent — legacy ffmpeg
0.6 for music (`libavformat.so.52`, `libavcodec.so.52`, `libavutil.so.50`,
`libopencore-amrnb.so.0`, `libfaac.so.0`), `libpalmvibe.so`, Palm's `libSDL_mixer`.
Point the script at a device image you own:

```sh
make sysroot LEGACY_ROOTFS=/path/to/nova-cust-image-topaz.rootfs-att
```

`libpalmvibe.so` is not in the TouchPad image; it comes from a Veer 2.1.1 image
(`VEER_ROOTFS=`).

When copying legacy libraries by hand, copy **both the symlink and its target** —
copy only the symlink and the loader still reports the library missing.

The build works without any overlay; fewer titles run.

### Missing sonames

If a title fails to load with an unresolved library, find which Debian package
carries it:

```sh
tools/resolve-deps.py sysroot/usr/lib/libFoo.so.1
```

Run it against **dlopened** libraries too, not just linked ones — that was the
whole EGL story, see [graphics.md](graphics.md).

## Build the shims

```sh
make
```

produces, in `build/`:

| | |
|---|---|
| `libpdl.so` | the PDK API (ARM softfp) |
| `libSDL-1.2.so.0` | the webOS SDL shim (ARM softfp) |
| `libGLES_CM.so` | GLES1 extension entry points (ARM softfp) |
| `libSDL_cinema.so` | video-library stub (ARM softfp) |
| `crashcatch.so` | `LD_PRELOAD` fault reporter (ARM softfp) |
| `egltest` | standalone EGL/GLES2 probe (ARM softfp) |
| `pdkhost` | PIpc host — built **natively**, not ARM |

`pdkhost` is host-native on purpose: it stands in for LunaSysMgr, so it belongs on
the other side of the emulation boundary.

```sh
make install
```

stages the four libraries into `$(SYSROOT)/usr/lib`.

## Check the ABI

The single most useful sanity check. Every library the app loads must say
`soft-float ABI`:

```sh
readelf -h build/libpdl.so | grep Flags
# Flags: 0x5000200, Version5 EABI, soft-float ABI      ← correct
# Flags: 0x5000002, Version5 EABI, <unknown>            ← also correct, see below
# Flags: 0x5000400, Version5 EABI, hard-float ABI      ← wrong, will corrupt floats
```

## Smoke tests

```sh
make egl     # EGL/GLES2 probe — should report Mesa + llvmpipe, no SDL involved
make run     # the bundled PDK sample under qemu-arm
make run VIDEO=wayland   # ... against your own compositor
```

A healthy `make egl`:

```
eglGetPlatformDisplayEXT(surfaceless) -> 0x400388d0
EGL 1.5   EGL_VENDOR: Mesa Project
GL_VERSION:  OpenGL ES 3.2 Mesa 22.3.6
GL_RENDERER: llvmpipe (LLVM 15.0.6, 128 bits)
shader compile: OK
```

## Deploying to a target

```sh
rsync -a sysroot/ root@target:/opt/pdk/sysroot/
scp tools/pdk-run root@target:/opt/pdk/
scp path/to/qemu-arm-static root@target:/opt/pdk/qemu-arm     # x86 targets only
```

`qemu-arm` is only needed where the CPU cannot execute ARM32 itself. On ARMv7
hardware, drop it and edit `pdk-run` to exec the binary directly.

Then install applications with `tools/install-games.sh` — see
[running.md](running.md#installing-applications).

## Make variables

| variable | default | |
|---|---|---|
| `SYSROOT` | `./sysroot` | where the sysroot lives |
| `OUT` | `build` | build output directory |
| `LEGACY_ROOTFS` | — | extracted webOS 3.0.5 device image, for the overlay |
| `VEER_ROOTFS` | — | Veer 2.1.1 image, for `libpalmvibe.so` |
| `LLD` / `PATCHELF` / `QEMU_ARM` | vendored, else `PATH` | tool overrides |
| `VIDEO` | `dummy` | SDL video driver for `make run` |
