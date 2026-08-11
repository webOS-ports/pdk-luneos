SUMMARY = "Launcher and install tooling for legacy webOS PDK applications"
DESCRIPTION = "pdk-run, which executes a PDK application against the soft-float \
sysroot with the Wayland, audio and GL environment it expects, plus the helper \
used to install legacy IPKs so that sam will launch them."
HOMEPAGE = "https://github.com/webOS-ports/pdk-luneos"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=89aea4e17d99a7cacdbeed46a0096b10"

# Plain git fetch rather than meta-luneos's webos_ports_repo class, so this layer
# parses standalone without meta-webos-ports present.
PDK_GIT_REPO ?= "git://github.com/webOS-ports/pdk-luneos.git"
SRC_URI = "${PDK_GIT_REPO};protocol=https;branch=main"
SRCREV = "3f25d726192ddc04643cdcbf6c2b581b4170f023"
S = "${WORKDIR}/git"

PV = "1.0.0+git"

# Shell scripts only.
inherit allarch

PDK_PREFIX = "/opt/pdk"

do_install() {
    install -d ${D}${PDK_PREFIX}
    install -m 0755 ${S}/tools/pdk-run        ${D}${PDK_PREFIX}/pdk-run
    install -m 0755 ${S}/tools/install-games.sh ${D}${PDK_PREFIX}/install-games.sh

    # On ARMv7 targets the emulator is unnecessary - a hard-float CPU executes
    # soft-float code natively; only the calling convention differs, not the
    # instruction set. pdk-run notices the absence of /opt/pdk/qemu-arm and execs
    # the binary directly.
    install -d ${D}${sysconfdir}/profile.d
    cat > ${D}${sysconfdir}/profile.d/pdk.sh <<EOF
export PATH="\$PATH:${PDK_PREFIX}"
EOF
}

FILES:${PN} = "${PDK_PREFIX} ${sysconfdir}/profile.d/pdk.sh"

RDEPENDS:${PN} = "pdk-sysroot"

# Only where the CPU cannot execute ARM32 itself. qemu-user-arm is a dynamic
# package of oe-core's qemu recipe and contains /usr/bin/qemu-arm.
RDEPENDS:${PN}:append:x86-64 = " qemu-user-arm"
RDEPENDS:${PN}:append:x86 = " qemu-user-arm"
RDEPENDS:${PN}:append:aarch64 = " qemu-user-arm"

RDEPENDS:${PN} += "luna-send"
