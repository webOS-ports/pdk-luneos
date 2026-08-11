#!/bin/bash
# Build the hybrid ARM softfp sysroot used to run legacy webOS PDK apps.
#
# Debian armel is EABI soft-float - byte-for-byte the ABI the Palm PDK emitted in
# 2010 - so its packages drop straight in. The base is modern (glibc 2.36, Mesa,
# SDL2, sdl12-compat) with only the Palm-specific libraries overlaid on top.
#
# Direction matters: glibc is backward compatible, so 2011 binaries run happily on
# glibc 2.36 but never the reverse. Always modern-base + legacy-overlay.
#
# Unprivileged throughout: packages are fetched and unpacked with dpkg-deb, no
# root, no debootstrap, no multiarch setup on the host.
#
# Usage: tools/mk-sysroot.sh [SYSROOT_DIR]

set -euo pipefail

SYSROOT="${1:-$HOME/webos/pdk-luneos/sysroot}"
WORK="${SYSROOT}.work"
SUITE=bookworm
MIRROR=https://deb.debian.org/debian

# Legacy webOS rootfs to take the Palm-only libraries from.
LEGACY="${LEGACY_ROOTFS:-/home/herrie/webos/305att/nova-cust-image-topaz.rootfs-att}"
# A few libraries are not in the 3.0.5 TouchPad image but are in the Veer 2.1.1
# one (libpalmvibe.so, the Immersion vibrator API). Same ARM EABI5 softfp ABI.
VEER="${VEER_ROOTFS:-/home/herrie/webos/Veer/Doctor211}"

# Modern base. Mesa 22.3 + llvmpipe, SDL2 + sdl12-compat, glibc 2.36.
BASE_PKGS="
libc6 libgcc-s1 libstdc++6 libc6-dev linux-libc-dev libgcc-12-dev
libgl1-mesa-dri libegl1 libegl-mesa0 libgles1 libgles2 libglapi-mesa libglvnd0
libglx-mesa0 libgbm1 libdrm2 libdrm-amdgpu1 libdrm-nouveau2 libdrm-radeon1
libllvm15 libelf1 libzstd1 liblzma5 libffi8 libedit2 libtinfo6 libncursesw6
libunwind8 libsensors5 libpciaccess0 libexpat1 zlib1g libbsd0 libmd0
libx11-6 libx11-xcb1 libxcb1 libxau6 libxdmcp6 libxext6 libxfixes3 libxshmfence1
libxcb-dri2-0 libxcb-dri3-0 libxcb-present0 libxcb-sync1 libxcb-xfixes0
libwayland-client0 libwayland-server0 libwayland-egl1 libwayland-cursor0
libsdl1.2-compat libsdl2-2.0-0 libsdl2-image-2.0-0 libsdl2-mixer-2.0-0
libsdl2-net-2.0-0 libsdl2-ttf-2.0-0
libasound2 libpulse0 libsamplerate0 libxcursor1 libxi6 libxrandr2 libxss1
libxkbcommon0 libdecor-0-0 libxrender1 libsndfile1 libasyncns0 libapparmor1
libflac12 libvorbis0a libvorbisenc2 libogg0 libopus0 libmpg123-0 libmp3lame0
libdbus-1-3 libsystemd0 libgcrypt20 libgpg-error0 liblz4-1 libcap2 libxml2 libicu72
"

# Palm libraries with no modern substitute. Deliberately excluded:
#   libc/libm/libpthread/libdl/librt/ld-linux  - Debian's glibc 2.36 supersedes them
#   libEGL/libGLESv2/libGLES_CM/libPiranha-GL  - replaced by Mesa (the originals are
#                                                the Qualcomm Adreno blob and need
#                                                /dev/kgsl-3d0 from the 2011 kernel)
#   libstdc++/libgcc_s                         - Debian's carry the old symbol versions
#   libSDL-1.2, libpdl                         - replaced by our own builds
LEGACY_LIBS="
libSDL_image-1.2.so.0.1.6 libSDL_mixer-1.2.so.0.10.1 libSDL_ttf-2.0.so.0.10.0
libSDL_net-1.2.so.0.0.7 libSDL_cinema.so
libnapp.so libPiranha.so libhelpers.so libhelpers-ex.so
libLunaSysMgrIpc.so liblunaservice.so libluna-prefs.so libLunaKeymaps.so
libPmLogLib.so libdlmalloc.so libaffinity.so.0 libgoodfork.so.0
libcjson.so libmjson.so libpbnjson_c.so libpbnjson_cpp.so libyajl.so.1
libhal.so libhid.so libWebOsProxy.so libcares.so.2
libssl.so.0.9.8 libcrypto.so.0.9.8 libsqlite3.so.0 libcurl.so.4
libglib-2.0.so.0 libgthread-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0
libjpeg.so.62 libpng12.so.0 libfreetype.so.6
"

