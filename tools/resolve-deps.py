#!/usr/bin/env python3
"""Resolve missing shared-library dependencies in the armel sysroot.

Repeatedly runs the guest dynamic loader against a target, and for each
"cannot open shared object file" pulls the providing Debian armel package and
unpacks it. Keeps going until the target's whole closure resolves.

This matters for libraries Mesa *dlopens* rather than links: libEGL_mesa.so.0 and
the DRI drivers are opened at runtime, so a missing dependency there does not
show up as a link error - libglvnd just silently ends up with no EGL vendor and
every eglGetDisplay call returns EGL_BAD_PARAMETER.

Usage:
    tools/resolve-deps.py <target.so> [more targets...]

Env:
    SYSROOT   default ../sysroot relative to this script
    PKGCACHE  default ../pkgcache
"""

import gzip
import os
import re
import subprocess
import sys
import urllib.request

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSROOT = os.environ.get("SYSROOT", os.path.join(TOP, "sysroot"))
PKGCACHE = os.environ.get("PKGCACHE", os.path.join(TOP, "pkgcache"))
MIRROR = "https://deb.debian.org/debian"
SUITE = "bookworm"

QEMU = os.path.join(TOP, "tools", "host", "qemu-arm-static")
LOADER = os.path.join(SYSROOT, "lib", "ld-linux.so.3")


def fetch_index(name, url):
    path = os.path.join(PKGCACHE, name)
    if not os.path.exists(path):
        os.makedirs(PKGCACHE, exist_ok=True)
        print(f"  fetching {name} ...", flush=True)
        urllib.request.urlretrieve(url, path)
    return path


def build_maps():
    contents = fetch_index(
        "Contents-armel.gz", f"{MIRROR}/dists/{SUITE}/main/Contents-armel.gz")
    packages = fetch_index(
        "Packages.gz", f"{MIRROR}/dists/{SUITE}/main/binary-armel/Packages.gz")

    soname2pkg = {}
    for line in gzip.open(contents, "rt", errors="replace"):
        parts = line.rsplit(None, 1)
        if len(parts) != 2:
            continue
        path, pkgs = parts
        base = os.path.basename(path.strip())
        if ".so" not in base:
            continue
        soname2pkg.setdefault(base, pkgs.strip().split(",")[0].split("/")[-1])

    pkg2file, cur = {}, {}
    for line in gzip.open(packages, "rt", errors="replace"):
        line = line.rstrip("\n")
        if line == "":
            p = cur.get("Package")
            if p and p not in pkg2file:
                pkg2file[p] = cur.get("Filename")
            cur = {}
            continue
        m = re.match(r"^(\w[\w.+-]*): (.*)$", line)
        if m:
            cur[m.group(1)] = m.group(2)
    return soname2pkg, pkg2file


def missing_soname(target):
    r = subprocess.run(
        [QEMU, "-L", SYSROOT, "-E", "LD_LIBRARY_PATH=/lib:/usr/lib",
         LOADER, "--list", target],
        capture_output=True, text=True, env=dict(os.environ, LC_ALL="C"))
    m = re.search(r"([\w.+-]+\.so[.\d]*): cannot open shared object",
                  r.stdout + r.stderr)
    return m.group(1) if m else None


def install(pkg, pkg2file):
    fn = pkg2file.get(pkg)
    if not fn:
        return False
    deb = os.path.join(PKGCACHE, "armel", os.path.basename(fn))
    os.makedirs(os.path.dirname(deb), exist_ok=True)
    if not os.path.exists(deb):
        urllib.request.urlretrieve(f"{MIRROR}/{fn}", deb)
    subprocess.run(["dpkg-deb", "-x", deb, SYSROOT], capture_output=True)
    # the 2011 loader knows nothing about multiarch, so flatten
    for d in ("usr/lib", "lib"):
        src = os.path.join(SYSROOT, d, "arm-linux-gnueabi")
        if os.path.isdir(src):
            subprocess.run(f"cp -a {src}/. {os.path.join(SYSROOT, d)}/",
                           shell=True, capture_output=True)
    return True


def main():
    targets = sys.argv[1:]
    if not targets:
        print(__doc__)
        return 1

    print("indexing Debian armel ...", flush=True)
    soname2pkg, pkg2file = build_maps()

    total = 0
    for target in targets:
        if not os.path.exists(target):
            print(f"skip (not found): {target}")
            continue
        print(f"\nresolving {os.path.basename(target)}")
        for _ in range(60):
            so = missing_soname(target)
            if so is None:
                print("  resolved")
                break
            pkg = soname2pkg.get(so)
            if not pkg:
                print(f"  NO PACKAGE PROVIDES {so}")
                break
            if not install(pkg, pkg2file):
                print(f"  NO FILE for package {pkg} (needed for {so})")
                break
            print(f"  +{pkg:26s} (for {so})", flush=True)
            total += 1
    print(f"\n{total} package(s) added")
    return 0


if __name__ == "__main__":
    sys.exit(main())
