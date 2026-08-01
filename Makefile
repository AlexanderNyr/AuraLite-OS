# =============================================================================
# AuraLite OS — Top-level Makefile
# Toolchain: Clang (--target=x86_64-elf) + LLD + NASM, booted by Limine.
# =============================================================================

ARCH        := x86_64
TARGET      := $(ARCH)-elf
CC          := clang
LD          := ld.lld
AS          := nasm
HOST_CC     := cc

BUILD_DIR   := build
LIMINE_DIR  := limine
LIMINE_SRC  := third_party/limine
# Prefer the checked-in Limine binary bundle.  This keeps a normal clone buildable
# even when the Git submodule was not initialised and avoids rebuilding the whole
# Limine tree in CI.  If the bundle is removed, Make falls back to the submodule.
LIMINE_ARCHIVE := limine-binary.tar.gz
ifneq ($(wildcard $(LIMINE_ARCHIVE)),)
LIMINE_BIN  := $(BUILD_DIR)/limine-binary
LIMINE_MODE := bundled
else
LIMINE_BIN  := $(LIMINE_SRC)/bin
LIMINE_MODE := submodule
endif
LIMINE_DEPS := $(LIMINE_BIN)/limine $(LIMINE_BIN)/limine-bios.sys \
               $(LIMINE_BIN)/limine-uefi-cd.bin $(LIMINE_BIN)/limine-bios-cd.bin \
               $(LIMINE_BIN)/BOOTX64.EFI
# mformat/mcopy are needed by mkisoimage_bios.sh / mkisoimage_dual.sh
# to build the FAT32 partition; lld-link is needed by `make efi` to
# link BOOTX64.EFI as PE32+.  xorriso is retained even though the
# custom loaders no longer use it, because `make iso-limine` still
# wraps its output with xorriso -- keep it in REQUIRED_TOOLS so that
# users trying the fallback path get a fast error instead of a
# cryptic xorriso "command not found".
REQUIRED_TOOLS := $(CC) $(LD) $(AS) $(HOST_CC) python3 tar xorriso \
                  mformat mcopy lld-link
ifeq ($(LIMINE_MODE),submodule)
REQUIRED_TOOLS += git autoreconf
endif
KERNEL_ELF  := $(BUILD_DIR)/kernel.elf
ISO_IMAGE   := $(BUILD_DIR)/auralite.iso

# -mcmodel=kernel: code lives in the top 2 GiB (negative addresses); required so
#   clang emits relocations valid for the higher-half link address.
# -mno-red-zone: the 128-byte red zone is unsafe under interrupts.
# -mno-sse/mmx: we never initialise the FPU/SSE unit in the kernel.
CFLAGS      := --target=$(TARGET) \
               -std=c11 -ffreestanding -fstack-protector-strong \
               -fno-pie -fno-pic -mcmodel=kernel -mno-red-zone \
               -mno-mmx -mno-sse -mno-sse2 \
               -fno-omit-frame-pointer \
               -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
               -O2 -g \
               -DARCH_X86_64 -I . -I $(BUILD_DIR)

ASFLAGS     := -f elf64 -I $(BUILD_DIR)/

# The linker script fixes the higher-half address; no --image-base needed.
LDFLAGS     := -nostdlib -static -T kernel.ld -z max-page-size=4096

KERNEL_SRCS := $(shell find kernel drivers -name '*.c')
KERNEL_ASMS := $(shell find kernel drivers -name '*.asm')
# NOTE: a .c and .asm file MUST NOT share a base name (e.g. foo.c + foo.asm),
# because both compile to the same object path build/.../foo.o, which would
# collide and double-link. Keep assembly stubs named distinctly (e.g.
# foo_stubs.asm). ISR stubs live in isr_stubs.asm for this reason.
KERNEL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS)) \
               $(patsubst %.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASMS))

.PHONY: all kernel user iso usb vbox vmware vm-configs run run-usb-msc clean \
        deps-check test-unit test-integration test-integration-fast test limine-build

all: iso

# Fail early with actionable messages instead of a later "command not found".
deps-check:
	@missing=0; \
	for tool in $(REQUIRED_TOOLS); do \
		if ! command -v $$tool >/dev/null 2>&1; then \
			echo "[deps] missing required tool: $$tool"; \
			missing=1; \
		fi; \
	done; \
	if [ $$missing -ne 0 ]; then \
		echo "[deps] Debian/Ubuntu: sudo apt install clang lld nasm xorriso qemu-system-x86 mtools ovmf make gcc python3"; \
		exit 127; \
	fi

kernel: $(KERNEL_ELF)

# =============================================================================
# BL2: BIOS Stage 1 (MBR) -- flat 512-byte binary.
# =============================================================================
BOOT_BIOS_DIR   := boot/bios
MBR_BIN         := $(BUILD_DIR)/boot/mbr.bin
MBR_DUAL_BIN    := $(BUILD_DIR)/boot/mbr_dual.bin
STAGE2_BIN      := $(BUILD_DIR)/boot/stage2.bin

.PHONY: mbr mbr-dual stage2 boot-offsets
mbr:      $(MBR_BIN)
mbr-dual: $(MBR_DUAL_BIN)
stage2:   $(STAGE2_BIN)

$(MBR_BIN): $(BOOT_BIOS_DIR)/stage1/mbr.asm
	@mkdir -p $(dir $@)
	$(AS) -f bin -o $@ $<
	@sz=$$(wc -c < $@); \
	  if [ "$$sz" -ne 512 ]; then \
	    echo "[mbr]  ERROR: $@ is $$sz bytes, expected 512"; exit 1; \
	  fi
	@sig=$$(od -An -tx1 -N2 -j510 $@ | tr -d ' \n'); \
	  if [ "$$sig" != "55aa" ]; then \
	    echo "[mbr]  ERROR: bad boot signature $$sig (expected 55aa)"; exit 1; \
	  fi
	@printf "  [mbr] %-40s %d bytes, sig=0x55AA\n" $@ 512

$(MBR_DUAL_BIN): $(BOOT_BIOS_DIR)/stage1/mbr_dual.asm
	@mkdir -p $(dir $@)
	$(AS) -f bin -o $@ $<
	@sz=$$(wc -c < $@); \
	  [ "$$sz" -eq 512 ] || { echo "[mbr-dual] ERROR: $@ is $$sz bytes"; exit 1; }
	@sig=$$(od -An -tx1 -N2 -j510 $@ | tr -d ' \n'); \
	  [ "$$sig" = "55aa" ] || { echo "[mbr-dual] ERROR: bad sig $$sig"; exit 1; }
	@printf "  [mbr-dual] %-35s %d bytes, sig=0x55AA\n" $@ 512

