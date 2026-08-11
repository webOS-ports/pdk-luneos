# Building into a LuneOS image

The `yocto/` directory in this repository is a complete BitBake layer
(`meta-pdk`). Adding it to a LuneOS build produces `/opt/pdk` in the image —
sysroot, shims and launcher — instead of hand-copying files to a device.

> **Status: verified as far as configuration, not yet compiled.**
>
> What has been checked, against scarthgap in a scratch build directory:
>
> * the layer parses clean — `bitbake -p`, 0 errors, with the multiconfig enabled
> * the tune resolves correctly: `TARGET_FPU="softfp"`,
>   `TARGET_SYS="arm-oe-linux-gnueabi"`, `TUNE_CCARGS` containing
>   `-mfloat-abi=softfp` — this is the load-bearing claim and BitBake confirms it
> * `pdk-sysroot-image`'s dependency graph resolves fully in the `pdk-armel`
>   multiconfig
> * the cross-multiconfig dependency registers:
>   `mcdepends: {do_install: mc::pdk-armel:pdk-sysroot-image:do_image_complete}`
>
> What has **not** been run: an actual compile. No `bitbake luneos-image` and no
> `bitbake mc:pdk-armel:pdk-sysroot-image` has been taken through to a built
> artefact, so expect the usual first-build friction from mode 1. The prebuilt
> path (mode 2) is the configuration the 81 % compatibility figure was measured
> against and does not depend on any of this compiling.

## The problem this has to solve

A PDK application and everything it loads must be **ARM soft-float**. LuneOS is
hard-float. So the build has to produce two userlands with incompatible ABIs and
put one inside the other.

BitBake has exactly the right mechanism for this: **multiconfig**. A second
configuration, with its own `TMPDIR` and its own tune, builds the soft-float tree
from the same recipes; the main build consumes the result through `mcdepends`.

```
mc:pdk-armel   ──build──▶  pdk-sysroot-image (tar.bz2, ARM softfp)
                                   │
                                   │ mcdepends
                                   ▼
main build     ──────────▶  pdk-sysroot ──▶ /opt/pdk/sysroot in the image
                            pdk-tools   ──▶ /opt/pdk/pdk-run
                            qemu-user-arm  (x86 / arm64 targets only)
```

The tune is the whole trick:

| tune | `TUNE_FEATURES` | ELF flags | |
|---|---|---|---|
| `armv7athf-neon` | includes `callconvention-hard` | `0x5000200` | LuneOS |
| `armv7at-neon` | no `callconvention-hard` | `0x5000002` | PDK, Debian armel |

`armv7at-neon` is EABI5 with the soft-float *calling convention* over a real
VFP/NEON unit — byte-for-byte the ABI Palm's 2010 toolchain emitted. That is what
`conf/machine/pdk-armel.conf` selects.

## Enabling it

In `conf/bblayers.conf`:

```
BBLAYERS += "/path/to/pdk-luneos/yocto"
```

In `conf/local.conf`:

```
BBMULTICONFIG = "pdk-armel"
DISTRO_FEATURES:append = " pdk"
IMAGE_INSTALL:append = " packagegroup-luneos-pdk"
```

Then build normally:

```sh
bitbake luneos-image
```

The multiconfig dependency pulls the soft-float userland in automatically. To
build just that part:

```sh
bitbake mc:pdk-armel:pdk-sysroot-image
```

The `pdk` distro feature is only read by `qemu_%.bbappend`, which narrows target
qemu to `arm-linux-user` and drops SDL, KVM, Xen and virgl. Without the feature
the layer changes nothing about anyone else's qemu.

## What each recipe does

| recipe | |
|---|---|
| `conf/machine/pdk-armel.conf` | the soft-float pseudo-machine |
| `conf/multiconfig/pdk-armel.conf` | separate `TMPDIR`, minimal distro features, Mesa cut down to swrast + freedreno |
| `pdk-sysroot-image` | the soft-float rootfs: glibc, Mesa, SDL2 + sdl12-compat, SDL helpers, codecs, and the shims |
| `sdl12-compat` | stock upstream, with its soname renamed to `libSDL12compat.so.0` on `pdk-armel` so the webOS shim can own `libSDL-1.2.so.0` |
| `pdk-luneos` | the four shims. `COMPATIBLE_MACHINE = "pdk-armel"` — building them hard-float would corrupt every float crossing into them |
| `pdk-sysroot` | unpacks the result into `/opt/pdk/sysroot` in the main image |
| `pdk-sysroot-fonts` | the font filenames twelve titles open by absolute path |
| `pdk-tools` | `pdk-run` and `install-games.sh` |
| `packagegroup-luneos-pdk` | the lot, plus `luna-send` and a PulseAudio server |
| `qemu_%.bbappend` | trims target qemu, only when `pdk` is in `DISTRO_FEATURES` |

