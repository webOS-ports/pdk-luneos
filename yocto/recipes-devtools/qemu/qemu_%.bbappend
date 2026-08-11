# Trim target qemu down to what the PDK layer actually needs.
#
# pdk-tools RDEPENDS on qemu-user-arm on non-ARM32 machines, which otherwise
# drags in a full qemu built for fifteen architectures with SDL, KVM and virgl.
# The only thing PDK applications need is user-mode ARM emulation.
#
# Conditional on the 'pdk' DISTRO_FEATURE, so merely having this layer present
# does not change anyone else's qemu. Enable with:
#
#     DISTRO_FEATURES:append = " pdk"
#
# Done in an anonymous python function rather than with inline expansion so that
# when the feature is off, QEMU_TARGETS and PACKAGECONFIG are left entirely alone
# - a bbappend that restates oe-core's default list would silently go stale the
# next time upstream changes it.

python () {
    if not bb.utils.contains('DISTRO_FEATURES', 'pdk', True, False, d):
        return

    d.setVar('QEMU_TARGETS', 'arm')

    # System emulation, its display backends and its accelerators are all dead
    # weight for user-mode ARM.
    drop = {'sdl', 'kvm', 'xen', 'virglrenderer', 'epoxy', 'seccomp', 'fdt'}
    cfg = (d.getVar('PACKAGECONFIG') or '').split()
    d.setVar('PACKAGECONFIG', ' '.join(c for c in cfg if c not in drop))
}