say() { printf '\n== %s\n' "$*"; }

mkdir -p "$WORK/debs" "$SYSROOT"

say "fetching the $SUITE armel package index"
[ -f "$WORK/Packages.gz" ] || \
  curl -sL -o "$WORK/Packages.gz" "$MIRROR/dists/$SUITE/main/binary-armel/Packages.gz"

say "resolving package filenames"
python3 - "$WORK/Packages.gz" "$WORK/urls.txt" $BASE_PKGS <<'PY'
import gzip, re, sys
index, out, want = sys.argv[1], sys.argv[2], set(sys.argv[3:])
cur, found = {}, {}
for line in gzip.open(index, 'rt', errors='replace'):
    line = line.rstrip('\n')
    if line == '':
        p = cur.get('Package')
        if p in want and p not in found:
            found[p] = cur.get('Filename')
        cur = {}
        continue
    m = re.match(r'^(\w[\w.+-]*): (.*)$', line)
    if m:
        cur[m.group(1)] = m.group(2)
with open(out, 'w') as f:
    for p, fn in sorted(found.items()):
        f.write(f"https://deb.debian.org/debian/{fn}\n")
missing = want - set(found)
print(f"  resolved {len(found)}/{len(want)}")
if missing:
    print("  MISSING:", " ".join(sorted(missing)))
PY

say "downloading packages"
( cd "$WORK/debs" && xargs -n1 -P8 curl -sfLO < "$WORK/urls.txt" )