## Two modes for the sysroot

### Mode 1 — built from source (default)

Everything comes out of OpenEmbedded recipes at the soft-float tune. Clean,
reproducible, no Debian involved, and the correct long-term answer.

Costs: it builds glibc, LLVM and Mesa a second time, which is not cheap. And it
cannot supply the Palm libraries some titles need — those are not
redistributable and are not built from source by anyone.

### Mode 2 — a tarball from `tools/mk-sysroot.sh`

```
PDK_SYSROOT_TARBALL = "file:///srv/pdk/pdk-sysroot.tar.bz2"
PDK_SYSROOT_TARBALL_SHA256 = "..."
```

Skips the multiconfig entirely and installs a tree assembled outside BitBake from
Debian armel packages plus the optional proprietary overlay. This is what the
measured compatibility numbers refer to, and it is the only route that gets the
legacy ffmpeg 0.6 libraries, `libpalmvibe.so` and Palm's `libSDL_mixer` that some
titles need.

Use mode 2 to reproduce known-good behaviour; use mode 1 for something shippable.

## Known gaps

Stating these plainly, because two of them will bite on the first build:

**1. SDL2 version vs. luna-surfacemanager.** oe-core scarthgap ships SDL2 2.30.1.
LuneOS's compositor advertises `wl_shell` and `wl_webos_shell` but **not
`xdg_wm_base`**, and SDL2 removed its `wl_shell` backend in 2.0.16. So a
from-source sysroot will build fine and then fail to get a window on LuneOS.

The fix is in luna-surfacemanager, not here: **add `XdgShell`**. Until then, mode 1
works on a normal compositor (a dev workstation) but not on a device, and mode 2
pins SDL2 to 2.0.14. Pinning oe-core's `libsdl2` down to 2.0.14 inside the
multiconfig is possible but that release does not build cleanly against a modern
sysroot, so it is not offered here.

This is the single highest-leverage outstanding item in the project.

**2. Fonts.** Palm's Prelude family is not redistributable. `pdk-sysroot-fonts`
symlinks the filenames twelve titles hardcode to DejaVu Sans Condensed so
`TTF_OpenFont` returns non-NULL and they do not crash. Text will not look right.
Point `PDK_WEBOS_FONTS` at a directory of real Prelude fonts if you have a device
image.

**3. `freedreno` on real hardware is untested.** `conf/multiconfig/pdk-armel.conf`
selects `swrast,freedreno`, on the expectation that a2xx covers the TouchPad's
Adreno 220. Nobody has run it. On the emulator llvmpipe is used and works.

**4. `SRCREV = "${AUTOREV}"`** in `pdk-luneos_git.bb` and `pdk-tools_1.0.bb`. Pin
both to a release commit before shipping an image.

**5. Image size.** The soft-float userland is a second glibc, a second Mesa and a
second SDL2 — a few hundred megabytes. On a 4 GB device that matters. Nothing in
the layer tries to deduplicate against the host userland, and it could not: the
ABIs differ.

## Where the recipes live

They are in **this repository**, under `yocto/`, and the layer is added to
`bblayers.conf` by path — not copied into `meta-webos-ports`. That keeps them
versioned alongside the code they build, and keeps them out of the way of a layer
that has lost uncommitted work to tooling before.

If you would rather have them in `meta-luneos`, the tree maps directly onto
`meta-webos-ports/meta-luneos/recipes-pdk/`; only `conf/layer.conf` becomes
redundant.

## Verifying a build

The check that matters, on the built image:

```sh
readelf -h /opt/pdk/sysroot/usr/lib/libpdl.so | grep Flags
# Flags: 0x5000002, Version5 EABI, soft-float ABI
```

If that says hard-float, the multiconfig tune did not take effect and nothing
involving a `float` will work correctly.

Then:

```sh
/opt/pdk/pdk-run /usr/palm/applications/com.example.game thegame
```
