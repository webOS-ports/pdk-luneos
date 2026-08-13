# Building into a LuneOS image

The `yocto/` directory in this repository is a complete BitBake layer
(`meta-pdk`). Adding it to a LuneOS build produces `/opt/pdk` in the image —
sysroot, shims and launcher — instead of hand-copying files to a device.

> **Status: builds.** Verified against a real LuneOS scarthgap tree,
> `MACHINE=tissot-halium`:
>
> * `bitbake -n luneos-image` — exit 0, no errors, 14612 tasks planned
> * `bitbake mc:pdk-armel:pdk-sysroot-image` — builds, 42 MB `tar.xz`
> * `bitbake packagegroup-luneos-pdk` — builds; `/opt/pdk/sysroot` is 141 MB
>   unpacked, a 58 MB ipk, plus `/opt/pdk/pdk-run`
> * every shim in the image reports `0x5000200, Version5 EABI, soft-float ABI`
> * `libSDL-1.2.so.0` carries `SONAME libSDL-1.2.so.0` and
>   `NEEDED libSDL12compat.so.0` — the soname swap works as designed
> * `swrast_dri.so` (llvmpipe), `msm_dri.so` and `kgsl_dri.so` (freedreno) are
>   all present
>
> What has **not** been done: run a game against a Yocto-built sysroot. The
> artefacts are correct by inspection; nobody has launched anything with them.
> Expect the `xdg_wm_base` problem below to bite on a device. The prebuilt path
> (mode 2) remains the configuration the 81 % compatibility figure was measured
> against.

## The problem this has to solve

A PDK application and everything it loads must be **ARM soft-float**. LuneOS is
hard-float. So the build has to produce two userlands with incompatible ABIs and
put one inside the other.

BitBake has exactly the right mechanism for this: **multiconfig**. A second
configuration, with its own `TMPDIR` and its own tune, builds the soft-float tree
from the same recipes; the main build consumes the result through `mcdepends`.

```
mc:pdk-armel   ──build──▶  pdk-sysroot-image (tar.xz, ARM softfp)
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
| `armv7athf-neon` | includes `callconvention-hard` | `0x5000400` | LuneOS |
| `armv7at-neon` | no `callconvention-hard` | `0x5000200` | this multiconfig, Debian armel |

`armv7at-neon` is EABI5 with the soft-float *calling convention* over a real
VFP/NEON unit — byte-for-byte the ABI Palm's 2010 toolchain emitted. That is what
`conf/machine/pdk-armel.conf` selects.

## Enabling it

**In a LuneOS checkout**, the recipes are already in `meta-luneos` (see
[Where the recipes live](#where-the-recipes-live)), so there is no
`bblayers.conf` change to make. Anywhere else, add the standalone layer:

```
BBLAYERS += "/path/to/pdk-luneos/yocto"
```

Either way, in `conf/local.conf`:

```
BBMULTICONFIG = "pdk-armel"
PDK_QEMU_USERMODE_ONLY = "1"

# Masking the webOS and BSP layers in the multiconfig makes bitbake warn once per
# empty layer on every parse. These must live here, not in the multiconfig:
# cooker.py reads them from the base datastore, via
# collection_priorities(..., self.data), even though the warning is per-mc.
BBFILE_PATTERN_IGNORE_EMPTY_meta-luneui = "1"
BBFILE_PATTERN_IGNORE_EMPTY_pine64-luneos-layer = "1"
# ... one per masked layer; see conf/local.conf in the LuneOS tree
IMAGE_INSTALL:append:pn-luneos-image = " packagegroup-luneos-pdk"
IMAGE_INSTALL:append:pn-luneos-dev-image = " packagegroup-luneos-pdk"
```

**Scope that append with `:pn-`.** A bare `IMAGE_INSTALL:append` is global: it
lands on *every* image in the build, including the initramfs, which is how
141 MB of `/opt/pdk` ends up somewhere it very much should not be —

```
The initramfs size 652101(K) exceeds INITRAMFS_MAXSIZE: 131072(K)
```

— and on `pdk-sysroot-image` itself, which would then try to install the
LuneOS-side packagegroup into the soft-float guest tree. The multiconfig
defends against the second case with `IMAGE_INSTALL:remove`; nothing defends
against the first but naming the images you mean.

Then build normally:

```sh
bitbake luneos-image
```

The multiconfig dependency pulls the soft-float userland in automatically. To
build just that part:

```sh
bitbake mc:pdk-armel:pdk-sysroot-image
```

`PDK_QEMU_USERMODE_ONLY` is read only by `qemu_%.bbappend`, which narrows target
qemu to `arm-linux-user` and drops SDL, KVM, Xen and virgl. Left unset, the layer
changes nothing about anyone else's qemu.

It is a private variable rather than a `DISTRO_FEATURE` on purpose.
`DISTRO_FEATURES` is part of the task signature of a large share of recipes, so
adding one to an existing build invalidates sstate broadly and triggers a big
rebuild — a steep price for configuring a single recipe.

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
| `qemu_%.bbappend` | trims target qemu, only when `PDK_QEMU_USERMODE_ONLY = "1"` |

## Two modes for the sysroot

### Mode 1 — built from source (default)

Everything comes out of OpenEmbedded recipes at the soft-float tune. Clean,
reproducible, no Debian involved, and the correct long-term answer.

Costs: it builds glibc, LLVM and Mesa a second time, which is not cheap. And it
cannot supply the Palm libraries some titles need — those are not
redistributable and are not built from source by anyone.

### Mode 2 — a tarball from `tools/mk-sysroot.sh`

```
PDK_SYSROOT_TARBALL = "file:///srv/pdk/pdk-sysroot.tar.xz"
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