say "unpacking into $SYSROOT"
for d in "$WORK"/debs/*.deb; do dpkg-deb -x "$d" "$SYSROOT"; done

# The 2011 loader knows nothing about multiarch, so flatten the layout.
say "flattening multiarch directories"
for d in usr/lib lib usr/include; do
    [ -d "$SYSROOT/$d/arm-linux-gnueabi" ] && \
        cp -a "$SYSROOT/$d/arm-linux-gnueabi/." "$SYSROOT/$d/" || true
done

say "overlaying Palm libraries from $LEGACY"
if [ -d "$LEGACY" ]; then
    n=0
    for f in $LEGACY_LIBS; do
        if [ -e "$LEGACY/usr/lib/$f" ]; then
            cp -a "$LEGACY/usr/lib/$f" "$SYSROOT/usr/lib/" && n=$((n+1))
        fi
    done
    echo "  copied $n legacy libraries"
    ( cd "$SYSROOT/usr/lib"
      ln -sf libSDL_image-1.2.so.0.1.6  libSDL_image-1.2.so.0
      ln -sf libSDL_mixer-1.2.so.0.10.1 libSDL_mixer-1.2.so.0
      ln -sf libSDL_ttf-2.0.so.0.10.0   libSDL_ttf-2.0.so.0
      ln -sf libSDL_net-1.2.so.0.0.7    libSDL_net-1.2.so.0 )
else
    echo "  WARNING: legacy rootfs not found at $LEGACY - set LEGACY_ROOTFS"
fi

say "pointing the legacy GL sonames at Mesa"
( cd "$SYSROOT/usr/lib"
  ln -sf libEGL.so.1        libEGL.so
  ln -sf libGLESv2.so.2     libGLESv2.so
  ln -sf libGLESv1_CM.so.1  libGLES_CM.so )

# sdl12-compat ships with soname libSDL-1.2.so.0, which our shim needs to own.
say "renaming sdl12-compat so the shim can own libSDL-1.2.so.0"
COMPAT=$(find "$SYSROOT" -name 'libSDL-1.2.so.1.2.60' | head -1)
if [ -n "$COMPAT" ]; then
    cp "$COMPAT" "$SYSROOT/usr/lib/libSDL12compat.so.0"
    if command -v patchelf >/dev/null 2>&1; then
        patchelf --set-soname libSDL12compat.so.0 "$SYSROOT/usr/lib/libSDL12compat.so.0"
        echo "  ok"
    else
        echo "  WARNING: patchelf not found - soname not changed, the shim will not link"
    fi
else
    echo "  WARNING: sdl12-compat not found in the sysroot"
fi

# luna-surfacemanager advertises wl_shell and wl_webos_shell but NOT xdg_wm_base.
# SDL2 dropped wl_shell in 2.0.16, so anything newer segfaults against it. 2.0.14 is
# the last release that still speaks it. (A desktop compositor works with either, so
# this only matters on the LuneOS target.)
# Palm's device had unversioned .so symlinks and a number of packages link those
# names directly (libSDL_image.so, libSDL_ttf.so, libz.so, even libPDL.so with a
# capital PDL). Without them the loader fails with "error while loading shared
# libraries" before the app gets a chance to run.
# webOS system fonts. Apps hardcode absolute paths like
# /usr/share/fonts/PreludeCondensed-Medium.ttf; without them TTF_OpenFont returns
# NULL, and none of them check the result, so they segfault inside
# TTF_RenderText_Blended. 11 of 12 SDL_ttf crashes in the corpus were only this.
say "libraries only present in other webOS images"
[ -e "$VEER/usr/lib/libpalmvibe.so" ] && cp -a "$VEER/usr/lib/libpalmvibe.so" "$SYSROOT/usr/lib/" && echo "  libpalmvibe.so (from Veer 2.1.1)"
# com.gameloft.app.tennis links libamrnb.so.3, which no image ships under that
# soname; it references none of its symbols, so point it at the real codec.
if [ -e "$LEGACY/lib/libamrnbcodec.so" ]; then
    cp -a "$LEGACY/lib/libamrnbcodec.so" "$SYSROOT/usr/lib/"
    ( cd "$SYSROOT/usr/lib" && ln -sf libamrnbcodec.so libamrnb.so.3 )
    echo "  libamrnb.so.3 -> libamrnbcodec.so"
fi

say "installing webOS system fonts"
if [ -d "$LEGACY/usr/share/fonts" ]; then
    mkdir -p "$SYSROOT/usr/share/fonts"
    cp -a "$LEGACY/usr/share/fonts/." "$SYSROOT/usr/share/fonts/"
    echo "  $(ls "$SYSROOT/usr/share/fonts" | wc -l) fonts"
else
    echo "  WARNING: no fonts in the legacy rootfs - SDL_ttf apps will crash"
fi

say "creating unversioned library symlinks"
( cd "$SYSROOT/usr/lib"
  ln -sf libSDL_image-1.2.so.0 libSDL_image.so
  ln -sf libSDL_mixer-1.2.so.0 libSDL_mixer.so
  ln -sf libSDL_ttf-2.0.so.0   libSDL_ttf.so
  ln -sf libSDL_net-1.2.so.0   libSDL_net.so
  ln -sf libpdl.so             libPDL.so
  [ -e libstdc++.so ] || ln -sf libstdc++.so.6 libstdc++.so
  [ -e libz.so.1 ]    || ln -sf ../../lib/libz.so.1 libz.so.1
  ln -sf libz.so.1 libz.so
  for n in libgcc_s libm libdl libpthread librt; do
      [ -e "$n.so" ] && continue
      t=$(ls ../../lib/$n.so.[0-9] 2>/dev/null | head -1)
      [ -n "$t" ] && ln -sf "$t" "$n.so"
  done ) 2>/dev/null
echo "  ok"

say "pinning SDL2 to 2.0.14 for luna-surfacemanager's wl_shell"
if [ -e "$SYSROOT/usr/lib/libSDL2-2.0.so.0.14.0" ]; then
    ln -sf libSDL2-2.0.so.0.14.0 "$SYSROOT/usr/lib/libSDL2-2.0.so.0"
    echo "  ok"
else
    echo "  WARNING: 2.0.14 not present; fetch libsdl2-2.0-0 from Debian bullseye armel"
fi

say "done: $SYSROOT ($(du -sh "$SYSROOT" | cut -f1))"
echo "next: make && make install"
