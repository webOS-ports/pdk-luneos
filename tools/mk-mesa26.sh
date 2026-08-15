#!/bin/bash
# Cross-build Mesa 26.2 with the panfrost driver for the armel PDK sysroot.
#
# Debian armel has no hardware GL driver at all - its Mesa 22.3 ships swrast and a
# pile of kmsro display stubs, because armel targets ARMv5 hardware that never had
# a GPU. That left the PDK games on llvmpipe at a few FPS while the device's Mali
# sat idle. This builds the driver Debian cannot supply.
#
# The result is installed BESIDE Debian's Mesa, in $SYSROOT/usr/lib/mesa26, not
# over it: this build has no llvmpipe (LLVM is a large dependency panfrost does
# not need) and a 26.2 libEGL cannot load Debian's 22.3 DRI drivers, so replacing
# the stack outright would leave nothing to fall back on. pdk-run prefers mesa26
# and honours PDK_SOFTWARE_GL=1 to go back to llvmpipe.
#
# Verified on a PineTab2 (RK3566, Mali-G52):
#     GL_VERSION:  OpenGL ES 3.1 Mesa 26.2.0
#     GL_RENDERER: Mali-G52 r1 MC1 (Panfrost)
#
# Usage: tools/mk-mesa26.sh [WORKDIR]

set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$TOP/mesa26-build}"
SYSROOT="${PDK_SYSROOT:-$TOP/sysroot}"
DEVSYSROOT="$WORK/sysroot-dev"
MESA_VER=26.2.0
SUITE=bookworm
MIRROR=https://deb.debian.org/debian

# Yocto has already built these for LuneOS itself; borrowing them avoids building
# LLVM and clang for the host. Point at another build tree if yours differs.
YOCTO_SYSROOTS="${YOCTO_SYSROOTS:-/home/herrie/wrynose-tmp/tmp/sysroots-components/x86_64}"

say() { echo "== $*"; }

mkdir -p "$WORK"/{debs,native-bin}
cd "$WORK"

# --- host-side tooling -----------------------------------------------------
# No pip on this host, so meson and mako are unpacked from tarballs and run from
# where they land. Mesa 26.2 needs meson >= 1.4.
[ -d meson-1.8.2 ] || { curl -sfL -o meson.tar.gz \
    https://github.com/mesonbuild/meson/releases/download/1.8.2/meson-1.8.2.tar.gz
    tar xf meson.tar.gz; }
[ -d Mako-1.3.5 ] || { curl -sfL -o mako.tar.gz \
    https://files.pythonhosted.org/packages/source/M/Mako/Mako-1.3.5.tar.gz
    tar xf mako.tar.gz; }
[ -d MarkupSafe-2.1.5 ] || { curl -sfL -o markupsafe.tar.gz \
    https://files.pythonhosted.org/packages/source/M/MarkupSafe/MarkupSafe-2.1.5.tar.gz
    tar xf markupsafe.tar.gz; }

export PYTHONPATH="$WORK/Mako-1.3.5:$WORK/MarkupSafe-2.1.5/src"
MESON="python3 $WORK/meson-1.8.2/meson.py"

# --- mesa source -----------------------------------------------------------
# Note the path: archive.mesa3d.org keeps only recent releases at the top level.
[ -d "mesa-$MESA_VER" ] || {
    say "fetching mesa $MESA_VER"
    curl -sfL -o "mesa-$MESA_VER.tar.xz" "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz"
    tar xf "mesa-$MESA_VER.tar.xz"
}

# --- a sysroot with headers ------------------------------------------------
# The runtime sysroot is deployed to devices, so the -dev packages go into a copy
# rather than bloating it. Same unprivileged dpkg-deb approach as mk-sysroot.sh.
if [ ! -d "$DEVSYSROOT" ]; then
    say "building dev sysroot from $SYSROOT"
    cp -a "$SYSROOT" "$DEVSYSROOT"

    DEV_PKGS="libdrm-dev libexpat1-dev zlib1g-dev libzstd-dev libwayland-dev
              libwayland-bin libwayland-egl-backend-dev libglvnd-dev libglvnd-core-dev
              libffi-dev libstdc++-12-dev libx11-dev libxext-dev libxcb1-dev
              libxrandr-dev libxshmfence-dev libxxf86vm-dev"

    curl -sL -o Packages.gz "$MIRROR/dists/$SUITE/main/binary-armel/Packages.gz"
    curl -sL -o Packages-all.gz "$MIRROR/dists/$SUITE/main/binary-all/Packages.gz"
    python3 - "$MIRROR" $DEV_PKGS <<'PY' > urls.txt
