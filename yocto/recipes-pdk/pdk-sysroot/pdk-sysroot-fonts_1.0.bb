SUMMARY = "Font names legacy webOS PDK applications open by absolute path"
DESCRIPTION = "Twelve titles in the catalogue open \
/usr/share/fonts/PreludeCondensed-Medium.ttf directly and never check \
TTF_OpenFont for NULL, so a missing font is a segfault rather than ugly text. \
Palm's Prelude family is not redistributable, so this provides those filenames \
backed by a font that is."

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

inherit allarch

# Substituted rather than shipped. If you have a device image and want the real
# metrics, set PDK_WEBOS_FONTS to a directory containing the Prelude family and
# it will be used instead - see docs/yocto.md.
PDK_WEBOS_FONTS ?= ""

PDK_FONT_SUBSTITUTE ?= "${datadir}/fonts/truetype/DejaVuSansCondensed.ttf"

# The names applications hardcode.
PDK_FONT_NAMES = " \
    PreludeCondensed-Medium.ttf \
    PreludeCondensed-Light.ttf \
    Prelude-Medium.ttf \
    Prelude-Regular.ttf \
"

RDEPENDS:${PN} = "ttf-dejavu-sans-condensed"

do_install() {
    install -d ${D}${datadir}/fonts

    if [ -n "${PDK_WEBOS_FONTS}" ]; then
        for f in ${PDK_FONT_NAMES}; do
            if [ -f "${PDK_WEBOS_FONTS}/$f" ]; then
                install -m 0644 "${PDK_WEBOS_FONTS}/$f" ${D}${datadir}/fonts/
            else
                bbwarn "pdk-sysroot-fonts: $f not in ${PDK_WEBOS_FONTS}, substituting"
                ln -sf ${PDK_FONT_SUBSTITUTE} ${D}${datadir}/fonts/$f
            fi
        done
    else
        # Symlinks, not copies: the point is that the path resolves, and a reader
        # of the image should be able to see immediately that it is a stand-in.
        for f in ${PDK_FONT_NAMES}; do
            ln -sf ${PDK_FONT_SUBSTITUTE} ${D}${datadir}/fonts/$f
        done
    fi
}

FILES:${PN} = "${datadir}/fonts"

# Dangling until ttf-dejavu-sans-condensed is installed alongside; that is an
# RDEPENDS, so it always is.
INSANE_SKIP:${PN} += "dev-so"