**1. luna-surfacemanager needs the `xdg_wm_base` patch.** oe-core scarthgap ships
SDL2 2.30.1, which has no `wl_shell` backend, and the stock compositor advertises
only `wl_shell` and `wl_webos_shell`. Without the patch a game builds and runs
but never gets a window.

meta-luneos carries
`recipes-webos-ose/luna-surfacemanager/luna-surfacemanager/0016-Advertise-xdg_wm_base-so-modern-toolkits-can-map-wind.patch`,
which adds a `QWaylandXdgShell` next to the existing `QWaylandWlShell`. If you
are building against an unpatched luna-surfacemanager, carry that patch or pin
SDL2 to 2.0.14 in the multiconfig.

**2. Fonts — mostly a non-issue.** LuneOS already ships the complete Prelude
family through `luna-init-fonts`, all 34 faces the legacy image had, so
`pdk-sysroot-fonts` simply depends on it. What LuneOS does not carry is the
Microsoft core fonts Palm licensed (Arial, Times New Roman, Courier New, Georgia,
Verdana, Lucida Console) plus four CJK faces. Liberation is metric-compatible with
the first three — same advance widths, so text laid out for Arial still fits —
and Georgia/Verdana fall back to a face that reflows. `PDK_LEGACY_FONTS` takes the
originals from a device image if you have one.

**3. llvmpipe crashes under `qemu-arm`, and it is LLVM rather than Mesa.**
On the qemux86-64 emulator a GLES1 title segfaults on its first real GL work.
Tested: Mesa 24.0.7 crashes, Mesa 26.2.0 crashes identically, and the Debian
sysroot's Mesa 22.3 does not — the difference is LLVM 18 versus LLVM 15, not the
Mesa version. Raising Mesa to match the host did not help.

`pdk-run` defaults to `GALLIUM_DRIVER=softpipe` on the emulated path, which is
correct but roughly 7x slower. Only affects emulated targets; real ARM hardware
has no second JIT and uses freedreno. If the speed matters on the emulator, the
lead to pull is an older LLVM in the multiconfig, not a newer Mesa.

**4. `freedreno` on real hardware is untested.** `conf/multiconfig/pdk-armel.conf`
selects `swrast,freedreno`, on the expectation that a2xx covers the TouchPad's
Adreno 220. Nobody has run it. On the emulator llvmpipe is used and works.

**5. `SRCREV` is pinned to a commit, not a tag.** `pdk-luneos_git.bb` and
`pdk-tools_1.0.bb` name an explicit revision, which is right, but it has to be
bumped by hand whenever the shims change. Cut a tag and pin to that once the
interface settles.

**6. Image size.** The soft-float userland is a second glibc, a second Mesa and a
second SDL2 — a few hundred megabytes. On a 4 GB device that matters. Nothing in
the layer tries to deduplicate against the host userland, and it could not: the
ABIs differ.

## Where the recipes live

Two copies, deliberately:

* **`yocto/` in this repository** is the upstream copy and a complete standalone
  layer (`meta-pdk`), so the recipes are versioned alongside the code they build
  and anyone can use them without meta-webos-ports.
* **`meta-webos-ports/meta-luneos/`** carries the deployed copy in a LuneOS
  checkout — `recipes-pdk/`, `recipes-graphics/sdl12-compat/`,
  `recipes-devtools/qemu/`, plus `conf/machine/pdk-armel.conf` and
  `conf/multiconfig/pdk-armel.conf`. `conf/layer.conf` is not copied; meta-luneos
  has its own, and its `BBFILES` glob picks `recipes-pdk` up automatically.

Only one of the two should ever be in `bblayers.conf` — having both would give
BitBake duplicate recipes. In a LuneOS checkout that is meta-luneos, and no
`bblayers.conf` change is needed at all.

When changing a recipe, change it here and copy across. Keep an eye on drift.

## Verifying a build

The check that matters, on the built image:

```sh
readelf -h /opt/pdk/sysroot/usr/lib/libpdl.so | grep Flags
# Flags: 0x5000200, Version5 EABI, soft-float ABI
```

If that says hard-float, the multiconfig tune did not take effect and nothing
involving a `float` will work correctly.

Then:

```sh
/opt/pdk/pdk-run /usr/palm/applications/com.example.game thegame
```
