# Trim target qemu down to what the PDK layer actually needs.
#
# pdk-tools RDEPENDS on qemu-user-arm on non-ARM32 machines, which otherwise
# drags in a full qemu built for fifteen architectures with SDL, KVM and virgl.
# The only thing PDK applications need is user-mode ARM emulation.
#
# Conditional on the 'pdk' DISTRO_FEATURE, so merely having this layer in
# bblayers.conf does not change anyone else's qemu. Enable with:
#
#     DISTRO_FEATURES:append = " pdk"

QEMU_TARGETS = "${@bb.utils.contains('DISTRO_FEATURES', 'pdk', 'arm', \
    'arm aarch64 i386 loongarch64 mips mipsel mips64 mips64el ppc ppc64 ppc64le riscv32 riscv64 sh4 x86_64', d)}"

# System emulation, its display backends and its accelerators are all dead weight
# for user-mode ARM. Removing rather than reassigning keeps whatever else the
# distro has configured intact.
PACKAGECONFIG:remove = "${@bb.utils.contains('DISTRO_FEATURES', 'pdk', \
    'sdl kvm xen virglrenderer epoxy seccomp fdt', '', d)}"
