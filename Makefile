# pdk-luneos - run legacy webOS PDK/SDL apps on LuneOS
#
# Cross-compiles the ARM softfp replacement libraries, plus the x86-64 PIpc host.
# There is no ARM cross-gcc on the dev box, so this uses clang + lld against a
# Debian armel sysroot (see tools/mk-sysroot.sh).
#
#   make sysroot     one-time: build the armel sysroot under $(SYSROOT)
#   make             build everything
#   make install     stage the built libs into the sysroot
#   make clean

TOP     := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
SYSROOT ?= $(TOP)/sysroot
OUT     ?= build

# ARMv7 softfp: same ABI the PDK toolchain emitted in 2010, and the same ABI
# Debian armel uses (EABI, soft-float calling convention).
TRIPLE  := arm-linux-gnueabi
GCCDIR  := $(SYSROOT)/usr/lib/gcc/$(TRIPLE)/12

# lld and patchelf are vendored under tools/host/ because this box has no
# system lld and no passwordless sudo. A system copy takes precedence if present.
# NB: must be the ld.lld symlink, not the lld binary - lld dispatches on argv[0].
LLD     ?= $(if $(wildcard $(TOP)/tools/host/ld.lld),$(TOP)/tools/host/ld.lld,\
             $(shell command -v ld.lld 2>/dev/null || echo ld.lld))
PATCHELF ?= $(if $(wildcard $(TOP)/tools/host/patchelf),$(TOP)/tools/host/patchelf,patchelf)
QEMU_ARM ?= $(if $(wildcard $(TOP)/tools/host/qemu-arm-static),$(TOP)/tools/host/qemu-arm-static,qemu-arm-static)

ARMCC   := clang --target=$(TRIPLE) -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3 \
           --sysroot=$(SYSROOT) -fuse-ld=$(LLD) -B$(GCCDIR) -L$(GCCDIR)
ARMCFLAGS := -O2 -fPIC -Wall -Wextra -Wno-unused-parameter

HOSTCC  ?= clang
HOSTCFLAGS := -O2 -Wall -static

ARM_TARGETS  := $(OUT)/libpdl.so $(OUT)/libSDL-1.2.so.0 $(OUT)/egltest $(OUT)/crashcatch.so $(OUT)/libSDL_cinema.so $(OUT)/libGLES_CM.so
HOST_TARGETS := $(OUT)/pdkhost

all: $(ARM_TARGETS) $(HOST_TARGETS)

$(OUT):
	@mkdir -p $(OUT)

# --- the PDL rewrite -------------------------------------------------------
# Mirror the original libpdl's NEEDED set. Other Palm libraries were linked
# assuming libpdl would drag in libcurl/libssl/libsqlite3, and relied on those
# symbols being available transitively - drop them and 7+ titles die at load with
# "undefined symbol: curl_easy_init". --no-as-needed keeps them recorded even
# though this libpdl does not call into them itself.
$(OUT)/libpdl.so: src/libpdl.c | $(OUT)
	$(ARMCC) $(ARMCFLAGS) -shared -Wl,-soname,libpdl.so -o $@ $< -ldl \
	    -L$(SYSROOT)/usr/lib -Wl,--no-as-needed \
	    -l:libcurl.so.4 -l:libssl.so.0.9.8 -l:libcrypto.so.0.9.8 -l:libsqlite3.so.0

# --- SDL 1.2 shim over sdl12-compat ---------------------------------------
# The stock sdl12-compat has soname libSDL-1.2.so.0, which we want to own, so it
# is renamed to libSDL12compat.so.0 (done by mk-sysroot.sh) and chained to here.
$(OUT)/libSDL-1.2.so.0: src/sdl_webos_shim.c | $(OUT)
	$(ARMCC) $(ARMCFLAGS) -shared -Wl,-soname,libSDL-1.2.so.0 -o $@ $< \
	    -L$(SYSROOT)/usr/lib -l:libSDL12compat.so.0 -ldl

# --- minimal EGL/GLES2 probe (no SDL involved) ----------------------------
$(OUT)/egltest: src/egltest.c | $(OUT)
	$(ARMCC) -O1 -Wall -o $@ $< -L$(SYSROOT)/usr/lib -l:libEGL.so.1 -l:libGLESv2.so.2

# --- GLES1 extension entry points Mesa does not export --------------------
$(OUT)/libGLES_CM.so: src/gles_oes_shim.c | $(OUT)
	$(ARMCC) $(ARMCFLAGS) -shared -Wl,-soname,libGLES_CM.so -o $@ $< \
	    -L$(SYSROOT)/usr/lib -l:libGLESv1_CM.so.1 -l:libEGL.so.1 -ldl

# --- stub for Palm's media-service video library ---------------------------
$(OUT)/libSDL_cinema.so: src/sdl_cinema_stub.c | $(OUT)
	$(ARMCC) $(ARMCFLAGS) -shared -Wl,-soname,libSDL_cinema.so -o $@ $<

# --- LD_PRELOAD fault reporter (debugging aid) -----------------------------
$(OUT)/crashcatch.so: src/crashcatch.c | $(OUT)
	$(ARMCC) $(ARMCFLAGS) -funwind-tables -shared -o $@ $< -ldl

# --- PIpc host: runs natively on the target, not under emulation ----------
$(OUT)/pdkhost: src/pdkhost.c src/msgnames.h | $(OUT)
	$(HOSTCC) $(HOSTCFLAGS) -Isrc -o $@ $<

sysroot:
	PATH="$(TOP)/tools/host:$$PATH" tools/mk-sysroot.sh $(SYSROOT)

# Run the bundled PDK sample against the built stack (x86 dev box, via qemu-arm).
run: install
	cd testapp && $(QEMU_ARM) -L $(SYSROOT) -E LD_LIBRARY_PATH=/lib:/usr/lib \
	    -E SDL_VIDEODRIVER=$(or $(VIDEO),dummy) -E SDL_AUDIODRIVER=dummy \
	    -E PDL_DEBUG=1 -E PDK_SHIM_DEBUG=1 ./simple

# Reproduce the outstanding EGL failure in isolation.
egl: $(OUT)/egltest
	$(QEMU_ARM) -L $(SYSROOT) -E LD_LIBRARY_PATH=/lib:/usr/lib \
	    -E LIBGL_ALWAYS_SOFTWARE=1 -E GALLIUM_DRIVER=llvmpipe $(OUT)/egltest

install: $(ARM_TARGETS)
	install -d $(SYSROOT)/usr/lib
	install -m 0644 $(OUT)/libpdl.so $(SYSROOT)/usr/lib/
	install -m 0644 $(OUT)/libSDL-1.2.so.0 $(SYSROOT)/usr/lib/
	install -m 0644 $(OUT)/libSDL_cinema.so $(SYSROOT)/usr/lib/
	install -m 0644 $(OUT)/libGLES_CM.so $(SYSROOT)/usr/lib/
	ln -sf libSDL-1.2.so.0 $(SYSROOT)/usr/lib/libSDL.so
	@echo "staged into $(SYSROOT)/usr/lib"

clean:
	rm -rf $(OUT)

.PHONY: all sysroot install clean run egl