# ---- BL3: BIOS Stage 2 -----------------------------------------------------
# Stage 2 is a single flat binary assembled from stage2_start.asm which
# %includes every submodule (uart16.inc, e820.inc, a20.inc, ...).  There is
# no linker involved: nasm -f bin outputs raw code ready to load at ORG.
# Max size: 63 KiB (126 sectors) -- enforced below.
STAGE2_SRC      := $(BOOT_BIOS_DIR)/stage2/stage2_start.asm
STAGE2_INCS     := $(wildcard $(BOOT_BIOS_DIR)/stage2/*.inc)
BOOT_OFFSETS_GEN := $(BUILD_DIR)/gen_boot_offsets
BOOT_OFFSETS_INC := $(BUILD_DIR)/boot_offsets.inc
BOOT_OFFSETS_H   := $(BUILD_DIR)/boot_offsets.h

boot-offsets: $(BOOT_OFFSETS_INC) $(BOOT_OFFSETS_H)

$(BOOT_OFFSETS_GEN): tools/gen_boot_offsets.c boot/shared/boot_info.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -I . $< -o $@

$(BOOT_OFFSETS_INC): $(BOOT_OFFSETS_GEN)
	@mkdir -p $(dir $@)
	$< --asm > $@
	@echo "  [offsets] $@"

$(BOOT_OFFSETS_H): $(BOOT_OFFSETS_GEN)
	@mkdir -p $(dir $@)
	$< --c > $@
	@echo "  [offsets] $@"

$(STAGE2_BIN): $(STAGE2_SRC) $(STAGE2_INCS) $(BOOT_OFFSETS_INC)
	@mkdir -p $(dir $@)
	$(AS) -f bin -I . -I $(BUILD_DIR)/ -o $@ $<
	@sz=$$(wc -c < $@); \
	  maxsz=$$((126*512)); \
	  if [ "$$sz" -gt "$$maxsz" ]; then \
	    echo "[stage2] FATAL: $@ is $$sz bytes; MBR loads max $$maxsz"; \
	    rm -f $@; \
	    exit 1; \
	  fi; \
	  printf "  [stage2] %-38s %d / %d bytes\n" $@ $$sz $$maxsz

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Special rule for render3d.c: needs SSE for float math.
$(BUILD_DIR)/drivers/framebuffer/render3d.o: drivers/framebuffer/render3d.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -msse -msse2 -mfpmath=sse -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Auto-generated nasm struct-offset include (keeps hand-written asm in sync
# with tcb_t).  Built by a tiny host program using offsetof().
$(BUILD_DIR)/gen_asm_offsets: tools/gen_asm_offsets.c kernel/proc/thread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -I . $< -o $@

$(BUILD_DIR)/asm_offsets.inc: $(BUILD_DIR)/gen_asm_offsets
	@mkdir -p $(dir $@)
	$< > $@

# context.asm %includes asm_offsets.inc for TCB field offsets;
# syscall_entry.asm %includes it for the per-CPU struct cpu_local slots.
$(BUILD_DIR)/kernel/proc/context.o: $(BUILD_DIR)/asm_offsets.inc
$(BUILD_DIR)/kernel/arch/x86_64/syscall_entry.o: $(BUILD_DIR)/asm_offsets.inc

# ---- SMP AP trampoline (raw 16-bit blob embedded into the kernel) ----
# boot/smp/ap_trampoline.asm assembles to a flat position-fixed binary
# (org 0x8000; same pattern as stage2: nasm -f bin, no linker).  smp.c
# embeds the bytes via the generated build/ap_trampoline.inc header and
# copies them to SMP_TRAMPOLINE_CODE_PHYS before sending SIPIs.
AP_TRAMPOLINE_SRC := boot/smp/ap_trampoline.asm
AP_TRAMPOLINE_BIN := $(BUILD_DIR)/boot/ap_trampoline.bin
AP_TRAMPOLINE_INC := $(BUILD_DIR)/ap_trampoline.inc

$(AP_TRAMPOLINE_BIN): $(AP_TRAMPOLINE_SRC)
	@mkdir -p $(dir $@)
	$(AS) -f bin -o $@ $<

$(AP_TRAMPOLINE_INC): $(AP_TRAMPOLINE_BIN) tools/gen_ap_trampoline_inc.py
	@mkdir -p $(dir $@)
	python3 tools/gen_ap_trampoline_inc.py $< > $@
	@echo "  [ap-trampoline] $@"

# smp.c #includes build/ap_trampoline.inc directly.
$(BUILD_DIR)/kernel/arch/x86_64/smp.o: $(AP_TRAMPOLINE_INC)

$(KERNEL_ELF): $(KERNEL_OBJS) kernel.ld $(USER_BIN_H)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@
	@echo "[link] $(KERNEL_ELF)"

# ---- User-space program build (compiled with host cc, linked with LLD) ----
# Two ELF binaries:
#   init.elf  — the interactive shell, embedded in the kernel AND in the initrd
#   hello.elf — a simple test program, in the initrd only
USER_BUILD   := $(BUILD_DIR)/user
INIT_ELF     := $(USER_BUILD)/init.elf
HELLO_ELF    := $(USER_BUILD)/hello.elf
USER_BIN_H   := $(BUILD_DIR)/init_bin.h

USER_CFLAGS  := -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
                -O2 -Wall -Wextra -Werror -I . -I libc/include
USER_LDFLAGS := -nostdlib -static -T libc/user.ld -z max-page-size=4096

# Common objects shared by all user programs.
# Extra libc translation units (each its own object, all linked into every
# user program via USER_COMMON).  These provide the P9/P10 surface: pthreads,
# dirent, regex, env, getopt, pwd, utsname, getrlimit, and the math/stdio/
# stdlib/string extensions.
LIBC_EXTRA_OBJS := $(USER_BUILD)/pthread.o $(USER_BUILD)/rwlock.o $(USER_BUILD)/barrier.o $(USER_BUILD)/spin.o $(USER_BUILD)/dirent.o \
                   $(USER_BUILD)/regex.o $(USER_BUILD)/env.o \
                   $(USER_BUILD)/getopt.o $(USER_BUILD)/pwd.o \
                   $(USER_BUILD)/utsname.o $(USER_BUILD)/resource.o \
                   $(USER_BUILD)/math_extra.o $(USER_BUILD)/stdio_extra.o \
                   $(USER_BUILD)/stdlib_extra.o $(USER_BUILD)/string_extra.o \
                   $(USER_BUILD)/posix_extra.o $(USER_BUILD)/posix_spawn.o $(USER_BUILD)/q10_stubs.o

USER_COMMON := $(USER_BUILD)/crt0.o $(USER_BUILD)/syscall.o $(USER_BUILD)/libc.o \
               $(USER_BUILD)/malloc.o $(USER_BUILD)/sigreturn.o $(USER_BUILD)/setjmp.o \
               $(USER_BUILD)/compat.o $(LIBC_EXTRA_OBJS)

USER_CFLAGS_INC := libc/include/unistd.h libc/include/string.h libc/include/stdio.h libc/include/stdlib.h \
                   libc/include/errno.h libc/include/limits.h libc/include/stdbool.h \
                   libc/include/ctype.h libc/include/math.h libc/include/assert.h
# Augment include path so user apps can include "auragui.h".
USER_CFLAGS += -I libauragui/include

# Application ELFs.
USER_APPS := $(USER_BUILD)/calc.elf $(USER_BUILD)/sysinfo.elf \
             $(USER_BUILD)/editor.elf $(USER_BUILD)/http.elf \
             $(USER_BUILD)/clock.elf $(USER_BUILD)/guess.elf \
             $(USER_BUILD)/snake.elf $(USER_BUILD)/browser.elf \
             $(USER_BUILD)/selftest.elf \
             $(USER_BUILD)/proctest.elf $(USER_BUILD)/fdtest.elf \
             $(USER_BUILD)/p10test.elf $(USER_BUILD)/argv_echo.elf \
             $(USER_BUILD)/execve_child.elf \
             $(USER_BUILD)/gcalc.elf $(USER_BUILD)/gedit.elf \
             $(USER_BUILD)/gfiles.elf $(USER_BUILD)/gterm.elf \
             $(USER_BUILD)/gsysmon.elf $(USER_BUILD)/gabout.elf \
             $(USER_BUILD)/gtaskmgr.elf $(USER_BUILD)/gtheme.elf \
             $(USER_BUILD)/glaunch.elf \
             $(USER_BUILD)/apm.elf $(USER_BUILD)/matrix.elf \
             $(USER_BUILD)/life.elf $(USER_BUILD)/fetch.elf \
             $(USER_BUILD)/play.elf $(USER_BUILD)/gaudio.elf \
             $(USER_BUILD)/gbrowser.elf $(USER_BUILD)/gusb.elf \
             $(USER_BUILD)/tcpserver.elf $(USER_BUILD)/elfperm.elf \
             $(USER_BUILD)/udptest.elf $(USER_BUILD)/timestest.elf \
             $(USER_BUILD)/fifolinktest.elf $(USER_BUILD)/stackguard.elf

# auragui object linked into every GUI app.
USER_GUI_OBJ := $(USER_BUILD)/auragui.o

# ---- OpenGL (libgl) -- see GL_PLAN.md ----
# libgl is linked ONLY into GL applications, not into every user program, so
# the other 38 binaries in the initrd do not grow.  Add new libgl translation
# units here as the phases land.
LIBGL_OBJS := $(USER_BUILD)/glmath.o $(USER_BUILD)/auraglx.o \
              $(USER_BUILD)/glstate.o $(USER_BUILD)/glmatrix.o \
              $(USER_BUILD)/glimm.o $(USER_BUILD)/glraster.o \
              $(USER_BUILD)/glclip.o $(USER_BUILD)/gllight.o \
              $(USER_BUILD)/gltexture.o $(USER_BUILD)/glfrag.o \
              $(USER_BUILD)/glarray.o $(USER_BUILD)/gllist.o \
              $(USER_BUILD)/glu.o $(USER_BUILD)/glbackend.o \
              $(USER_BUILD)/glvirgl.o
USER_GL_OBJ := $(LIBGL_OBJS)
USER_CFLAGS += -I libgl/include

# GL applications: linked with libgl in addition to libauragui.
USER_GL_APPS := $(USER_BUILD)/gltest.elf $(USER_BUILD)/glcube.elf \
                $(USER_BUILD)/glgears.elf

user: $(INIT_ELF) $(HELLO_ELF) $(USER_APPS) $(USER_GL_APPS)

# Pattern rule for linking user ELFs (each links with crt0 + syscall + libc).
# GUI apps additionally link the libauragui object.
$(USER_BUILD)/%.elf: $(USER_BUILD)/%.o $(USER_COMMON) $(USER_GUI_OBJ) libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/$*.o $(USER_COMMON) $(USER_GUI_OBJ) -o $@
	@echo "[link] $@"

# Compile rules for each application.
$(USER_BUILD)/calc.o: userspace/calc/calc.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/sysinfo.o: userspace/sysinfo/sysinfo.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/editor.o: userspace/editor/editor.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/http.o: userspace/http/http.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/tcpserver.o: userspace/tcpserver/tcpserver.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/elfperm.o: userspace/elfperm/elfperm.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/udptest.o: userspace/udptest/udptest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/timestest.o: userspace/timestest/timestest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/fifolinktest.o: userspace/fifolinktest/fifolinktest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/stackguard.o: userspace/stackguard/stackguard.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/clock.o: userspace/clock/clock.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/guess.o: userspace/guess/guess.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/snake.o: userspace/snake/snake.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/browser.o: userspace/browser/browser.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/selftest.o: userspace/selftest/selftest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/proctest.o: userspace/proctest/proctest.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/fdtest.o: userspace/fdtest/fdtest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/p10test.o: userspace/p10test/p10test.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/argv_echo.o: userspace/argv_echo/argv_echo.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/execve_child.o: userspace/execve_child/execve_child.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/apm.o: userspace/apm/apm.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/matrix.o: userspace/matrix/matrix.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/life.o: userspace/life/life.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/fetch.o: userspace/fetch/fetch.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/play.o: userspace/play/play.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- GUI applications and libauragui ----
$(USER_BUILD)/auragui.o: libauragui/src/auragui.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- libgl (OpenGL) translation units -- see GL_PLAN.md ----
$(USER_BUILD)/glmath.o: libgl/src/glmath.c libgl/include/GL/glmath.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/auraglx.o: libgl/src/auraglx.c libgl/include/GL/auraglx.h \
                         libgl/src/glcontext.h libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glstate.o: libgl/src/glstate.c libgl/src/glcontext.h \
                         libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glmatrix.o: libgl/src/glmatrix.c libgl/src/glcontext.h \
                          libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glimm.o: libgl/src/glimm.c libgl/src/glcontext.h \
                       libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glraster.o: libgl/src/glraster.c libgl/src/glcontext.h \
                          libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glclip.o: libgl/src/glclip.c libgl/src/glcontext.h \
                        libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/gllight.o: libgl/src/gllight.c libgl/src/glcontext.h \
                         libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/gltexture.o: libgl/src/gltexture.c libgl/src/glcontext.h \
                           libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glfrag.o: libgl/src/glfrag.c libgl/src/glcontext.h \
                        libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glarray.o: libgl/src/glarray.c libgl/src/glcontext.h \
                         libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/gllist.o: libgl/src/gllist.c libgl/src/glcontext.h \
                        libgl/src/glvertex.h libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# glu.c is built purely on the public GL API: no glcontext.h dependency.
$(USER_BUILD)/glu.o: libgl/src/glu.c libgl/include/GL/glu.h \
                     libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glbackend.o: libgl/src/glbackend.c libgl/include/GL/glbackend.h \
                           libgl/src/glcontext.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glvirgl.o: libgl/src/glvirgl.c libgl/include/GL/glbackend.h \
                         libgl/src/glcontext.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- GL applications ----
$(USER_BUILD)/gltest.o: userspace/gltest/gltest.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glcube.o: userspace/glcube/glcube.c libauragui/include/auragui.h \
                        libgl/include/GL/gl.h libgl/include/GL/auraglx.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glgears.o: userspace/glgears/glgears.c libauragui/include/auragui.h \
                         libgl/include/GL/gl.h libgl/include/GL/glu.h \
                         libgl/include/GL/auraglx.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# Explicit link rule: GL apps additionally pull in libgl.  This overrides the
# generic %.elf pattern rule below for these targets.
$(USER_GL_APPS): $(USER_BUILD)/%.elf: $(USER_BUILD)/%.o $(USER_COMMON) \
                                      $(USER_GUI_OBJ) $(USER_GL_OBJ) libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/$*.o $(USER_COMMON) $(USER_GUI_OBJ) \
	      $(USER_GL_OBJ) -o $@
	@echo "[link] $@ (libgl)"

$(USER_BUILD)/gcalc.o:   userspace/gui-calc/gcalc.c     libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gedit.o:   userspace/gui-edit/gedit.c     libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gfiles.o:  userspace/gui-files/gfiles.c   libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gterm.o:   userspace/gui-term/gterm.c     libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gsysmon.o: userspace/gui-sysmon/gsysmon.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gabout.o:  userspace/gui-about/gabout.c   libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gtaskmgr.o: userspace/gui-taskmgr/gtaskmgr.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/glaunch.o: userspace/gui-launcher/glaunch.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gaudio.o: userspace/gui-audio/gaudio.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gbrowser.o: userspace/gui-browser/gbrowser.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gusb.o: userspace/gui-usb/gusb.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gtheme.o: userspace/gui-theme/theme.c libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/hello.o: userspace/hello/hello.c libc/include/unistd.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/init.o: userspace/init/init.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/libc.o: libc/src/libc.c libc/include/unistd.h libc/include/string.h \
                       libc/include/stdio.h libc/include/stdlib.h libc/include/errno.h \
                       libc/include/ctype.h libc/include/math.h libc/include/limits.h \
                       libc/include/stdbool.h libc/include/assert.h libc/include/signal.h \
                       libc/include/sys/uio.h libc/include/fcntl.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/malloc.o: libc/src/malloc.c libc/include/stdlib.h libc/include/unistd.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# Generic rule for the extra libc translation units in libc/src/*.c.
$(USER_BUILD)/%.o: libc/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# The pthread runtime lives in a sub-directory.
$(USER_BUILD)/pthread.o: libc/src/pthread/pthread.c libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/rwlock.o: libc/src/pthread/rwlock.c libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/barrier.o: libc/src/pthread/barrier.c libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/spin.o: libc/src/pthread/spin.c libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/crt0.o: libc/crt/crt0.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/syscall.o: libc/src/syscall.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/sigreturn.o: libc/crt/sigreturn.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/setjmp.o: libc/crt/setjmp.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/compat.o: libc/src/compat.c libc/include/strings.h libc/include/wctype.h \
                         libc/include/inttypes.h libc/include/setjmp.h libc/include/threads.h \
                         libc/include/uchar.h libc/include/fenv.h libc/include/complex.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(INIT_ELF): $(USER_BUILD)/init.o $(USER_COMMON) libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/init.o $(USER_COMMON) -o $@
	@echo "[link] $(INIT_ELF)"

$(HELLO_ELF): $(USER_BUILD)/hello.o $(USER_COMMON) libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/hello.o $(USER_COMMON) -o $@
	@echo "[link] $(HELLO_ELF)"

# Embed init.elf into the kernel as a C array.
$(USER_BIN_H): $(INIT_ELF) tools/gen_user_binary.py
	@mkdir -p $(dir $@)
	python3 tools/gen_user_binary.py $(INIT_ELF) $@ init_bin

# user.c includes the generated init_bin.h; ensure it exists first.
$(BUILD_DIR)/kernel/proc/user.o: $(USER_BIN_H)

USB_IMAGE   := $(BUILD_DIR)/usb.img

ifeq ($(LIMINE_MODE),bundled)
$(LIMINE_BIN)/limine: $(LIMINE_ARCHIVE)
	@echo "[limine] using bundled Limine binary package ($<)"
	@rm -rf $(LIMINE_BIN)
	@mkdir -p $(BUILD_DIR)
	@tar -xzf $< -C $(BUILD_DIR)
	@$(MAKE) -C $(LIMINE_BIN) limine

$(filter-out $(LIMINE_BIN)/limine,$(LIMINE_DEPS)): $(LIMINE_BIN)/limine
	@:
else
$(LIMINE_BIN)/limine:
	@if [ ! -d "$(LIMINE_SRC)/.git" ]; then \
		echo "[limine] missing submodule $(LIMINE_SRC)"; \
		echo "[limine] run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	@( cd $(LIMINE_SRC) && ([ -f configure ] || ./bootstrap) && \
	   ./configure --enable-all && make -j$$(nproc) )


$(filter-out $(LIMINE_BIN)/limine,$(LIMINE_DEPS)): $(LIMINE_BIN)/limine
	@:
endif

limine-build: $(LIMINE_DEPS)

# ---- BL6: UEFI bootloader (BOOTX64.EFI) -----------------------------------
# Freestanding C compiled for the Windows x64 ABI (RCX/RDX/... calling
# convention that UEFI uses), linked as PE32+ via ld.lld's pei output.
UEFI_DIR     := boot/uefi
EFI_BIN      := $(BUILD_DIR)/boot/BOOTX64.EFI
UEFI_CC      := $(CC) --target=x86_64-unknown-windows
UEFI_CFLAGS  := -ffreestanding -fno-stack-protector -fshort-wchar \
                -mno-red-zone -Wall -Wextra -O2 -I .
UEFI_SRCS    := $(UEFI_DIR)/efi_main.c \
                $(UEFI_DIR)/efi_paging.c \
                $(UEFI_DIR)/efi_elf.c \
                $(UEFI_DIR)/efi_acpi.c
UEFI_OBJS    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UEFI_SRCS))
UEFI_LD      := $(UEFI_DIR)/bootloader.ld

.PHONY: efi
efi: $(EFI_BIN)

$(BUILD_DIR)/$(UEFI_DIR)/%.o: $(UEFI_DIR)/%.c
	@mkdir -p $(dir $@)
	$(UEFI_CC) $(UEFI_CFLAGS) -c $< -o $@

$(EFI_BIN): $(UEFI_OBJS)
	@mkdir -p $(dir $@)
	lld-link -subsystem:efi_application \
	         -entry:efi_main \
	         -nodefaultlib \
	         -dll \
	         -out:$@ \
	         $(UEFI_OBJS)
	@printf "  [efi] %-40s %d bytes\n" $@ $$(wc -c < $@)

# ---- BL5: BIOS-only ISO built with our custom bootloader (no Limine).
# Produces a hybrid MBR image that boots on QEMU (`-drive if=ide`) and
# on real hardware via USB stick (`dd if=... of=/dev/sdX`).  Legacy
# CD-ROM (`-cdrom`) boot is out of scope -- BL7 adds a dual-boot ISO
# that supports both.
BIOS_ISO_IMAGE := $(BUILD_DIR)/auralite-bios.iso

.PHONY: iso-bios
iso-bios: deps-check kernel $(MBR_BIN) $(STAGE2_BIN)
	@bash tools/mkisoimage_bios.sh $(KERNEL_ELF) $(BIOS_ISO_IMAGE)

# ---- BL7: dual-boot ISO combining BIOS + UEFI on one file -----------------
# The resulting image boots on legacy BIOS (SeaBIOS reads the hybrid
# MBR and runs Stage 2) AND on UEFI (OVMF walks the GPT to find the
# ESP and runs BOOTX64.EFI).  Both paths share the same KERNEL.ELF
# stored in the FAT32 partition at LBA 256.
DUAL_ISO_IMAGE := $(BUILD_DIR)/auralite-dual.iso

# Size of the FAT32 ESP inside the hybrid image, in MiB.  The final image is
# ESP_MB + 1 MiB (the extra MiB holds the MBR, Stage 2 and the GPT areas).
# Default 48 MiB is the smallest size that still yields >65525 FAT32 clusters
# (below that OVMF rejects the volume as FAT16) while fitting kernel + initrd,
# each of which is stored twice for the BIOS and UEFI lookup paths.
# Override for a roomier image:  make iso ESP_MB=256
ESP_MB ?= 48
export ESP_MB

.PHONY: iso-dual
iso-dual: deps-check kernel $(BUILD_DIR)/initrd.tar $(MBR_DUAL_BIN) $(STAGE2_BIN) $(EFI_BIN)
	@bash tools/mkisoimage_dual.sh $(KERNEL_ELF) $(EFI_BIN) $(DUAL_ISO_IMAGE)

# ---- BL8: `make iso` now defaults to the custom dual-boot loader ----------
# Legacy `make iso-limine` is preserved below as a fallback for anyone
# who still needs the old Limine-based image.  The Limine path is
# fully optional: if the limine-binary.tar.gz bundle and the
# `third_party/limine` submodule are both absent, `make iso` still
# works because it no longer depends on `limine-build`.
.PHONY: iso iso-limine
iso: iso-dual
	@# Keep the historical build/auralite.iso path as the canonical local
	@# artefact.  The integration tests and run/debug targets consume it.
	@cp $(DUAL_ISO_IMAGE) $(ISO_IMAGE)
	@mkdir -p release
	@cp $(ISO_IMAGE) release/auralite.iso
	@cp $(BUILD_DIR)/kernel.elf release/kernel.elf
	@[ -f $(BUILD_DIR)/initrd.tar ] && cp $(BUILD_DIR)/initrd.tar release/initrd.tar || true
	@cd release && sha256sum auralite.iso kernel.elf $$( [ -f initrd.tar ] && echo initrd.tar) \
	    > SHA256SUMS
	@cp release/SHA256SUMS SHA256SUMS
	@echo "[release] Wrote ISO, kernel.elf$$([ -f release/initrd.tar ] && echo ', initrd.tar'), and SHA256SUMS to 'release/' folder"

# Preserved for backwards compatibility.  Requires either the
# limine-binary.tar.gz bundle or the third_party/limine submodule.
# Because BL1 removed every limine_get_* accessor from the kernel and
# BL1 also dropped the .limine_requests* sections from kernel.ld, the
# ISO built here contains Limine only as a chain-loader that never
# reaches the kernel with its own memory-map / framebuffer info -- it
# will boot to the kernel banner via boot_info fallbacks but the
# custom BIOS/UEFI paths (make iso-dual) are the supported default.
iso-limine: deps-check kernel $(BUILD_DIR)/initrd.tar limine-build
	@bash tools/mkisoimage_limine.sh $(KERNEL_ELF) $(ISO_IMAGE) $(LIMINE_BIN)
	@mkdir -p release
	@cp $(ISO_IMAGE) release/auralite-limine.iso
	@echo "[iso-limine] wrote release/auralite-limine.iso"

usb: iso
	@cp $(ISO_IMAGE) $(USB_IMAGE)
	@echo "[usb] wrote $(USB_IMAGE) ($(shell du -h $(USB_IMAGE) | cut -f1))"
	@echo "[usb] Boot: qemu-system-x86_64 -drive file=$(USB_IMAGE),format=raw -m 512M -serial stdio"
	@echo "[usb] Or write to USB: sudo dd if=$(USB_IMAGE) of=/dev/sdX bs=4M"

# Generate host-VM launcher/configuration files for desktop hypervisors.
# VirtualBox uses Intel PRO/1000 MT Desktop (82540EM); VMware uses e1000
# (usually 82545EM). Both IDs are accepted by the kernel e1000 driver.
vbox: iso
	@bash tools/mkvbox.sh $(ISO_IMAGE)

vmware: iso
	@bash tools/mkvmware.sh $(ISO_IMAGE)

vm-configs: vbox vmware

# Build the initrd (USTAR tarball of userspace binaries).
INITRD_DIR := $(USER_BUILD)/initrd_root
$(BUILD_DIR)/initrd.tar: $(INIT_ELF) $(HELLO_ELF) $(USER_APPS) $(USER_GL_APPS)
	@mkdir -p $(INITRD_DIR)
	@cp $(INIT_ELF) $(INITRD_DIR)/init
	@cp $(HELLO_ELF) $(INITRD_DIR)/hello
	@cp $(USER_BUILD)/calc.elf $(INITRD_DIR)/calc
	@cp $(USER_BUILD)/sysinfo.elf $(INITRD_DIR)/sysinfo
	@cp $(USER_BUILD)/editor.elf $(INITRD_DIR)/editor
	@cp $(USER_BUILD)/http.elf $(INITRD_DIR)/http
	@cp $(USER_BUILD)/clock.elf $(INITRD_DIR)/clock
	@cp $(USER_BUILD)/guess.elf $(INITRD_DIR)/guess
	@cp $(USER_BUILD)/snake.elf $(INITRD_DIR)/snake
	@cp $(USER_BUILD)/browser.elf $(INITRD_DIR)/browser
	@cp $(USER_BUILD)/selftest.elf $(INITRD_DIR)/selftest
	@cp $(USER_BUILD)/proctest.elf $(INITRD_DIR)/proctest
	@cp $(USER_BUILD)/fdtest.elf   $(INITRD_DIR)/fdtest
	@cp $(USER_BUILD)/p10test.elf  $(INITRD_DIR)/p10test
	@cp $(USER_BUILD)/argv_echo.elf $(INITRD_DIR)/argv_echo
	@cp $(USER_BUILD)/execve_child.elf $(INITRD_DIR)/execve_child
	@cp $(USER_BUILD)/gltest.elf  $(INITRD_DIR)/gltest
	@cp $(USER_BUILD)/glcube.elf  $(INITRD_DIR)/glcube
	@cp $(USER_BUILD)/glgears.elf $(INITRD_DIR)/glgears
	@cp $(USER_BUILD)/gcalc.elf   $(INITRD_DIR)/gcalc
	@cp $(USER_BUILD)/gedit.elf   $(INITRD_DIR)/gedit
	@cp $(USER_BUILD)/gfiles.elf  $(INITRD_DIR)/gfiles
	@cp $(USER_BUILD)/gterm.elf   $(INITRD_DIR)/gterm
	@cp $(USER_BUILD)/gsysmon.elf $(INITRD_DIR)/gsysmon
	@cp $(USER_BUILD)/gabout.elf  $(INITRD_DIR)/gabout
	@cp $(USER_BUILD)/gtaskmgr.elf $(INITRD_DIR)/gtaskmgr
	@cp $(USER_BUILD)/glaunch.elf $(INITRD_DIR)/glaunch
	@cp $(USER_BUILD)/apm.elf     $(INITRD_DIR)/apm
	@cp $(USER_BUILD)/matrix.elf  $(INITRD_DIR)/matrix.pkg
	@cp $(USER_BUILD)/life.elf    $(INITRD_DIR)/life.pkg
	@cp $(USER_BUILD)/fetch.elf   $(INITRD_DIR)/fetch.pkg
	@cp $(USER_BUILD)/play.elf    $(INITRD_DIR)/play
	@cp $(USER_BUILD)/gaudio.elf  $(INITRD_DIR)/gaudio
	@cp $(USER_BUILD)/gbrowser.elf $(INITRD_DIR)/gbrowser
	@cp $(USER_BUILD)/gusb.elf    $(INITRD_DIR)/gusb
	@cp $(USER_BUILD)/tcpserver.elf $(INITRD_DIR)/tcpserver
	@cp $(USER_BUILD)/elfperm.elf $(INITRD_DIR)/elfperm
	@cp $(USER_BUILD)/udptest.elf $(INITRD_DIR)/udptest
	@cp $(USER_BUILD)/timestest.elf $(INITRD_DIR)/timestest
	@cp $(USER_BUILD)/fifolinktest.elf $(INITRD_DIR)/fifolinktest
	@cp $(USER_BUILD)/stackguard.elf $(INITRD_DIR)/stackguard
	@bash tools/mkinitrd.sh $(INITRD_DIR) $@

run: iso
	@bash tools/run_qemu.sh $(ISO_IMAGE)

run-usb-msc: iso
	@bash tools/run_qemu_usb_msc.sh $(ISO_IMAGE)

debug: iso
	@echo "Attach with: gdb $(KERNEL_ELF) -ex 'target remote :1234'"
	@bash tools/debug_qemu.sh $(ISO_IMAGE)

# ---- Host-side unit tests (built with the host compiler, no freestanding) ----
UNIT_TESTS   := $(BUILD_DIR)/test_glmath $(BUILD_DIR)/test_glstate \
                $(BUILD_DIR)/test_glimm $(BUILD_DIR)/test_glraster \
                $(BUILD_DIR)/test_glclip $(BUILD_DIR)/test_gllight \
                $(BUILD_DIR)/test_gltex $(BUILD_DIR)/test_glarray \
                $(BUILD_DIR)/test_glu $(BUILD_DIR)/test_glbackend \
                $(BUILD_DIR)/test_gpu_syscall \
                $(BUILD_DIR)/test_pmm $(BUILD_DIR)/test_heap \
                $(BUILD_DIR)/test_string $(BUILD_DIR)/test_bitmap \
                $(BUILD_DIR)/test_net $(BUILD_DIR)/test_kprintf \
                $(BUILD_DIR)/test_libc $(BUILD_DIR)/test_3d \
                $(BUILD_DIR)/test_usb $(BUILD_DIR)/test_wm \
                $(BUILD_DIR)/test_usb_audio $(BUILD_DIR)/test_usb_cdc \
                $(BUILD_DIR)/test_usb_full $(BUILD_DIR)/test_usb_hub \
                $(BUILD_DIR)/test_usb_isoc \
                $(BUILD_DIR)/test_vfs $(BUILD_DIR)/test_network \
                $(BUILD_DIR)/test_elf $(BUILD_DIR)/test_gui \
                $(BUILD_DIR)/test_process $(BUILD_DIR)/test_spinlock \
                $(BUILD_DIR)/test_fat32 $(BUILD_DIR)/test_errno \
                $(BUILD_DIR)/test_ctype $(BUILD_DIR)/test_open_flags \
                $(BUILD_DIR)/test_lseek \
                $(BUILD_DIR)/test_signals \
                $(BUILD_DIR)/test_termios \
                $(BUILD_DIR)/test_jobcontrol \
                $(BUILD_DIR)/test_permissions \
                $(BUILD_DIR)/test_cow \
                $(BUILD_DIR)/test_slab \
                $(BUILD_DIR)/test_virgl \
                $(BUILD_DIR)/test_virtio_net \
                $(BUILD_DIR)/test_stack_guard \
                $(BUILD_DIR)/test_select_stack \
                $(BUILD_DIR)/test_vma \
                $(BUILD_DIR)/test_page_cache \
                $(BUILD_DIR)/test_mprotect \
                $(BUILD_DIR)/test_gdt_tss \
                $(BUILD_DIR)/test_boot_offsets \
                $(BUILD_DIR)/test_boot_info \
                $(BUILD_DIR)/test_q1_headers \
                $(BUILD_DIR)/test_pthread_ext \
                $(BUILD_DIR)/test_string_ext \
                $(BUILD_DIR)/test_stdio_ext \
                $(BUILD_DIR)/test_stdlib_ext \
                $(BUILD_DIR)/test_q11_new \
                $(BUILD_DIR)/test_posix_spawn \
                $(BUILD_DIR)/test_q10_stubs \
                $(BUILD_DIR)/test_ipc

test-unit: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do echo "[unit] running $$t"; ./$$t || exit 1; done

$(BUILD_DIR)/test_pmm: tests/unit/test_pmm.c kernel/lib/bitmap.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_heap: tests/unit/test_heap.c kernel/mm/heap.c kernel/mm/heap.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . tests/unit/test_heap.c kernel/mm/heap.c -o $@

$(BUILD_DIR)/test_string: tests/unit/test_string.c kernel/lib/string.c kernel/lib/string.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_bitmap: tests/unit/test_bitmap.c kernel/lib/bitmap.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_net: tests/unit/test_net.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_kprintf: tests/unit/test_kprintf.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_libc: tests/unit/test_libc.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_3d: tests/unit/test_3d.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@ -lm

# ---- libgl host unit tests -----------------------------------------------
#
# Every GL unit test links the REAL libgl sources, never a copy, so a test can
# never drift away from the shipping implementation (GL_PLAN.md principle 1).
#
# The source list is a SINGLE variable shared by all of them.  It used to be
# duplicated per rule, and when phases G5/G6 added gllight.c, gltexture.c and
# glfrag.c the copies drifted: auraglx.c gained calls to the new modules but
# the test_glstate rule still listed only the G1 set, so `make test-unit`
# failed to link on a clean tree.  One variable makes that class of bug
# impossible -- adding a module here fixes every test at once.
LIBGL_TEST_SRCS := libgl/src/auraglx.c libgl/src/glstate.c \
                   libgl/src/glmath.c libgl/src/glmatrix.c \
                   libgl/src/glimm.c libgl/src/glraster.c \
                   libgl/src/glclip.c libgl/src/gllight.c \
                   libgl/src/gltexture.c libgl/src/glfrag.c \
                   libgl/src/glarray.c libgl/src/gllist.c \
                   libgl/src/glu.c libgl/src/glbackend.c \
                   libgl/src/glvirgl.c

LIBGL_TEST_HDRS := libgl/src/glcontext.h libgl/src/glvertex.h \
                   libgl/include/GL/glu.h libgl/include/GL/glbackend.h \
                   libgl/include/GL/gl.h libgl/include/GL/glmath.h \
                   libgl/include/GL/auraglx.h

LIBGL_TEST_STUB := tests/unit/glstub/auragui_stub.c
LIBGL_TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 \
                     -I libgl/include -I libgl/src -I tests/unit/glstub

# What each test covers:
#   test_glmath   vector/matrix math (no context needed, links glmath.c alone)
#   test_glstate  context lifecycle, GL error contract, glClear, presentation
#   test_glimm    matrix stacks, immediate mode, the transform pipeline
#   test_glraster filled rasterizer, depth buffer, culling, top-left fill rule
#   test_glclip   frustum clipping and the glPushAttrib/glPopAttrib stack
#   test_gllight  the GL 1.1 lighting equation and materials
#   test_gltex    texture objects, sampling, perspective correction, blending,
#                 the alpha test and fog
#
# libauragui cannot be built for the host (it needs AuraLite's freestanding
# libc), so tests/unit/glstub/ provides a recording stand-in for ag_blit() and
# ag_render_now() -- the code under test is still the real auraglx.c.
LIBGL_TESTS := test_glstate test_glimm test_glraster test_glclip \
               test_gllight test_gltex test_glarray test_glu \
               test_glbackend

$(addprefix $(BUILD_DIR)/,$(LIBGL_TESTS)): $(BUILD_DIR)/%: tests/unit/%.c \
                                           $(LIBGL_TEST_SRCS) \
                                           $(LIBGL_TEST_HDRS) \
                                           $(LIBGL_TEST_STUB)
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBGL_TEST_CFLAGS) $< $(LIBGL_TEST_SRCS) \
	          $(LIBGL_TEST_STUB) -o $@ -lm

# glmath.c is standalone: no context, no stub, so it gets its own rule.
# test_gpu_syscall links the REAL validator from kernel/gpu/gpu_cmdcheck.c.
# That file is deliberately free of kernel dependencies so this test exercises
# the shipping code rather than a copy — it is the function standing between a
# hostile process and the host GPU, so it gets direct malformed-input testing.
$(BUILD_DIR)/test_gpu_syscall: tests/unit/test_gpu_syscall.c \
                               kernel/gpu/gpu_cmdcheck.c \
                               kernel/gpu/gpu_syscalls.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	          tests/unit/test_gpu_syscall.c kernel/gpu/gpu_cmdcheck.c -o $@

$(BUILD_DIR)/test_glmath: tests/unit/test_glmath.c libgl/src/glmath.c \
                          libgl/include/GL/glmath.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I libgl/include \
	          tests/unit/test_glmath.c libgl/src/glmath.c -o $@ -lm

$(BUILD_DIR)/test_virgl: tests/unit/test_virgl.c drivers/gpu/virgl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_virtio_net: tests/unit/test_virtio_net.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_stack_guard: tests/unit/test_stack_guard.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_select_stack: tests/unit/test_select_stack.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_vma: tests/unit/test_vma.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_page_cache: tests/unit/test_page_cache.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@ -lpthread

$(BUILD_DIR)/test_mprotect: tests/unit/test_mprotect.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_boot_info: tests/unit/test_boot_info.c kernel/boot_info.c kernel/boot_info.h boot/shared/boot_info.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	    tests/unit/test_boot_info.c kernel/boot_info.c -o $@

$(BUILD_DIR)/test_boot_offsets: tests/unit/test_boot_offsets.c boot/shared/boot_info.h $(BOOT_OFFSETS_H)
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . -I $(BUILD_DIR) $< -o $@

# POSIX.1-2024 Q1: verify every new C standard header (stdarg.h, stddef.h,
# stdint.h, float.h, inttypes.h, iso646.h, stdalign.h, stdnoreturn.h,
# tgmath.h, complex.h, fenv.h, stdatomic.h, wctype.h, strings.h, uchar.h,
# setjmp.h, threads.h) #includes cleanly and that their non-builtin logic
# behaves correctly.
# ---- Phase Q3: string extension unit test ----
$(BUILD_DIR)/test_string_ext: tests/unit/test_string_ext.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q2: stdio extension unit test ----
$(BUILD_DIR)/test_stdio_ext: tests/unit/test_stdio_ext.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@



# ---- Phase Q4: stdlib extension unit test ----
$(BUILD_DIR)/test_stdlib_ext: tests/unit/test_stdlib_ext.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q11: new POSIX.1-2024 functions unit test ----
$(BUILD_DIR)/test_q11_new: tests/unit/test_q11_new.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q6: pthread extension unit test ----
$(BUILD_DIR)/test_pthread_ext: tests/unit/test_pthread_ext.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q9: posix_spawn unit test ----
$(BUILD_DIR)/test_posix_spawn: tests/unit/test_posix_spawn.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q10: stub headers unit test ----
$(BUILD_DIR)/test_q10_stubs: tests/unit/test_q10_stubs.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q7: IPC unit test ----
$(BUILD_DIR)/test_ipc: tests/unit/test_ipc.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_q1_headers: tests/unit/test_q1_headers.c \
                               libc/include/stdarg.h libc/include/stddef.h libc/include/stdint.h \
                               libc/include/float.h libc/include/inttypes.h libc/include/iso646.h \
                               libc/include/stdalign.h libc/include/stdnoreturn.h libc/include/tgmath.h \
                               libc/include/complex.h libc/include/fenv.h libc/include/stdatomic.h \
                               libc/include/wctype.h libc/include/strings.h libc/include/uchar.h \
                               libc/include/setjmp.h libc/include/threads.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_gdt_tss: tests/unit/test_gdt_tss.c kernel/arch/x86_64/gdt.c kernel/arch/x86_64/gdt.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . tests/unit/test_gdt_tss.c kernel/arch/x86_64/gdt.c -o $@

$(BUILD_DIR)/test_usb: tests/unit/test_usb.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- USB class/protocol point tests (previously present but not wired up) ----
$(BUILD_DIR)/test_usb_audio: tests/unit/test_usb_audio.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_usb_cdc: tests/unit/test_usb_cdc.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_usb_full: tests/unit/test_usb_full.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_usb_hub: tests/unit/test_usb_hub.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_usb_isoc: tests/unit/test_usb_isoc.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_wm: tests/unit/test_wm.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- New unit tests (Phase 15+) ----

$(BUILD_DIR)/test_vfs: tests/unit/test_vfs.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

$(BUILD_DIR)/test_network: tests/unit/test_network.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

$(BUILD_DIR)/test_elf: tests/unit/test_elf.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

$(BUILD_DIR)/test_gui: tests/unit/test_gui.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

$(BUILD_DIR)/test_process: tests/unit/test_process.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

$(BUILD_DIR)/test_spinlock: tests/unit/test_spinlock.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@ -lpthread

$(BUILD_DIR)/test_fat32: tests/unit/test_fat32.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

$(BUILD_DIR)/test_errno: tests/unit/test_errno.c libc/include/errno.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_ctype: tests/unit/test_ctype.c libc/include/ctype.h \
                         libc/include/limits.h libc/include/stdbool.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_open_flags: tests/unit/test_open_flags.c libc/include/fcntl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_lseek: tests/unit/test_lseek.c libc/include/sys/uio.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_signals: tests/unit/test_signals.c libc/include/signal.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_termios: tests/unit/test_termios.c libc/include/termios.h libc/include/sys/ioctl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_jobcontrol: tests/unit/test_jobcontrol.c libc/include/sys/wait.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_permissions: tests/unit/test_permissions.c libc/include/unistd.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_cow: tests/unit/test_cow.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_slab: tests/unit/test_slab.c kernel/mm/slab.c kernel/mm/slab.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . tests/unit/test_slab.c kernel/mm/slab.c -o $@

clean:
	rm -rf $(BUILD_DIR)

# ---- QEMU integration tests ----
# Each case in tests/integration/cases/ boots the ISO in QEMU and asserts on
# the serial console.  Logs are written to build/integration-logs/.
test-integration: iso
	@bash tests/integration/run_all.sh

test-integration-fast: iso
	@bash tests/integration/run_all.sh --fast

# Convenience: run host unit tests AND QEMU integration tests.
test: test-unit test-integration
