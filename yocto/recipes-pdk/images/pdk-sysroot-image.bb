SUMMARY = "ARM soft-float userland for legacy webOS PDK applications"
DESCRIPTION = "The complete environment a 2010-2012 webOS PDK/SDL game runs \
inside: glibc, Mesa, SDL2 + sdl12-compat, the SDL helper libraries, and the \
pdk-luneos shims that supply the Palm-only APIs. Built at the soft-float ABI and \
shipped inside a normal (hard-float) LuneOS image at /opt/pdk/sysroot."

LICENSE = "MIT"

# This is not a bootable image - it is a library tree that another image carries.
# No kernel, no init, no package manager, no login.
IMAGE_FEATURES = ""
IMAGE_LINGUAS = ""
IMAGE_FSTYPES = "tar.bz2"
NO_RECOMMENDATIONS = "1"

inherit image

IMAGE_INSTALL = " \
    ${PDK_SYSROOT_BASE} \
    ${PDK_SYSROOT_GRAPHICS} \
    ${PDK_SYSROOT_SDL} \
    ${PDK_SYSROOT_MEDIA} \
    ${PDK_SYSROOT_SHIMS} \
"

# glibc and the handful of C-library-adjacent things every title touches.
PDK_SYSROOT_BASE = " \
    base-files \
    glibc \
    libgcc \
    libstdc++ \
    zlib \
    curl \
    openssl \
    sqlite3 \
    libpng \
    jpeg \
    freetype \
    fontconfig \
"

# Mesa, and the Wayland client side. There is no X11 in here: SDL2 talks Wayland
# to luna-surfacemanager directly.
PDK_SYSROOT_GRAPHICS = " \
    mesa \
    mesa-megadriver \
    libegl-mesa \
    libgles1-mesa \
    libgles2-mesa \
    wayland \
    libxkbcommon \
"

PDK_SYSROOT_SDL = " \
    libsdl2 \
    sdl12-compat \
    libsdl2-image \
    libsdl2-ttf \
    libsdl2-mixer \
"

# Games load music through SDL_mixer, which needs the codecs present at runtime.
PDK_SYSROOT_MEDIA = " \
    libvorbis \
    libogg \
    flac \
    alsa-lib \
    libpulse \
    libpulse-simple \
"

PDK_SYSROOT_SHIMS = "pdk-luneos"

# Fonts: twelve titles hardcode /usr/share/fonts/PreludeCondensed-Medium.ttf and
# never check TTF_OpenFont for NULL, so a missing font is a crash rather than
# ugly text. The webOS fonts are not redistributable, so a metric-compatible
# substitute is installed under the expected names by pdk-sysroot-fonts; see
# docs/yocto.md.
IMAGE_INSTALL += "pdk-sysroot-fonts"

# Nothing in this tree is ever executed by the host system directly, and the ABI
# deliberately differs from the image that carries it.
IMAGE_ROOTFS_EXTRA_SPACE = "0"