import gzip, sys
mirror, want = sys.argv[1], set(sys.argv[2:]) | {'wayland-protocols'}
for index in ('Packages.gz', 'Packages-all.gz'):
    cur = {}
    for line in gzip.open(index, 'rt', errors='replace'):
        line = line.rstrip('\n')
        if not line:
            if cur.get('Package') in want:
                print(mirror + '/' + cur['Filename'])
            cur = {}
            continue
        if ': ' in line and not line.startswith(' '):
            k, v = line.split(': ', 1)
            cur[k] = v
PY
    ( cd debs && xargs -n1 -P8 curl -sfLO < ../urls.txt )
    for d in debs/*.deb; do dpkg-deb -x "$d" "$DEVSYSROOT"; done
fi

# --- native wayland-scanner ------------------------------------------------
# Mesa insists the scanner version equals the target libwayland (1.21.0 here) and
# checks the binary itself, so a newer host scanner will not do and a .pc override
# does not help. dtd validation is off to avoid needing libxml2 headers.
if [ ! -x "$WORK/native-wl/bin/wayland-scanner" ]; then
    say "building native wayland-scanner 1.21.0"
    [ -d wayland-1.21.0 ] || {
        curl -sfL -o wayland-1.21.0.tar.xz \
            https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.21.0/downloads/wayland-1.21.0.tar.xz
        tar xf wayland-1.21.0.tar.xz
    }
    # expat headers for the build host, relocated so pkg-config finds them
    if [ ! -f native-deps/usr/lib/x86_64-linux-gnu/pkgconfig/expat.pc ]; then
        mkdir -p native-deps
        ( cd debs
          for u in pool/main/e/expat/libexpat1-dev_2.5.0-1+deb12u2_amd64.deb \
                   pool/main/e/expat/libexpat1_2.5.0-1+deb12u2_amd64.deb; do
              curl -sfLO "$MIRROR/$u"
          done )
        for d in debs/libexpat1*_amd64.deb; do dpkg-deb -x "$d" native-deps; done
        sed -i "s|^prefix=/usr|prefix=$WORK/native-deps/usr|" \
            native-deps/usr/lib/x86_64-linux-gnu/pkgconfig/expat.pc
        ln -sf libexpat.so.1 native-deps/usr/lib/x86_64-linux-gnu/libexpat.so
    fi
    PKG_CONFIG_PATH="$WORK/native-deps/usr/lib/x86_64-linux-gnu/pkgconfig" \
        $MESON setup build-wl wayland-1.21.0 -Dlibraries=false -Dtests=false \
        -Ddocumentation=false -Ddtd_validation=false -Dscanner=true \
        --prefix="$WORK/native-wl" >/dev/null
    $MESON install -C build-wl >/dev/null
fi

# --- native mesa_clc / vtn_bindgen2 ----------------------------------------
# Panfrost pulls in CLC (with_driver_using_cl lists it), which needs LLVM for the
# host tool. Rather than build LLVM+clang here, wrap the ones Yocto already built.
if [ ! -x "$WORK/native-bin/mesa_clc" ]; then
    say "wrapping Yocto's mesa_clc"
    [ -x "$YOCTO_SYSROOTS/mesa-native/usr/bin/mesa_clc" ] || {
        echo "no mesa_clc under $YOCTO_SYSROOTS - build mesa-native first, or set YOCTO_SYSROOTS" >&2
        exit 1
    }
    LP="$YOCTO_SYSROOTS/llvm-native/usr/lib:$YOCTO_SYSROOTS/clang-native/usr/lib"
    LP="$LP:$YOCTO_SYSROOTS/spirv-tools-native/usr/lib"
    LP="$LP:$YOCTO_SYSROOTS/spirv-llvm-translator-native/usr/lib"
    for t in mesa_clc vtn_bindgen2; do
        printf '#!/bin/sh\nexec env LD_LIBRARY_PATH=%s %s/mesa-native/usr/bin/%s "$@"\n' \
            "$LP" "$YOCTO_SYSROOTS" "$t" > "native-bin/$t"
        chmod +x "native-bin/$t"
    done
fi

# --- qemu wrapper ----------------------------------------------------------
# Mesa builds panfrost_compile for the target and runs it during the build to
# precompile shaders. It only takes that path when meson believes host binaries
# are runnable; without a wrapper it instead tries to link a target library into a
# build-machine binary, which meson refuses outright. Same trick Yocto uses.
QEMU_ARM="${QEMU_ARM:-$YOCTO_SYSROOTS/qemu-native/usr/bin/qemu-arm}"
[ -x "$QEMU_ARM" ] || { echo "no qemu-arm at $QEMU_ARM (set QEMU_ARM=)" >&2; exit 1; }
printf '#!/bin/sh\nexec %s -L %s "$@"\n' "$QEMU_ARM" "$DEVSYSROOT" > qemu-armel-wrapper
chmod +x qemu-armel-wrapper

# --- cross file ------------------------------------------------------------
# Two things here are load-bearing and cost an afternoon each:
#
#  * Link-only flags go in c_link_args, never in the compiler command. Meson runs
#    its feature checks compile-only with -Werror=unused-command-line-argument, so
#    a stray --ld-path or -L there fails every check. Nothing errors out - the
#    checks simply all answer "no" and Mesa builds against a libc it believes has
#    no struct timespec, then dies far away redefining it.
#
#  * -mfpu=neon, not vfpv3. u_format_unpack_neon.c reaches for NEON behind a
#    #pragma GCC target("fpu=neon") that clang does not implement for ARM, so the
#    file will not compile unless NEON is on build-wide. Every LuneOS ARM target
#    has NEON and it does not change the softfp calling convention, so the result
#    stays ABI-compatible with the Palm binaries (readelf: soft-float ABI).
GCCDIR="$DEVSYSROOT/usr/lib/gcc/arm-linux-gnueabi/12"
LLD="${PDK_LLD:-$TOP/tools/host/ld.lld}"
cat > armel-cross.txt <<EOF
[binaries]
c = ['clang', '--target=arm-linux-gnueabi', '-march=armv7-a', '-mfloat-abi=softfp', '-mfpu=neon', '--sysroot=$DEVSYSROOT']
cpp = ['clang++', '--target=arm-linux-gnueabi', '-march=armv7-a', '-mfloat-abi=softfp', '-mfpu=neon', '--sysroot=$DEVSYSROOT']
ar = '${LLVM_AR:-llvm-ar-21}'
strip = '${LLVM_STRIP:-llvm-strip-21}'
pkg-config = 'pkg-config'
exe_wrapper = '$WORK/qemu-armel-wrapper'

[built-in options]
c_link_args = ['--ld-path=$LLD', '-B$GCCDIR', '-L$GCCDIR']
cpp_link_args = ['--ld-path=$LLD', '-B$GCCDIR', '-L$GCCDIR']

[properties]
needs_exe_wrapper = true
sys_root = '$DEVSYSROOT'
pkg_config_libdir = '$DEVSYSROOT/usr/lib/arm-linux-gnueabi/pkgconfig:$DEVSYSROOT/usr/share/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
EOF

# --- configure and build ---------------------------------------------------
# legacy-wayland=bind-wayland-display is required, not optional: it gates the
# whole client-side wl_drm path. Without it the games get "failed to get driver
# name for fd -1" and "failed to create dri2 screen" on this compositor, because
# it does not advertise linux-dmabuf feedback. LuneOS's own Yocto mesa recipe
# passes the same flag.
say "configuring mesa"
export PATH="$WORK/native-wl/bin:$WORK/native-bin:$PATH"
export PKG_CONFIG_PATH="$WORK/native-wl/lib/x86_64-linux-gnu/pkgconfig:$WORK/native-wl/share/pkgconfig"

$MESON setup build26 "mesa-$MESA_VER" --cross-file armel-cross.txt \
    -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms=wayland \
    -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dglvnd=enabled \
    -Dgles1=enabled -Dgles2=enabled -Dopengl=true -Dllvm=disabled \
    -Dvideo-codecs= -Dmesa-clc=system -Dlegacy-wayland=bind-wayland-display \
    -Dbuildtype=release

say "building"
$MESON compile -C build26

say "installing into $SYSROOT/usr/lib/mesa26"
rm -rf stage26
DESTDIR="$WORK/stage26" $MESON install -C build26 --skip-subprojects >/dev/null

DEST="$SYSROOT/usr/lib/mesa26"
rm -rf "$DEST"
mkdir -p "$DEST"/{lib/gbm,egl_vendor.d,drirc.d}
cp -a stage26/usr/local/lib/libgallium-$MESA_VER.so \
      stage26/usr/local/lib/libEGL_mesa.so.0.0.0 \
      stage26/usr/local/lib/libgbm.so.1.0.0 "$DEST/lib/"
cp -a stage26/usr/local/lib/gbm/dri_gbm.so "$DEST/lib/gbm/"
( cd "$DEST/lib" && ln -sf libEGL_mesa.so.0.0.0 libEGL_mesa.so.0 && ln -sf libgbm.so.1.0.0 libgbm.so.1 )
cp stage26/usr/local/share/glvnd/egl_vendor.d/50_mesa.json "$DEST/egl_vendor.d/"
cp stage26/usr/local/share/drirc.d/*.conf "$DEST/drirc.d/"

say "done: $(du -sh "$DEST" | cut -f1) in $DEST"
say "check it with: pdk-run's egltest - expect GL_RENDERER: Mali-... (Panfrost)"
