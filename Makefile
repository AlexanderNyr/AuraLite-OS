# =============================================================================
# AuraLite OS — Top-level Makefile
# Toolchain: Clang (--target=x86_64-elf) + LLD + NASM, booted by the custom
# BIOS/UEFI loader chain (BL2..BL7) shipped in this repository.
# =============================================================================

ARCH        := x86_64
TARGET      := $(ARCH)-elf
CC          := clang
LD          := ld.lld
AS          := nasm
AR          := ar
HOST_CC     := cc

BUILD_DIR   := build

# mformat/mcopy are needed by mkisoimage_bios.sh / mkisoimage_dual.sh
# to build the FAT32 partition; lld-link is needed by `make efi` to
# link BOOTX64.EFI as PE32+.
### RUST: add rustc to required tools
REQUIRED_TOOLS := $(CC) $(LD) $(AS) $(HOST_CC) python3 tar \
                  mformat mcopy lld-link rustc
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
               -DARCH_X86_64 -I . -I $(BUILD_DIR) -I w32/include

# FIX_R8 (FIXES_PLAN.md): compile-time keyboard layout selection.  This only
# picks the layout that is active at boot; the `kbd` shell command switches
# at runtime (syscall SYS_KBD_LAYOUT) regardless of this setting.
# Usage:  make KEYMAP=de
KEYMAP ?= us
ifeq ($(KEYMAP),de)
CFLAGS      += -DKEYBOARD_DEFAULT_LAYOUT=keymap_de
endif

# OPT_O2 (OPT_PLAN.md): build-default boot self-test intensity.  This is
# what real hardware (no fw_cfg) gets; a QEMU boot can override it at run
# time with -fw_cfg name=opt/auralite.selftest,string=full|fast|off, which
# is how the integration lib pins CI boots to `full`.
# Usage:  make SELFTEST=full
SELFTEST ?= fast
ifeq ($(SELFTEST),full)
CFLAGS      += -DSELFTEST_DEFAULT_FULL
endif
ifeq ($(SELFTEST),off)
CFLAGS      += -DSELFTEST_DEFAULT_OFF
endif

ASFLAGS     := -f elf64 -I $(BUILD_DIR)/

# The linker script fixes the higher-half address; no --image-base needed.
LDFLAGS     := -nostdlib -static -T kernel.ld -z max-page-size=4096

# A1 (ARM64_PLAN): kernel/dt/ is excluded too -- the shared DTB walker
# belongs to the DTB-consuming kernels (riscv64, aarch64), which list
# it explicitly in their KERNEL*_SHARED variables.  x86 has no DTB and
# no dt_phys_to_virt definition; the A0 lesson (this find IS the
# shared build logic) caught its second victim here, at link time.
KERNEL_SRCS := $(shell find kernel drivers -name '*.c' -not -path 'kernel/arch/i386/*' -not -path 'kernel/arch/riscv64/*' -not -path 'kernel/arch/aarch64/*' -not -path 'kernel/dt/*') w32/src/w32_pe.c
# WIN32_PLAN.md W32-3: the kernel PE loader calls the same parser the host
# unit test and fuzz corpus exercise (D2 -- one implementation, tested once).
# w32/src/ is freestanding C with no libc dependency, so it compiles with the
# kernel CFLAGS unchanged.
# I386_PLAN I1: kernel/arch/i386/ is excluded from the x86_64 kernel -- it
# builds through the separate kernel32 target with the i686 toolchain flags.
KERNEL_ASMS := $(shell find kernel drivers -name '*.asm' -not -path 'kernel/arch/i386/*' -not -path 'kernel/arch/riscv64/*' -not -path 'kernel/arch/aarch64/*')
# NOTE: a .c and .asm file MUST NOT share a base name (e.g. foo.c + foo.asm),
# because both compile to the same object path build/.../foo.o, which would
# collide and double-link. Keep assembly stubs named distinctly (e.g.
# foo_stubs.asm). ISR stubs live in isr_stubs.asm for this reason.
KERNEL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS)) \
               $(patsubst %.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASMS))

.PHONY: all kernel user iso usb vbox vmware vm-configs run run-usb-msc clean \
        deps-check test-unit test-integration test-integration-fast test \
        libs sdk sdk-check w32-sdk w32-sdk-check

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
		echo "[deps] Debian/Ubuntu: sudo apt install clang lld nasm qemu-system-x86 mtools ovmf make gcc python3"; \
		echo "[deps] Also install Rust via: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"; \
		echo "[deps] Then: rustup target add $(RUST_TARGET)"; \
		exit 127; \
	fi
	@if ! $(RUSTC) --target=$(RUST_TARGET) --print target-libdir >/dev/null 2>&1; then \
		echo "[deps] missing Rust target: $(RUST_TARGET)"; \
		echo "[deps] install it with: rustup target add $(RUST_TARGET)"; \
		exit 127; \
	fi
	@# RISCV_PLAN V0: qemu-system-riscv64 is OPTIONAL, the mingw pattern --
	@# absent means the rv64 smoke test skips loudly, not that the build
	@# fails.  clang/lld already cross-compile to riscv64, so building
	@# kernelrv needs no new tools at all; only *running* it does.
	@if ! command -v qemu-system-riscv64 >/dev/null 2>&1; then \
		echo "[deps] optional tool absent: qemu-system-riscv64 (rv64 boot tests will skip)"; \
		echo "[deps] Debian/Ubuntu: sudo apt install qemu-system-misc"; \
	fi
	@# ARM64_PLAN A0: qemu-system-aarch64 is OPTIONAL, the same pattern --
	@# clang/lld already cross-compile to aarch64, so building kernela64
	@# needs no new tools at all; only *running* it does.
	@if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then \
		echo "[deps] optional tool absent: qemu-system-aarch64 (a64 boot tests will skip)"; \
		echo "[deps] Debian/Ubuntu: sudo apt install qemu-system-arm"; \
	fi

kernel: $(KERNEL_ELF)

# =============================================================================
# I386_PLAN I2: the i386 kernel (KERNEL32.ELF).
#
# Same clang/lld toolchain, one width down (--target=i686-elf, nasm -f
# elf32, ld.lld -m elf_i386) -- deps-check does not grow.  I1 booted a
# ~60-line stub through this target; I2 replaced the stub with the real
# bring-up kernel (GDT/TSS, 256-gate IDT, PIC, PIT, named exception
# diagnostics), built from everything under kernel/arch/i386/.
#
# -malign-double: the i386 System V psABI aligns uint64_t to 4 bytes, the
# AMD64 one to 8.  boot_info_t is written by 16-bit assembly against
# offsets generated from the 64-bit layout (build/boot_offsets.inc), so a
# plain -m32 compile silently reads mmap[] 8 bytes early -- measured: the
# I1 stub printed "mmap entries: 0" until this flag landed.  With
# -malign-double both ABIs agree on every offset in the struct, and the
# boot log asserts that at runtime via the magic + mmap_count checks.
# =============================================================================
KERNEL32_ELF  := $(BUILD_DIR)/kernel32.elf
KERNEL32_DIR  := kernel/arch/i386
# I8: the first SHARED sources compiled into the 32-bit kernel -- the
# proof of the I6 thesis.  drivers/pci/pci.c includes arch.h (migrated
# in the I6 batch) and compiles for both widths unchanged; every file
# added to this list is portable code that now has i386 as a second
# consumer.  Growth rule: a file lands here only when something on
# this side actually calls it.
KERNEL32_SHARED := drivers/pci/pci.c kernel/net/miniproto.c
KERNEL32_SRCS := $(shell find $(KERNEL32_DIR) -name '*.c') $(KERNEL32_SHARED)
KERNEL32_ASMS := $(shell find $(KERNEL32_DIR) -name '*.asm')
KERNEL32_OBJS := $(patsubst %.c,$(BUILD_DIR)/k32/%.o,$(KERNEL32_SRCS)) \
                 $(patsubst %.asm,$(BUILD_DIR)/k32/%.o,$(KERNEL32_ASMS))
# -Werror + truncation warnings promoted (I6 gate): the i386 build is
# where a 64-bit value silently narrowing IS the bug class the width
# sweep exists for, so here it is fatal.  -Wshorten-64-to-32 is the
# clang spelling; -Wconversion is deliberately NOT enabled (it flags
# benign integer promotions and would bury the signal).
CFLAGS32      := --target=i686-elf \
                 -std=c11 -ffreestanding -fno-stack-protector \
                 -fno-pie -fno-pic -mno-mmx -mno-sse -mno-sse2 \
                 -malign-double \
                 -fno-omit-frame-pointer \
                 -Wall -Wextra -Wno-unused-parameter \
                 -Werror -Wshorten-64-to-32 \
                 -O2 -g -I .

# Depend on every i386 header: the tree is small enough that a full
# rebuild on any header edit is cheaper than the stale-object hunt it
# prevents (measured: a KHEAP32_BASE move in paging32.h left kheap32.o
# compiled against the old constant and the heap failed to commit).
KERNEL32_HDRS := $(shell find $(KERNEL32_DIR) -name '*.h') boot/shared/boot_info.h

$(BUILD_DIR)/k32/%.o: %.c $(KERNEL32_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS32) -c $< -o $@

$(BUILD_DIR)/k32/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 -o $@ $<

$(KERNEL32_ELF): $(KERNEL32_OBJS) $(KERNEL32_DIR)/kernel32.ld
	$(LD) -m elf_i386 -nostdlib -static -T $(KERNEL32_DIR)/kernel32.ld \
	    $(KERNEL32_OBJS) -o $@
	@echo "  [kernel32] $@ ($$(du -h $@ | cut -f1))"

.PHONY: kernel32
kernel32: $(KERNEL32_ELF)

# =============================================================================
# RISCV_PLAN V0: the rv64 kernel (kernelrv.elf).
#
# Same clang/lld toolchain, third target (--target=riscv64 -march=rv64gc,
# ld.lld -m elf64lriscv) -- REQUIRED_TOOLS does not grow.  Assembly is
# .S through clang's integrated GNU as: there is no NASM on this arch.
#
# -mcmodel=medany: the code must be linkable anywhere in the low 2 GiB
#   span around PC (the medlow model caps at +-2 GiB of address zero,
#   which 0x80200000 violates).
# -mno-relax: linker relaxation rewrites call sequences against gp; the
#   kernel never sets __global_pointer$ (see kernelrv.ld), so relaxation
#   must be off or the linker would emit gp-relative accesses to a
#   register containing garbage.
# -mabi=lp64d: rv64gc's D extension is real; the FPU context story is
#   costed into V4 (plan risk: the M1 lesson).
# =============================================================================
KERNELRV_ELF  := $(BUILD_DIR)/kernelrv.elf
KERNELRV_DIR  := kernel/arch/riscv64
# V7: kernel/net/miniproto.c is the SHARED bring-up protocol file --
# the second consumer (net32.c is the first); the whole point of the
# lift is that both NICs prove the same packets.
# A1 (ARM64_PLAN): kernel/dt/fdt.c is the SHARED DTB walker, promoted
# out of kernel/arch/riscv64/ -- both DTB-consuming kernels compile
# this one file, and the claim checkers assert it (a promotion that
# forked would be worse than no promotion).
KERNELRV_SHARED := kernel/net/miniproto.c kernel/dt/fdt.c
KERNELRV_SRCS := $(shell find $(KERNELRV_DIR) -name '*.c' 2>/dev/null) $(KERNELRV_SHARED)
KERNELRV_ASMS := $(shell find $(KERNELRV_DIR) -name '*.S' 2>/dev/null)
KERNELRV_OBJS := $(patsubst %.c,$(BUILD_DIR)/krv/%.o,$(KERNELRV_SRCS)) \
                 $(patsubst %.S,$(BUILD_DIR)/krv/%.o,$(KERNELRV_ASMS))
KERNELRV_HDRS := $(shell find $(KERNELRV_DIR) -name '*.h' 2>/dev/null) boot/shared/boot_info.h kernel/dt/fdt.h
CFLAGSRV      := --target=riscv64 -march=rv64gc -mabi=lp64d \
                 -mcmodel=medany -mno-relax \
                 -std=c11 -ffreestanding -fno-stack-protector \
                 -fno-pie -fno-pic \
                 -Wall -Wextra -Wno-unused-parameter \
                 -Werror \
                 -O2 -g -I .

$(BUILD_DIR)/krv/%.o: %.c $(KERNELRV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSRV) -c $< -o $@

$(BUILD_DIR)/krv/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSRV) -c $< -o $@

$(KERNELRV_ELF): $(KERNELRV_OBJS) $(KERNELRV_DIR)/kernelrv.ld
	$(LD) -m elf64lriscv -nostdlib -static -T $(KERNELRV_DIR)/kernelrv.ld \
	    $(KERNELRV_OBJS) -o $@
	@echo "  [kernelrv] $@ ($$(du -h $@ | cut -f1))"

.PHONY: kernelrv
kernelrv: $(KERNELRV_ELF)

# Convenience: boot the rv64 kernel under QEMU (serial on stdio).
.PHONY: run-rv
run-rv: kernelrv
	qemu-system-riscv64 -machine virt -m 256M \
	    -display none -serial stdio -no-reboot \
	    -kernel $(KERNELRV_ELF) \
	    $(if $(wildcard $(BUILD_DIR)/initrd.tar),-initrd $(BUILD_DIR)/initrd.tar)

# =============================================================================
# ARM64_PLAN A0: the aarch64 kernel (kernela64.elf).
#
# Same clang/lld toolchain, fourth target (--target=aarch64-unknown-
# none-elf, ld.lld -m aarch64linux) -- REQUIRED_TOOLS does not grow.
# Assembly is .S through clang's integrated GNU as, like riscv64.
#
# -mstrict-align: LOAD-BEARING before the MMU (plan Fact 5.1).  With
#   SCTLR_EL1.M clear all memory is Device-nGnRnE and an unaligned
#   access faults -- with no vector table installed yet, that is a
#   silent hang, not a message.  A3 measures whether the flag can be
#   dropped once paging is on; until a measurement says so, it stays.
# -mgeneral-regs-only: no FP/SIMD registers in kernel code -- the
#   compiler must not spill through q-registers the context switch
#   does not save yet.  A4 owns the FPU story (the M1 lesson, costed).
# =============================================================================
KERNELA64_ELF  := $(BUILD_DIR)/kernela64.elf
KERNELA64_DIR  := kernel/arch/aarch64
# A1: the shared DTB walker -- the same object list entry the rv64
# kernel carries; both consumers, one file (the promotion's whole point).
KERNELA64_SHARED := kernel/dt/fdt.c
KERNELA64_SRCS := $(shell find $(KERNELA64_DIR) -name '*.c' 2>/dev/null) $(KERNELA64_SHARED)
KERNELA64_ASMS := $(shell find $(KERNELA64_DIR) -name '*.S' 2>/dev/null)
KERNELA64_OBJS := $(patsubst %.c,$(BUILD_DIR)/ka64/%.o,$(KERNELA64_SRCS)) \
                  $(patsubst %.S,$(BUILD_DIR)/ka64/%.o,$(KERNELA64_ASMS))
KERNELA64_HDRS := $(shell find $(KERNELA64_DIR) -name '*.h' 2>/dev/null) boot/shared/boot_info.h kernel/dt/fdt.h
CFLAGSA64      := --target=aarch64-unknown-none-elf \
                  -mstrict-align -mgeneral-regs-only \
                  -std=c11 -ffreestanding -fno-stack-protector \
                  -fno-pie -fno-pic \
                  -Wall -Wextra -Wno-unused-parameter \
                  -Werror \
                  -O2 -g -I .

$(BUILD_DIR)/ka64/%.o: %.c $(KERNELA64_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSA64) -c $< -o $@

$(BUILD_DIR)/ka64/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSA64) -c $< -o $@

$(KERNELA64_ELF): $(KERNELA64_OBJS) $(KERNELA64_DIR)/kernela64.ld
	$(LD) -m aarch64linux -nostdlib -static -T $(KERNELA64_DIR)/kernela64.ld \
	    $(KERNELA64_OBJS) -o $@
	@echo "  [kernela64] $@ ($$(du -h $@ | cut -f1))"

.PHONY: kernela64
kernela64: $(KERNELA64_ELF)

# Convenience: boot the aarch64 kernel under QEMU (serial on stdio).
.PHONY: run-a64
run-a64: kernela64
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial stdio -no-reboot \
	    -kernel $(KERNELA64_ELF) \
	    $(if $(wildcard $(BUILD_DIR)/initrd.tar),-initrd $(BUILD_DIR)/initrd.tar)

SMALLSH_SRC     := userspace/system/smallsh/smallsh.c
SMALLSH_DEFS32  := -DAURA_LIBC='"lib/libc32/libc32.h"' \
                   -DAURA_PUTS=puts32 \
                   -DAURA_UNAME='"AuraLite OS i386 (protected mode, higher half, I386_PLAN I7)"' \
                   -DAURA_RUN_EXAMPLE='"bin32/init32"'
SMALLSH_DEFSRV  := -DAURA_LIBC='"lib/libcrv/libcrv.h"' \
                   -DAURA_PUTS=puts_rv \
                   -DAURA_UNAME='"AuraLite OS riscv64 (Sv39 higher half, RISCV_PLAN V5)"' \
                   -DAURA_RUN_EXAMPLE='"binrv/init"'

# =============================================================================
# RISCV_PLAN V5: the rv64 userspace (initrv + smallsh over libcrv).
#
# The I5 pattern at the third width: crt0 + ecall wrapper + a static
# ELF at 0x08048000 (user_rv.ld), the shell at 0x30000000 (shellrv.ld,
# the same parent/child address treaty as shell32.ld).  smallsh is the
# PROMOTED shell32.c -- one source, two arches, seam = AURA_LIBC.
# =============================================================================
USERRV_BUILD := $(BUILD_DIR)/userrv
INITRV_ELF   := $(USERRV_BUILD)/init
SHELLRV_ELF  := $(USERRV_BUILD)/smallsh

CFLAGSRV_USER := --target=riscv64 -march=rv64gc -mabi=lp64d \
                 -mcmodel=medany -mno-relax \
                 -std=c11 -ffreestanding -fno-stack-protector \
                 -fno-pie -fno-pic \
                 -Wall -Wextra -Wno-unused-parameter \
                 -Werror \
                 -O2 -g -I .

$(USERRV_BUILD)/crt0_rv.o: lib/libcrv/crt0_rv.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSRV_USER) -c $< -o $@

$(USERRV_BUILD)/syscall_rv.o: lib/libcrv/syscall_rv.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSRV_USER) -c $< -o $@

$(USERRV_BUILD)/initrv.o: userspace/system/initrv/initrv.c lib/libcrv/libcrv.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSRV_USER) -c $< -o $@

$(INITRV_ELF): $(USERRV_BUILD)/crt0_rv.o $(USERRV_BUILD)/initrv.o $(USERRV_BUILD)/syscall_rv.o lib/libcrv/user_rv.ld
	$(LD) -m elf64lriscv -nostdlib -static -T lib/libcrv/user_rv.ld \
	    $(USERRV_BUILD)/crt0_rv.o $(USERRV_BUILD)/initrv.o $(USERRV_BUILD)/syscall_rv.o -o $@
	@echo "  [userrv] $@"

$(USERRV_BUILD)/smallsh.o: $(SMALLSH_SRC) lib/libcrv/libcrv.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGSRV_USER) $(SMALLSH_DEFSRV) -c $< -o $@

$(SHELLRV_ELF): $(USERRV_BUILD)/crt0_rv.o $(USERRV_BUILD)/smallsh.o $(USERRV_BUILD)/syscall_rv.o lib/libcrv/shellrv.ld
	$(LD) -m elf64lriscv -nostdlib -static -T lib/libcrv/shellrv.ld \
	    $(USERRV_BUILD)/crt0_rv.o $(USERRV_BUILD)/smallsh.o $(USERRV_BUILD)/syscall_rv.o -o $@
	@echo "  [userrv] $@"

.PHONY: userrv
userrv: $(INITRV_ELF) $(SHELLRV_ELF)

# =============================================================================
# I386_PLAN I5: the 32-bit userspace (init32 + libc32).
#
# Linked at 0x08048000 (the classic i386 ET_EXEC base, inside the
# loader's [ELF32_USER_MIN, ELF32_USER_MAX) window) with -Ttext rather
# than a script: two segments, no ambition -- the full user linker
# script arrives with the real libc port (I6).  The binaries land in
# the SHARED initrd under /bin32, so one archive serves both kernels
# and each loader refuses the other's ELF class.
# =============================================================================
USER32_BUILD := $(BUILD_DIR)/user32
INIT32_ELF   := $(USER32_BUILD)/init32

$(USER32_BUILD)/crt0_32.o: lib/libc32/crt0_32.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 -o $@ $<

$(USER32_BUILD)/syscall32.o: lib/libc32/syscall32.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 -o $@ $<

$(USER32_BUILD)/init32.o: userspace/system/init32/init32.c lib/libc32/libc32.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS32) -c $< -o $@

$(INIT32_ELF): $(USER32_BUILD)/crt0_32.o $(USER32_BUILD)/init32.o $(USER32_BUILD)/syscall32.o lib/libc32/user32.ld
	$(LD) -m elf_i386 -nostdlib -static -T lib/libc32/user32.ld \
	    $(USER32_BUILD)/crt0_32.o $(USER32_BUILD)/init32.o $(USER32_BUILD)/syscall32.o -o $@
	@echo "  [user32] $@"

# I7: the interactive shell.  Linked at 0x30000000 (shell32.ld) so the
# children it spawns at 0x08048000 share the address space -- see the
# script's header for the treaty.
#
# RISCV_PLAN V5 promoted the source: shell32.c became
# userspace/system/smallsh/smallsh.c, ONE portable-C shell compiled
# for both bring-up arches.  The libc seam (AURA_LIBC + the identity
# strings) is the whole per-arch surface; the i386 binary must behave
# byte-identically and i386_shell_smoke.sh is the gate that proves it.
SHELL32_ELF := $(USER32_BUILD)/shell32

$(USER32_BUILD)/shell32.o: $(SMALLSH_SRC) lib/libc32/libc32.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS32) $(SMALLSH_DEFS32) -c $< -o $@

$(SHELL32_ELF): $(USER32_BUILD)/crt0_32.o $(USER32_BUILD)/shell32.o $(USER32_BUILD)/syscall32.o lib/libc32/shell32.ld
	$(LD) -m elf_i386 -nostdlib -static -T lib/libc32/shell32.ld \
	    $(USER32_BUILD)/crt0_32.o $(USER32_BUILD)/shell32.o $(USER32_BUILD)/syscall32.o -o $@
	@echo "  [user32] $@"

.PHONY: user32
user32: $(INIT32_ELF) $(SHELL32_ELF)

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

# USB_PLAN U2: -Wunreachable-code, as an error, for the USB drivers.
#
# Not a style preference.  xhci_control_transfer() carried a complete, real
# TRB implementation that was shadowed by a `return data_len;` on the line
# above it, and clang had been reporting exactly that -- "code will never be
# executed" at xhci.c:887 -- for as long as the fabrication existed.  The
# warning is off by default in -Wall/-Wextra, so nobody saw it.  Turning it
# on here means that particular shape of mistake cannot come back quietly.
$(BUILD_DIR)/drivers/usb/%.o: drivers/usb/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wunreachable-code -Werror=unreachable-code -c $< -o $@

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

USER_CFLAGS  := -D__AURALITE__ -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
                -O2 -Wall -Wextra -Werror -I . -I lib/libc/include
USER_LDFLAGS := -nostdlib -static -T lib/libc/user.ld -z max-page-size=4096

### RUST: compiler and flags for Rust
RUSTC       := rustc
RUST_TARGET := x86_64-unknown-none
RUSTFLAGS   := --target=$(RUST_TARGET) -C relocation-model=static -C opt-level=2 -C debug-assertions=no

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
                   $(USER_BUILD)/posix_extra.o $(USER_BUILD)/posix_spawn.o $(USER_BUILD)/q10_stubs.o \
                   $(USER_BUILD)/progpath.o $(USER_BUILD)/apkg.o

# The C runtime objects, as one list.  This is the ONLY place they are
# enumerated: the archive is built from it and the link line names the
# archive, so the two cannot drift apart (SDK_PLAN phase S0).
#
# crt0.o is deliberately NOT in the archive.  It defines exactly one symbol,
# _start, which nothing references -- it is the ELF entry point, reached
# through e_entry rather than through a relocation.  An archive member that
# resolves no undefined symbol is never pulled in, so archiving crt0 would
# silently produce programs with no entry code.  Real toolchains keep crt0.o
# a standalone object for the same reason, and it is named explicitly on the
# link line below.
LIBAURAC_OBJS := $(USER_BUILD)/syscall.o $(USER_BUILD)/libc.o \
                 $(USER_BUILD)/malloc.o $(USER_BUILD)/sigreturn.o \
                 $(USER_BUILD)/setjmp.o $(USER_BUILD)/compat.o \
                 $(LIBC_EXTRA_OBJS)

USER_LIBDIR  := $(BUILD_DIR)/lib
LIBAURAC     := $(USER_LIBDIR)/libaurac.a
LIBAURAGUI   := $(USER_LIBDIR)/libauragui.a
LIBAGL       := $(USER_LIBDIR)/libaGL.a

# Two names, because they are two different things and conflating them is how
# a linker flag ends up in a prerequisite list:
#
#   USER_COMMON     — the FILES to depend on (make prerequisites)
#   USER_COMMON_LNK — the ARGUMENTS to pass to the linker
#
# --whole-archive on libaurac.a preserves the PREVIOUS behaviour exactly.
# Before S0 all 26 objects were named on the link line, so every one was
# linked unconditionally.  Plain archive semantics -- pull a member only if it
# resolves an undefined symbol -- would silently change that.
#
# Measured, not assumed.  Linking calc.o against the archive WITHOUT the flag
# drops q10_stubs.o entirely: `closelog` disappears and the binary shrinks
# from 96936 to 58472 bytes.  Those stubs exist precisely so that a program
# calling a not-yet-implemented POSIX function links instead of failing, which
# is a promise that only holds if the member is present.  (sigreturn.o
# survives either way -- libc.c references __sigreturn for sa_restorer -- so
# it is not the example to cite here.)
#
# The cost is that every program carries the whole libc.  That is exactly what
# happened before archiving, so nothing regressed; making libc granular is a
# separate change with its own risks.
USER_COMMON     := $(USER_BUILD)/crt0.o $(LIBAURAC)
USER_COMMON_LNK := $(USER_BUILD)/crt0.o \
                   --whole-archive $(LIBAURAC) --no-whole-archive

USER_CFLAGS_INC := lib/libc/include/unistd.h lib/libc/include/string.h lib/libc/include/stdio.h lib/libc/include/stdlib.h \
                   lib/libc/include/errno.h lib/libc/include/limits.h lib/libc/include/stdbool.h \
                   lib/libc/include/ctype.h lib/libc/include/math.h lib/libc/include/assert.h
# Augment include path so user apps can include "auragui.h".
USER_CFLAGS += -I lib/libauragui/include

# Application ELFs.
### RUST: add rustes.elf to the list
USER_APPS := $(USER_BUILD)/calc.elf $(USER_BUILD)/sysinfo.elf \
             $(USER_BUILD)/w32run.elf $(USER_BUILD)/sehtest.elf \
             $(USER_BUILD)/dlltest.elf $(USER_BUILD)/filesize.elf \
             $(USER_BUILD)/editor.elf $(USER_BUILD)/http.elf \
             $(USER_BUILD)/weather.elf \
             $(USER_BUILD)/trustinfo.elf \
             $(USER_BUILD)/clock.elf $(USER_BUILD)/guess.elf \
             $(USER_BUILD)/snake.elf $(USER_BUILD)/browser.elf \
             $(USER_BUILD)/selftest.elf \
             $(USER_BUILD)/proctest.elf $(USER_BUILD)/fdtest.elf \
             $(USER_BUILD)/p10test.elf $(USER_BUILD)/argv_echo.elf \
             $(USER_BUILD)/execve_child.elf \
             $(USER_BUILD)/gcalc.elf $(USER_BUILD)/gedit.elf \
             $(USER_BUILD)/gfiles.elf $(USER_BUILD)/gterm.elf \
             $(USER_BUILD)/gsysmon.elf $(USER_BUILD)/gabout.elf \
             $(USER_BUILD)/gweather.elf \
             $(USER_BUILD)/gtaskmgr.elf $(USER_BUILD)/gtheme.elf \
             $(USER_BUILD)/glaunch.elf \
             $(USER_BUILD)/apm.elf $(USER_BUILD)/matrix.elf \
             $(USER_BUILD)/life.elf $(USER_BUILD)/fetch.elf \
             $(USER_BUILD)/play.elf $(USER_BUILD)/gaudio.elf \
             $(USER_BUILD)/gusb.elf \
             $(USER_BUILD)/gbrowser.elf \
             $(USER_BUILD)/tcpserver.elf $(USER_BUILD)/elfperm.elf \
             $(USER_BUILD)/udptest.elf $(USER_BUILD)/timestest.elf \
             $(USER_BUILD)/fifolinktest.elf $(USER_BUILD)/stackguard.elf \
             $(USER_BUILD)/stoptest.elf $(USER_BUILD)/insttest.elf $(USER_BUILD)/hostilearg.elf \
             $(USER_BUILD)/socktest.elf \
             $(USER_BUILD)/tcpx5test.elf \
             $(USER_BUILD)/fpustress.elf \
             $(USER_BUILD)/siginfotest.elf \
             $(USER_BUILD)/auxvtest.elf \
             $(USER_BUILD)/fdsharetest.elf \
             $(USER_BUILD)/conformtest.elf \
             $(USER_BUILD)/ctortest.elf $(USER_BUILD)/errnotest.elf \
                $(USER_BUILD)/cryptotest.elf $(USER_BUILD)/x509test.elf \
                $(USER_BUILD)/tlstest.elf $(USER_BUILD)/httpx6.elf \
                $(USER_BUILD)/rustes.elf \
                $(USER_BUILD)/usertest.elf \
                $(USER_BUILD)/mmapshare.elf \
                $(USER_BUILD)/mmapfile.elf \
                $(USER_BUILD)/membench.elf

# auragui, linked into every GUI app.  As with libaurac, the archive is what
# the link line names; --whole-archive is not needed here because every
# auragui symbol a program uses is a genuine link-time reference.
USER_GUI_OBJ := $(LIBAURAGUI)

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
              $(USER_BUILD)/glvirgl.o $(USER_BUILD)/glfbo.o \
              $(USER_BUILD)/glsl_lex.o $(USER_BUILD)/glsl_type.o \
              $(USER_BUILD)/glsl_parse.o $(USER_BUILD)/glsl_sema.o \
              $(USER_BUILD)/glsl_exec.o \
              $(USER_BUILD)/glshader.o $(USER_BUILD)/glshaderpipe.o
USER_GL_OBJ := $(LIBAGL)
USER_CFLAGS += -I lib/libgl/include

# GL applications: linked with libgl in addition to libauragui.
USER_GL_APPS := $(USER_BUILD)/gltest.elf $(USER_BUILD)/glcube.elf \
                $(USER_BUILD)/glgears.elf $(USER_BUILD)/glrunner.elf

# ---- libatls (INTERNET_PLAN.md phase N1) ----
# The TLS crypto primitives live in userspace (decision D2): a bug in an
# ASN.1 parser must be a killed process, not a kernel panic.  Linked only
# into programs that actually use it (today: /tests/cryptotest; from N3,
# the TLS stack), never into every binary.
LIBATLS_OBJS := $(USER_BUILD)/atls_common.o $(USER_BUILD)/atls_sha256.o \
                $(USER_BUILD)/atls_sha512.o $(USER_BUILD)/atls_hmac.o \
                $(USER_BUILD)/atls_hkdf.o $(USER_BUILD)/atls_chacha20.o \
                $(USER_BUILD)/atls_poly1305.o $(USER_BUILD)/atls_aead.o \
                $(USER_BUILD)/atls_fe.o $(USER_BUILD)/atls_x25519.o \
                $(USER_BUILD)/atls_ed25519.o \
                $(USER_BUILD)/atls_der.o $(USER_BUILD)/atls_x509.o \
                $(USER_BUILD)/atls_tls_keys.o $(USER_BUILD)/atls_tls.o \
                $(USER_BUILD)/atls_rsa.o $(USER_BUILD)/atls_certval.o \
                $(USER_BUILD)/atls_ecdsa.o $(USER_BUILD)/atls_pem.o
LIBATLS      := $(USER_LIBDIR)/libatls.a
USER_CFLAGS  += -I lib/libatls/include

# ---- libahttp (INTERNET_PLAN.md phase N6) ----
# HTTP/1.1 client over plain TCP or TLS.  Linked only into apps that
# fetch URLs, never into every binary.
LIBAHTTP_OBJS := $(USER_BUILD)/ahttp.o
LIBAHTTP     := $(USER_LIBDIR)/libahttp.a
USER_CFLAGS  += -I lib/libahttp/include

# ---- Static libraries (SDK_PLAN phase S0) ----
#
# An archive is the one artefact a link command can name without knowing what
# is inside it.  Before this, linking a program meant naming 26 object files
# in the right place in build/user, which is why building an application
# outside this repository was effectively impossible.
#
# `ar rcs` is deterministic here: the member list comes from a fixed variable,
# not a wildcard, so the archive contents do not depend on what happens to be
# in the build directory.
$(LIBAURAC): $(LIBAURAC_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $(LIBAURAC_OBJS)
	@echo "[ar] $@ ($(words $(LIBAURAC_OBJS)) objects)"

$(LIBAURAGUI): $(USER_BUILD)/auragui.o
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $(USER_BUILD)/auragui.o
	@echo "[ar] $@"

$(LIBAGL): $(LIBGL_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $(LIBGL_OBJS)
	@echo "[ar] $@ ($(words $(LIBGL_OBJS)) objects)"

$(LIBATLS): $(LIBATLS_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $(LIBATLS_OBJS)
	@echo "[ar] $@ ($(words $(LIBATLS_OBJS)) objects)"

$(LIBAHTTP): $(LIBAHTTP_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $(LIBAHTTP_OBJS)
	@echo "[ar] $@"

libs: $(LIBAURAC) $(LIBAURAGUI) $(LIBAGL) $(LIBATLS) $(LIBAHTTP)

user: $(INIT_ELF) $(HELLO_ELF) $(USER_APPS) $(USER_GL_APPS)

# WIN32_PLAN.md W32-4: the w32 personality's user-space objects.  Built with
# the user CFLAGS plus -I w32/include; kernel32.c is plain freestanding C over
# the AuraLite libc, and w32_pe.c is the same parser the kernel uses.
W32_USER_OBJ := $(USER_BUILD)/w32_kernel32.o $(USER_BUILD)/w32_errno.o \
                $(USER_BUILD)/w32_handle.o  $(USER_BUILD)/w32_bind.o \
                $(USER_BUILD)/w32_peu.o     $(USER_BUILD)/w32_user32.o \
                $(USER_BUILD)/w32_crt.o     $(USER_BUILD)/w32_argv.o \
                $(USER_BUILD)/w32_module.o

$(USER_BUILD)/w32_kernel32.o: w32/src/kernel32.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@
$(USER_BUILD)/w32_errno.o: w32/src/w32_errno.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@
$(USER_BUILD)/w32_handle.o: w32/src/w32_handle.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@
$(USER_BUILD)/w32_bind.o: w32/src/w32_bind.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@
$(USER_BUILD)/w32_peu.o: w32/src/w32_pe.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@
# W32-5: USER32/GDI32 need the libauragui headers as well.
$(USER_BUILD)/w32_user32.o: w32/src/user32.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@

# W32-6: CRT startup (TLS callbacks, .CRT$XC*, setjmp-based __try/__except)
# and Win32 command-line splitting.
$(USER_BUILD)/w32_crt.o: w32/src/w32_crt.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@
$(USER_BUILD)/w32_argv.o: w32/src/w32_argv.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@
# W32-7: LoadLibrary/GetProcAddress/FreeLibrary over real DLL files.
$(USER_BUILD)/w32_module.o: w32/src/w32_module.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@

$(USER_BUILD)/w32run.o: userspace/apps/w32run/w32run.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@

$(USER_BUILD)/w32run.elf: $(USER_BUILD)/w32run.o $(W32_USER_OBJ) \
                          $(USER_COMMON) $(USER_GUI_OBJ) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/w32run.o $(W32_USER_OBJ) \
	      $(USER_COMMON_LNK) $(USER_GUI_OBJ) -o $@
	@echo "[link] $@ (w32 personality)"

# W32-6: the SEH shim exercised from native code, so a failure is the shim's
# and not the PE loader's.
$(USER_BUILD)/sehtest.o: userspace/apps/w32run/sehtest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@

$(USER_BUILD)/sehtest.elf: $(USER_BUILD)/sehtest.o $(USER_BUILD)/w32_crt.o \
                           $(USER_COMMON) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/sehtest.o $(USER_BUILD)/w32_crt.o \
	      $(USER_COMMON_LNK) -o $@
	@echo "[link] $@ (w32 SEH shim)"

# W32-7: the module layer exercised from native code.
# filesize: the read-path regression gate; see the file's own comment.
$(USER_BUILD)/filesize.o: userspace/tests/filesize/filesize.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/filesize.elf: $(USER_BUILD)/filesize.o $(USER_COMMON) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/filesize.o $(USER_COMMON_LNK) -o $@

# membench: the OPT_PLAN O0 copy-engine microbenchmark (O1's gate tooling).
$(USER_BUILD)/membench.o: userspace/tests/membench/membench.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/membench.elf: $(USER_BUILD)/membench.o $(USER_COMMON) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/membench.o $(USER_COMMON_LNK) -o $@

$(USER_BUILD)/dlltest.o: userspace/apps/w32run/dlltest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I w32/include -c $< -o $@

$(USER_BUILD)/dlltest.elf: $(USER_BUILD)/dlltest.o $(W32_USER_OBJ) \
                           $(USER_COMMON) $(USER_GUI_OBJ) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/dlltest.o $(W32_USER_OBJ) \
	      $(USER_COMMON_LNK) $(USER_GUI_OBJ) -o $@
	@echo "[link] $@ (w32 module loader)"

# Pattern rule for linking user ELFs (each links with crt0 + syscall + libc).
# GUI apps additionally link the libauragui object.
$(USER_BUILD)/%.elf: $(USER_BUILD)/%.o $(USER_COMMON) $(USER_GUI_OBJ) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/$*.o $(USER_COMMON_LNK) $(USER_GUI_OBJ) -o $@
	@echo "[link] $@"

# /http links libahttp + libatls (REALINTERNET_PLAN X2): it is the HTTPS
# client, so it needs the TLS library and the HTTP library, unlike the
# generic pattern rule above.
$(USER_BUILD)/http.elf: $(USER_BUILD)/http.o $(USER_BUILD)/ahttp.o \
                        $(USER_COMMON) $(LIBATLS) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/http.o $(USER_BUILD)/ahttp.o \
	      $(USER_COMMON_LNK) $(LIBATLS) -o $@
	@echo "[link] $@ (libahttp + libatls)"

# trustinfo — REALINTERNET_PLAN X8: print the shipped trust store's roots and
# their expiry dates.  Links libatls (the same X.509 parser the TLS stack uses).
$(USER_BUILD)/trustinfo.o: userspace/apps/trustinfo/trustinfo.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/trustinfo.elf: $(USER_BUILD)/trustinfo.o $(USER_COMMON) $(LIBATLS) \
                             lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/trustinfo.o $(USER_COMMON_LNK) \
	      $(LIBATLS) -o $@
	@echo "[link] $@ (libatls)"

# Compile rules for each application.
$(USER_BUILD)/calc.o: userspace/apps/calc/calc.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/sysinfo.o: userspace/apps/sysinfo/sysinfo.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/editor.o: userspace/apps/editor/editor.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/http.o: userspace/apps/http/http.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/weather.o: userspace/apps/weather/weather.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/tcpserver.o: userspace/tests/tcpserver/tcpserver.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/hostilearg.o: userspace/tests/hostilearg/hostilearg.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# M4 (MATURITY_PLAN.md): MAP_SHARED across fork, with MAP_PRIVATE as the
# control.  Added by AUDIT_A1 -- M4's gate had no program to run.
$(USER_BUILD)/mmapshare.o: userspace/tests/mmapshare/mmapshare.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# A6 (TESTAUDIT_PLAN.md): file-backed MAP_SHARED round trip -- store,
# msync, munmap, reopen -- with MAP_PRIVATE as the write-through control.
$(USER_BUILD)/mmapfile.o: userspace/tests/mmapfile/mmapfile.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# M3 (MATURITY_PLAN.md): fault-recovering uaccess negative test battery.
$(USER_BUILD)/usertest.o: userspace/tests/usertest/usertest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/ctortest.o: userspace/tests/ctortest/ctortest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/errnotest.o: userspace/tests/errnotest/errnotest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/stoptest.o: userspace/tests/stoptest/stoptest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/socktest.o: userspace/tests/socktest/socktest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/tcpx5test.o: userspace/tests/tcpx5test/tcpx5test.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# M1 (MATURITY_PLAN.md): FPU/SSE context-switch regression test.  -ffp-contract=off
# keeps the worker and the reference bit-comparable (no FMA fusion surprise).
$(USER_BUILD)/fpustress.o: userspace/tests/fpustress/fpustress.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -ffp-contract=off -c $< -o $@

# M5 (MATURITY_PLAN.md): SA_SIGINFO regression test.
$(USER_BUILD)/siginfotest.o: userspace/tests/siginfotest/siginfotest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# M5 (MATURITY_PLAN.md): auxiliary-vector (getauxval) test.
$(USER_BUILD)/auxvtest.o: userspace/tests/auxvtest/auxvtest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# M5 (MATURITY_PLAN.md): shared-OFD (fork/dup) + close_range gate.
$(USER_BUILD)/fdsharetest.o: userspace/tests/fdsharetest/fdsharetest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/conformtest.o: userspace/tests/conformtest/conformtest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/insttest.o: userspace/tests/insttest/insttest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

### RUST: rsbr - Rust Bridge library
RSBR_DIR := lib/rsbr
RSBR_RLIB := $(BUILD_DIR)/librsbr.rlib

$(RSBR_RLIB): $(RSBR_DIR)/common.rs
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --crate-type rlib -o $@ $<
	@echo "[rustc] $@"

### RUST: compile rustes.rs with rsbr
$(USER_BUILD)/rustes.o: userspace/apps/rustes/rustes.rs $(RSBR_RLIB)
	@mkdir -p $(dir $@)
	$(RUSTC) $(RUSTFLAGS) --emit obj --extern rsbr=$(RSBR_RLIB) -o $@ $<
	@echo "[rustc] $@"

$(USER_BUILD)/rustes.elf: $(USER_BUILD)/rustes.o $(RSBR_RLIB) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $< $(RSBR_RLIB) -o $@
	@echo "[link] $@"

# ---- cryptotest (INTERNET_PLAN.md N1): in-guest libatls smoke test ----
$(USER_BUILD)/cryptotest.o: userspace/tests/cryptotest/cryptotest.c \
                            lib/libatls/include/atls/atls.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# Explicit link rule: cryptotest additionally pulls in libatls.
$(USER_BUILD)/cryptotest.elf: $(USER_BUILD)/cryptotest.o $(USER_COMMON) \
                              $(LIBATLS) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/cryptotest.o $(USER_COMMON_LNK) \
	      $(LIBATLS) -o $@
	@echo "[link] $@ (libatls)"

# ---- x509test (INTERNET_PLAN.md N2): in-guest X.509 gate ----
# The crafted-DER builders are compiled once more, with guest flags, so the
# in-QEMU depth gate runs the identical bytes the host battery uses.
$(USER_BUILD)/x509test.o: userspace/tests/x509test/x509test.c \
                          lib/libatls/include/atls/x509.h tests/atls_test_certs.h \
                          $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/x509_testdata.o: tests/unit/atls_x509_testdata.c \
                               lib/libatls/src/atls_der.h \
                               lib/libatls/include/atls/atls.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -I lib/libatls/src -c $< -o $@

$(USER_BUILD)/x509test.elf: $(USER_BUILD)/x509test.o $(USER_BUILD)/x509_testdata.o \
                            $(USER_COMMON) $(LIBATLS) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/x509test.o $(USER_BUILD)/x509_testdata.o \
	      $(USER_COMMON_LNK) $(LIBATLS) -o $@
	@echo "[link] $@ (libatls)"
# ---- tlstest (INTERNET_PLAN.md N3): in-guest TLS 1.3 handshake gate ----
$(USER_BUILD)/tlstest.o: userspace/tests/tlstest/tlstest.c \
                         lib/libatls/include/atls/tls.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/tlstest.elf: $(USER_BUILD)/tlstest.o $(USER_COMMON) \
                           $(LIBATLS) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/tlstest.o $(USER_COMMON_LNK) \
	      $(LIBATLS) -o $@
	@echo "[link] $@ (libatls)"

# ---- httpx6 (REALINTERNET_PLAN X6): in-guest keep-alive/POST/redirect gate ----
$(USER_BUILD)/httpx6.o: userspace/tests/httpx6/httpx6.c \
                        lib/libahttp/include/ahttp/http.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/httpx6.elf: $(USER_BUILD)/httpx6.o $(USER_BUILD)/ahttp.o \
                          $(USER_COMMON) $(LIBATLS) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/httpx6.o $(USER_BUILD)/ahttp.o \
	      $(USER_COMMON_LNK) $(LIBATLS) -o $@
	@echo "[link] $@ (libahttp + libatls)"

$(USER_BUILD)/elfperm.o: userspace/tests/elfperm/elfperm.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/udptest.o: userspace/tests/udptest/udptest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/timestest.o: userspace/tests/timestest/timestest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/fifolinktest.o: userspace/tests/fifolinktest/fifolinktest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/stackguard.o: userspace/tests/stackguard/stackguard.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/clock.o: userspace/apps/clock/clock.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/guess.o: userspace/demos/guess/guess.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/snake.o: userspace/demos/snake/snake.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/browser.o: userspace/apps/browser/browser.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/selftest.o: userspace/tests/selftest/selftest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/proctest.o: userspace/tests/proctest/proctest.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/fdtest.o: userspace/tests/fdtest/fdtest.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/p10test.o: userspace/tests/p10test/p10test.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/argv_echo.o: userspace/tests/argv_echo/argv_echo.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/execve_child.o: userspace/tests/execve_child/execve_child.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/apm.o: userspace/apps/apm/apm.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/matrix.o: userspace/demos/matrix/matrix.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/life.o: userspace/demos/life/life.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/fetch.o: userspace/demos/fetch/fetch.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/play.o: userspace/apps/play/play.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- GUI applications and libauragui ----
$(USER_BUILD)/auragui.o: lib/libauragui/src/auragui.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- libgl (OpenGL) translation units -- see GL_PLAN.md ----
$(USER_BUILD)/glmath.o: lib/libgl/src/glmath.c lib/libgl/include/GL/glmath.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/auraglx.o: lib/libgl/src/auraglx.c lib/libgl/include/GL/auraglx.h \
                         lib/libgl/src/glcontext.h lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glstate.o: lib/libgl/src/glstate.c lib/libgl/src/glcontext.h \
                         lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glmatrix.o: lib/libgl/src/glmatrix.c lib/libgl/src/glcontext.h \
                          lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glimm.o: lib/libgl/src/glimm.c lib/libgl/src/glcontext.h \
                       lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glraster.o: lib/libgl/src/glraster.c lib/libgl/src/glcontext.h \
                          lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glclip.o: lib/libgl/src/glclip.c lib/libgl/src/glcontext.h \
                        lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/gllight.o: lib/libgl/src/gllight.c lib/libgl/src/glcontext.h \
                         lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/gltexture.o: lib/libgl/src/gltexture.c lib/libgl/src/glcontext.h \
                           lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glfrag.o: lib/libgl/src/glfrag.c lib/libgl/src/glcontext.h \
                        lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glarray.o: lib/libgl/src/glarray.c lib/libgl/src/glcontext.h \
                         lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/gllist.o: lib/libgl/src/gllist.c lib/libgl/src/glcontext.h \
                        lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# glu.c is built purely on the public GL API: no glcontext.h dependency.
$(USER_BUILD)/glu.o: lib/libgl/src/glu.c lib/libgl/include/GL/glu.h \
                     lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glbackend.o: lib/libgl/src/glbackend.c lib/libgl/include/GL/glbackend.h \
                           lib/libgl/src/glcontext.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# The VirGL backend shares the kernel's GPU ABI header rather than duplicating
# the struct layouts, so it needs the repository root on the include path.
$(USER_BUILD)/glvirgl.o: lib/libgl/src/glvirgl.c lib/libgl/include/GL/glbackend.h \
                         lib/libgl/src/glcontext.h kernel/gpu/gpu_syscalls.h \
                         drivers/gpu/virgl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -I . -c $< -o $@

$(USER_BUILD)/glfbo.o: lib/libgl/src/glfbo.c lib/libgl/src/glcontext.h \
                       lib/libgl/src/glvertex.h lib/libgl/include/GL/gl.h \
                       $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# The GLSL front end (phase G11a).  It depends on glsl.h and the public GL
# types only -- deliberately not on glcontext.h, so the compiler can be built
# and tested with no rendering context in sight.
$(USER_BUILD)/glsl_%.o: lib/libgl/src/glsl_%.c lib/libgl/src/glsl.h \
                        lib/libgl/include/GL/gl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# The shader object model and the pipeline seam (phase G11c).  Unlike the
# compiler these DO know about the GL context, so they depend on glcontext.h.
$(USER_BUILD)/glshader.o: lib/libgl/src/glshader.c lib/libgl/src/glcontext.h \
                          lib/libgl/src/glvertex.h lib/libgl/src/glsl.h \
                          $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glshaderpipe.o: lib/libgl/src/glshaderpipe.c lib/libgl/src/glcontext.h \
                              lib/libgl/src/glvertex.h lib/libgl/src/glsl.h \
                              $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- libahttp (N6) ----
$(USER_BUILD)/ahttp.o: lib/libahttp/src/ahttp.c lib/libahttp/include/ahttp/http.h \
                       $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- libatls objects (INTERNET_PLAN.md N1) ----
# One scoped pattern rule instead of eleven copies: every translation unit
# has the same dependency (the public header), and atls_fe.h resolves
# relative to each including file.
$(USER_BUILD)/atls_%.o: lib/libatls/src/atls_%.c \
                        lib/libatls/include/atls/atls.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# ---- GL applications ----
# /gltest reaches into lib/libgl/src for glsl.h: it is libgl's own regression
# suite, and the GLSL front end has no public entry point until phase G11c.
$(USER_BUILD)/gltest.o: userspace/tests/gltest/gltest.c lib/libauragui/include/auragui.h \
                        lib/libgl/src/glsl.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -I lib/libgl/src -c $< -o $@

$(USER_BUILD)/glcube.o: userspace/demos/glcube/glcube.c lib/libauragui/include/auragui.h \
                        lib/libgl/include/GL/gl.h lib/libgl/include/GL/auraglx.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glgears.o: userspace/demos/glgears/glgears.c lib/libauragui/include/auragui.h \
                         lib/libgl/include/GL/gl.h lib/libgl/include/GL/glu.h \
                         lib/libgl/include/GL/auraglx.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/glrunner.o: userspace/demos/glrunner/glrunner.c lib/libauragui/include/auragui.h \
                          lib/libgl/include/GL/gl.h lib/libgl/include/GL/glu.h \
                          lib/libgl/include/GL/auraglx.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# Explicit link rule: GL apps additionally pull in libgl.  This overrides the
# generic %.elf pattern rule below for these targets.
$(USER_GL_APPS): $(USER_BUILD)/%.elf: $(USER_BUILD)/%.o $(USER_COMMON) \
                                      $(USER_GUI_OBJ) $(USER_GL_OBJ) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/$*.o $(USER_COMMON_LNK) $(USER_GUI_OBJ) \
	      $(USER_GL_OBJ) -o $@
	@echo "[link] $@ (libgl)"

$(USER_BUILD)/gcalc.o:   userspace/apps/gui-calc/gcalc.c     lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gedit.o:   userspace/apps/gui-edit/gedit.c     lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gfiles.o:  userspace/apps/gui-files/gfiles.c   lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gterm.o:   userspace/apps/gui-term/gterm.c     lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gsysmon.o: userspace/apps/gui-sysmon/gsysmon.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gabout.o:  userspace/apps/gui-about/gabout.c   lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gweather.o: userspace/apps/gui-weather/gweather.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gtaskmgr.o: userspace/apps/gui-taskmgr/gtaskmgr.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/glaunch.o: userspace/apps/gui-launcher/glaunch.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gaudio.o: userspace/apps/gui-audio/gaudio.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gusb.o: userspace/apps/gui-usb/gusb.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/gbrowser.o: userspace/apps/gbrowser/gbrowser.c lib/libauragui/include/auragui.h \
                         userspace/apps/gbrowser/wv_html.h userspace/apps/gbrowser/wv_dom.h \
                         lib/libahttp/include/ahttp/http.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_html.o: userspace/apps/gbrowser/wv_html.c userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_dom.o: userspace/apps/gbrowser/wv_dom.c userspace/apps/gbrowser/wv_dom.h \
                        userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_layout.o: userspace/apps/gbrowser/wv_layout.c userspace/apps/gbrowser/wv_layout.h \
                          userspace/apps/gbrowser/wv_dom.h userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_paint.o: userspace/apps/gbrowser/wv_paint.c userspace/apps/gbrowser/wv_paint.h \
                          userspace/apps/gbrowser/wv_layout.h \
                          drivers/framebuffer/psf2_default_font.inc
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_css.o: userspace/apps/gbrowser/wv_css.c userspace/apps/gbrowser/wv_css.h \
                        userspace/apps/gbrowser/wv_dom.h
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_url.o: userspace/apps/gbrowser/wv_url.c userspace/apps/gbrowser/wv_url.h
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_http.o: userspace/apps/gbrowser/wv_http.c userspace/apps/gbrowser/wv_http.h \
                         userspace/apps/gbrowser/wv_url.h
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@
$(USER_BUILD)/wv_canvas.o: userspace/apps/gbrowser/wv_canvas.c userspace/apps/gbrowser/wv_canvas.h
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -I lib/libgl/include -c $< -o $@

# gbrowser links the tokeniser + DOM builder + layout + painter + css; the
# generic %.elf pattern rule does not know about them, so give explicit
# prerequisites.
$(USER_BUILD)/gbrowser.elf: $(USER_BUILD)/gbrowser.o $(USER_BUILD)/wv_html.o \
                           $(USER_BUILD)/wv_dom.o $(USER_BUILD)/wv_layout.o \
                           $(USER_BUILD)/wv_paint.o $(USER_BUILD)/wv_css.o \
                           $(USER_BUILD)/wv_url.o $(USER_BUILD)/wv_http.o \
                           $(USER_BUILD)/wv_canvas.o $(USER_BUILD)/ahttp.o \
                           $(USER_COMMON) $(USER_GUI_OBJ) $(USER_GL_OBJ) \
                           $(LIBATLS) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/gbrowser.o $(USER_BUILD)/wv_html.o \
	      $(USER_BUILD)/wv_dom.o $(USER_BUILD)/wv_layout.o \
	      $(USER_BUILD)/wv_paint.o $(USER_BUILD)/wv_css.o \
	      $(USER_BUILD)/wv_url.o $(USER_BUILD)/wv_http.o \
	      $(USER_BUILD)/wv_canvas.o $(USER_BUILD)/ahttp.o \
	      $(USER_COMMON_LNK) $(USER_GUI_OBJ) $(USER_GL_OBJ) $(LIBATLS) -o $@
	@echo "[link] $@ (wv_* + libgl canvas + libahttp/libatls https X6)"
$(USER_BUILD)/gtheme.o: userspace/apps/gui-theme/theme.c lib/libauragui/include/auragui.h $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@); $(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/hello.o: userspace/apps/hello/hello.c lib/libc/include/unistd.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/init.o: userspace/system/init/init.c $(USER_CFLAGS_INC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/libc.o: lib/libc/src/libc.c lib/libc/include/unistd.h lib/libc/include/string.h \
                       lib/libc/include/stdio.h lib/libc/include/stdlib.h lib/libc/include/errno.h \
                       lib/libc/include/ctype.h lib/libc/include/math.h lib/libc/include/limits.h \
                       lib/libc/include/stdbool.h lib/libc/include/assert.h lib/libc/include/signal.h \
                       lib/libc/include/sys/uio.h lib/libc/include/fcntl.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/malloc.o: lib/libc/src/malloc.c lib/libc/include/stdlib.h lib/libc/include/unistd.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# Generic rule for the extra libc translation units in lib/libc/src/*.c.
$(USER_BUILD)/%.o: lib/libc/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

# The pthread runtime lives in a sub-directory.
$(USER_BUILD)/pthread.o: lib/libc/src/pthread/pthread.c lib/libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/rwlock.o: lib/libc/src/pthread/rwlock.c lib/libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/barrier.o: lib/libc/src/pthread/barrier.c lib/libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/spin.o: lib/libc/src/pthread/spin.c lib/libc/include/pthread.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/crt0.o: lib/libc/crt/crt0.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/syscall.o: lib/libc/src/syscall.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/sigreturn.o: lib/libc/crt/sigreturn.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/setjmp.o: lib/libc/crt/setjmp.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/compat.o: lib/libc/src/compat.c lib/libc/include/strings.h lib/libc/include/wctype.h \
                         lib/libc/include/inttypes.h lib/libc/include/setjmp.h lib/libc/include/threads.h \
                         lib/libc/include/uchar.h lib/libc/include/fenv.h lib/libc/include/complex.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(INIT_ELF): $(USER_BUILD)/init.o $(USER_COMMON) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/init.o $(USER_COMMON_LNK) -o $@
	@echo "[link] $(INIT_ELF)"

$(HELLO_ELF): $(USER_BUILD)/hello.o $(USER_COMMON) lib/libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/hello.o $(USER_COMMON_LNK) -o $@
	@echo "[link] $(HELLO_ELF)"

# Embed init.elf into the kernel as a C array.
$(USER_BIN_H): $(INIT_ELF) tools/gen_user_binary.py
	@mkdir -p $(dir $@)
	python3 tools/gen_user_binary.py $(INIT_ELF) $@ init_bin

# user.c includes the generated init_bin.h; ensure it exists first.
$(BUILD_DIR)/kernel/proc/user.o: $(USER_BIN_H)

USB_IMAGE   := $(BUILD_DIR)/usb.img

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

# ---- BL5: BIOS-only ISO built with our custom bootloader.
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
iso-dual: deps-check kernel kernel32 $(BUILD_DIR)/initrd.tar $(MBR_DUAL_BIN) $(STAGE2_BIN) $(EFI_BIN)
	@bash tools/mkisoimage_dual.sh $(KERNEL_ELF) $(EFI_BIN) $(DUAL_ISO_IMAGE)

# ---- BL8: `make iso` uses the custom dual-boot loader ---------------------
.PHONY: iso
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
# ---- The runtime filesystem layout (FSLAYOUT_PLAN phase F3) ----
#
#   /bin    core system programs
#   /apps   applications
#   /demos  demonstrations
#   /tests  test programs
#   /pkg    package archives (apm's sources)
#   /opt    installed packages -- a tmpfs mount, not shipped here
#
# There are NO root-level aliases any more (phase F5).  Each program has
# exactly one location, and a command is found by name through the search
# path in lib/libc/src/progpath.c.  The hard-link support the aliases needed is
# kept in kernel/fs/initrd.c: it is tested, it costs nothing, and an image
# that wants two names for one file can still have them.
#
# INIT_BIN is deliberately NOT aliased anywhere but /bin: the bootloader path
# and the kernel's embedded copy are separate, and adding a second name for
# the one program the kernel starts by itself would invite confusion.

# name=source-basename pairs, grouped by destination directory.
INITRD_BIN   := init hello apm play sysinfo
INITRD_APPS  := calc editor http weather trustinfo clock browser w32run sehtest dlltest filesize gcalc gedit gfiles gterm \
                gsysmon gabout gweather gtaskmgr glaunch gaudio gusb gbrowser
INITRD_DEMOS := guess snake glcube glgears glrunner
INITRD_TESTS := selftest proctest fdtest p10test argv_echo execve_child \
                gltest tcpserver elfperm udptest timestest fifolinktest \
                stackguard stoptest insttest hostilearg ctortest errnotest rustes \
                socktest tcpx5test fpustress siginfotest auxvtest fdsharetest conformtest cryptotest x509test tlstest httpx6 \
                usertest mmapshare mmapfile membench

# WIN32_PLAN.md W32-3: a genuine PE32+ .exe for the kernel loader gate.
# Built with nasm -f win64 + lld-link, both already in REQUIRED_TOOLS (they
# build BOOTX64.EFI), so this adds no dependency.  Freestanding by design: it
# imports nothing, because imports are W32-4 and this gate is about the loader.
PETEST_EXE := $(BUILD_DIR)/user/petest.exe

$(PETEST_EXE): w32/tests/petest.asm
	@mkdir -p $(dir $@)
	$(AS) -f win64 $< -o $(BUILD_DIR)/user/petest.obj
	lld-link -subsystem:console -entry:start -nodefaultlib \
	         $(BUILD_DIR)/user/petest.obj -out:$@
	@echo "  [pe] $@"

# WIN32_PLAN.md W32-4: a .exe that imports KERNEL32 through a real PE import
# table.  The import library comes from a .def via lld-link; no Microsoft file
# is involved and no DLL is shipped (w32/LICENSING.md).
K32TEST_EXE := $(BUILD_DIR)/user/k32test.exe
K32_IMPLIB  := $(BUILD_DIR)/user/kernel32.lib

$(K32_IMPLIB): w32/tests/kernel32.def
	@mkdir -p $(dir $@)
	lld-link -def:$< -dll -noentry -machine:x64 \
	         -out:$(BUILD_DIR)/user/kernel32.dll -implib:$@ >/dev/null
	@echo "  [pe] $@ (import library)"

$(K32TEST_EXE): w32/tests/kernel32_test.asm $(K32_IMPLIB)
	@mkdir -p $(dir $@)
	$(AS) -f win64 $< -o $(BUILD_DIR)/user/k32test.obj
	lld-link -subsystem:console -entry:start -nodefaultlib \
	         $(BUILD_DIR)/user/k32test.obj $(K32_IMPLIB) -out:$@
	@echo "  [pe] $@ (imports KERNEL32)"

# W32-5: a .exe that creates a window through USER32/GDI32.
U32TEST_EXE := $(BUILD_DIR)/user/u32test.exe
CRTTEST_EXE := $(BUILD_DIR)/user/crttest.exe
U32_IMPLIB  := $(BUILD_DIR)/user/user32.lib
G32_IMPLIB  := $(BUILD_DIR)/user/gdi32.lib

$(U32_IMPLIB): w32/tests/user32.def
	@mkdir -p $(dir $@)
	lld-link -def:$< -dll -noentry -machine:x64 \
	         -out:$(BUILD_DIR)/user/user32.dll -implib:$@ >/dev/null
	@echo "  [pe] $@ (import library)"

$(G32_IMPLIB): w32/tests/gdi32.def
	@mkdir -p $(dir $@)
	lld-link -def:$< -dll -noentry -machine:x64 \
	         -out:$(BUILD_DIR)/user/gdi32.dll -implib:$@ >/dev/null
	@echo "  [pe] $@ (import library)"

$(U32TEST_EXE): w32/tests/user32_test.asm $(K32_IMPLIB) $(U32_IMPLIB) $(G32_IMPLIB)
	@mkdir -p $(dir $@)
	$(AS) -f win64 $< -o $(BUILD_DIR)/user/u32test.obj
	lld-link -subsystem:console -entry:start -nodefaultlib \
	         $(BUILD_DIR)/user/u32test.obj $(K32_IMPLIB) $(U32_IMPLIB) \
	         $(G32_IMPLIB) -out:$@
	@echo "  [pe] $@ (imports USER32 + GDI32)"

# W32-6: a PE with a TLS directory and a .CRT$XC* initialiser table.  It is
# linked at a fixed base because the TLS directory holds VAs, and lld-link
# populates the TLS data directory from the _tls_used symbol -- the same way
# it would for a compiler-emitted image.
$(CRTTEST_EXE): w32/tests/crt_test.asm $(K32_IMPLIB)
	@mkdir -p $(dir $@)
	$(AS) -f win64 $< -o $(BUILD_DIR)/user/crttest.obj
	lld-link -subsystem:console -entry:start -nodefaultlib -base:0x140000000 \
	         $(BUILD_DIR)/user/crttest.obj $(K32_IMPLIB) -out:$@
	@echo "  [pe] $@ (TLS callbacks + .CRT static initialisers)"

TESTDLL := $(BUILD_DIR)/user/testdll.dll

# W32-7: a real user-supplied DLL -- exports, its own KERNEL32 imports, and a
# DllMain.  Linked at a base far from where it will actually land, so the
# load path is genuinely relocating it.
$(TESTDLL): w32/tests/testdll.asm w32/tests/testdll.def $(K32_IMPLIB)
	@mkdir -p $(dir $@)
	$(AS) -f win64 $< -o $(BUILD_DIR)/user/testdll.obj
	lld-link -dll -def:w32/tests/testdll.def -entry:DllMain -nodefaultlib \
	         -machine:x64 -base:0x190000000 \
	         $(BUILD_DIR)/user/testdll.obj $(K32_IMPLIB) -out:$@
	@echo "  [pe] $@ (DLL: 3 exports, 2 imports, DllMain)"

# W32-8: the mingw-w64 examples.  Built only if the cross-compiler is
# installed -- the OS build must never require it -- but when it IS present
# the console example is copied into the initrd so the gate can run a
# genuinely compiler-emitted Win32 binary rather than only hand-written asm.
MINGW_CC := $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
W32_EXAMPLE_EXE := $(BUILD_DIR)/user/w32hello.exe
W32_UNSUP_EXE   := $(BUILD_DIR)/user/w32unsup.exe

# A stamp recording WHETHER the cross-compiler was available on the last
# build, rewritten only when that answer changes.
#
# Without it the examples go stale in a way that hides a gate.  Installing
# mingw-w64 after a build without it leaves the zero-byte placeholders newer
# than their sources, so make reports "nothing to be done", the examples are
# never built, and test_w32_integration goes on skipping -- quietly testing
# nothing.  Removing it after a build with it is the mirror image.  Making
# the examples depend on this stamp means either transition rebuilds them.
W32_MINGW_STAMP := $(BUILD_DIR)/user/.mingw-$(if $(MINGW_CC),present,absent)

$(W32_MINGW_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD_DIR)/user/.mingw-present $(BUILD_DIR)/user/.mingw-absent
	@touch $@

ifneq ($(MINGW_CC),)
$(W32_EXAMPLE_EXE): w32/examples/console-app/hello.c $(W32_MINGW_STAMP)
	@mkdir -p $(dir $@)
	$(MINGW_CC) -O2 -Wall -Wextra -m64 $< -o $@ \
	    -nostdlib -Wl,--entry=winstart -lkernel32 -luser32
	@echo "  [pe] $@ (mingw-w64 console example)"
# The deliberately unsupported example: it imports ADVAPI32, which is a
# non-goal (D8), and must be refused BY NAME at load time.
$(W32_UNSUP_EXE): w32/examples/unsupported-app/registry.c $(W32_MINGW_STAMP)
	@mkdir -p $(dir $@)
	$(MINGW_CC) -O2 -Wall -Wextra -m64 $< -o $@ \
	    -nostdlib -Wl,--entry=winstart -lkernel32 -ladvapi32
	@echo "  [pe] $@ (mingw-w64 unsupported example)"
else
# No cross-compiler: leave zero-byte placeholders so the initrd rule has its
# prerequisites, and skip the copy (the `if [ -s ... ]` guards below).
$(W32_EXAMPLE_EXE) $(W32_UNSUP_EXE): $(W32_MINGW_STAMP)
	@mkdir -p $(dir $@)
	@echo "  [pe] skipping the mingw-w64 examples (no x86_64-w64-mingw32-gcc)"
	@: > $@
endif

PETEST_RELOC_EXE := $(BUILD_DIR)/user/petest_reloc.exe

$(PETEST_RELOC_EXE): $(BUILD_DIR)/user/petest.obj
	lld-link -subsystem:console -entry:start -nodefaultlib \
	         -base:0x800000000000 $< -out:$@
	@echo "  [pe] $@ (linked above USER_VADDR_TOP; forces relocation)"

$(BUILD_DIR)/user/petest.obj: w32/tests/petest.asm
	@mkdir -p $(dir $@)
	$(AS) -f win64 $< -o $@

.PHONY: petest
petest: $(PETEST_EXE) $(PETEST_RELOC_EXE)

$(BUILD_DIR)/initrd.tar: $(INIT_ELF) $(HELLO_ELF) $(USER_APPS) $(USER_GL_APPS) $(PETEST_EXE) $(PETEST_RELOC_EXE) $(K32TEST_EXE) $(U32TEST_EXE) $(CRTTEST_EXE) $(TESTDLL) $(W32_EXAMPLE_EXE) $(W32_UNSUP_EXE) $(INIT32_ELF) $(SHELL32_ELF) $(INITRV_ELF) $(SHELLRV_ELF)
	@rm -rf $(INITRD_DIR)
	@mkdir -p $(INITRD_DIR)/bin $(INITRD_DIR)/apps $(INITRD_DIR)/demos \
	          $(INITRD_DIR)/tests $(INITRD_DIR)/pkg $(INITRD_DIR)/etc
# Binaries are stripped into the image: the BIOS boot path reserves a 16 MiB
# slot for initrd.tar (see mkisoimage_dual.sh) and the full userland with
# symbol tables no longer fits.  Unstripped ELFs stay in build/user for
# debugging; nothing in the OS reads user-space symtabs at runtime.
	@strip -s $(INIT_ELF) -o $(INITRD_DIR)/bin/init
	@strip -s $(HELLO_ELF) -o $(INITRD_DIR)/bin/hello
	@for p in apm play sysinfo; do \
	    strip -s $(USER_BUILD)/$$p.elf -o $(INITRD_DIR)/bin/$$p; done
	@for p in $(INITRD_APPS); do \
	    strip -s $(USER_BUILD)/$$p.elf -o $(INITRD_DIR)/apps/$$p; done
	@for p in $(INITRD_DEMOS); do \
	    strip -s $(USER_BUILD)/$$p.elf -o $(INITRD_DIR)/demos/$$p; done
	@for p in $(INITRD_TESTS); do \
	    strip -s $(USER_BUILD)/$$p.elf -o $(INITRD_DIR)/tests/$$p; done
# Package archives apm installs from (SDK_PLAN phase S4).
#
# These used to be `cp foo.elf foo.pkg` -- a renamed executable with no
# metadata, so apm could not report what it was about to install nor detect a
# truncated file.  They are real .apkg packages now: a short textual header
# with name, version, description, size and CRC-32, followed by the ELF.
	@$(MAKE) --no-print-directory $(MKAPKG) >/dev/null
	@$(MKAPKG) -n matrix -v 1.0 -d "Matrix digital rain screen simulation" \
	           -o $(INITRD_DIR)/pkg/matrix.apkg $(USER_BUILD)/matrix.elf >/dev/null
	@$(MKAPKG) -n life -v 1.2 -d "Conway's Game of Life simulation" \
	           -o $(INITRD_DIR)/pkg/life.apkg $(USER_BUILD)/life.elf >/dev/null
	@$(MKAPKG) -n fetch -v 2.1 -d "System information fetch utility" \
	           -o $(INITRD_DIR)/pkg/fetch.apkg $(USER_BUILD)/fetch.elf >/dev/null
# A deliberately corrupted package, so the integration suite can prove a bad
# checksum is REFUSED and leaves nothing behind.  Testing only the happy path
# would leave the verification untested, which is the half that matters.
	@cp $(INITRD_DIR)/pkg/fetch.apkg $(INITRD_DIR)/pkg/broken.apkg
	@printf 'X' | dd of=$(INITRD_DIR)/pkg/broken.apkg bs=1 seek=200 conv=notrunc \
	              status=none
# The PE32+ loader fixture (W32-3).  Not stripped: `strip` does not handle PE,
# and at 3 KiB it costs nothing in the 16 MiB initrd slot.
	@cp $(PETEST_EXE) $(INITRD_DIR)/tests/petest.exe
# The same image with its subsystem field forced to EFI_APPLICATION (10), so
# the integration test can prove a firmware binary is REFUSED rather than run.
# Patched here rather than assembled separately: one byte apart from the good
# image is what makes the negative result attributable.
	@python3 tools/mk_pe_efi_variant.py $(PETEST_EXE) \
	         $(INITRD_DIR)/tests/petest_efi.exe
# The same program LINKED at a base the loader cannot honour, so the base
# relocation path is exercised.  0x800000000000 is above USER_VADDR_TOP
# (0x800000000000), so the preferred base is always rejected and the image is
# moved to PE_FALLBACK_BASE.  It must still print the identical marker, which
# is only possible if the .data pointer was fixed up correctly.
	@cp $(PETEST_RELOC_EXE) $(INITRD_DIR)/tests/petest_reloc.exe
	@cp $(K32TEST_EXE) $(INITRD_DIR)/tests/k32test.exe
	@cp $(U32TEST_EXE) $(INITRD_DIR)/tests/u32test.exe
	@cp $(CRTTEST_EXE) $(INITRD_DIR)/tests/crttest.exe
	@cp $(TESTDLL) $(INITRD_DIR)/tests/testdll.dll
	@if [ -s $(W32_EXAMPLE_EXE) ]; then \
	    cp $(W32_EXAMPLE_EXE) $(INITRD_DIR)/tests/w32hello.exe; fi
	@if [ -s $(W32_UNSUP_EXE) ]; then \
	    cp $(W32_UNSUP_EXE) $(INITRD_DIR)/tests/w32unsup.exe; fi
# W32-7 hostile fixtures: a forwarder export, and a DllMain that fails.
	@python3 tools/mk_dll_variants.py --forwarder $(TESTDLL) \
	         $(INITRD_DIR)/tests/fwddll.dll
	@python3 tools/mk_dll_variants.py --dllmain-fails $(TESTDLL) \
	         $(INITRD_DIR)/tests/baddll.dll
# The same image with its first TLS callback pointed outside the image
# (W32-6 hostile gate): the loader must refuse to call through it.
	@python3 tools/mk_pe_badtls.py $(CRTTEST_EXE) \
	         $(INITRD_DIR)/tests/crtbad.exe
# The same image with its WNDPROC pointed outside the image (W32-5 hostile
# gate): dispatching to it must kill only that process, and the compositor
# must reap the window it had already created.
	@python3 tools/mk_pe_badwndproc.py $(U32TEST_EXE) \
	         $(INITRD_DIR)/tests/u32bad.exe
	@printf 'AuraLite OS\nfilesystem layout: see docs/filesystem.md\n' \
	    > $(INITRD_DIR)/etc/motd
# I386_PLAN I5: the 32-bit userland, in the SAME archive under /bin32.
# One initrd serves both kernels; each kernel's ELF loader refuses the
# other's class, and the path split means neither can even try.
	@mkdir -p $(INITRD_DIR)/bin32
	@strip -s $(INIT32_ELF) -o $(INITRD_DIR)/bin32/init32
	@strip -s $(SHELL32_ELF) -o $(INITRD_DIR)/bin32/shell32
# RISCV_PLAN V5: the rv64 userland, THIRD tenant under /binrv.  GNU
# strip does not speak EM_RISCV; llvm-strip is part of the clang
# toolchain the build already requires.
	@mkdir -p $(INITRD_DIR)/binrv
	@llvm-strip-19 -s $(INITRV_ELF) -o $(INITRD_DIR)/binrv/init 2>/dev/null || \
	    llvm-strip -s $(INITRV_ELF) -o $(INITRD_DIR)/binrv/init
	@llvm-strip-19 -s $(SHELLRV_ELF) -o $(INITRD_DIR)/binrv/smallsh 2>/dev/null || \
	    llvm-strip -s $(SHELLRV_ELF) -o $(INITRD_DIR)/binrv/smallsh
# Pinned trust store (REALINTERNET_PLAN X2): shipped in the image so the
# HTTPS client can validate server chains against it.
	@mkdir -p $(INITRD_DIR)/etc/ssl
	@cp etc/ssl/roots.pem $(INITRD_DIR)/etc/ssl/roots.pem
# SDK examples (SDK_PLAN S2).  Built from the STAGED SDK, never from the
# source tree, and shipped so that test_sdk_examples.sh can run them.  If the
# SDK stops being sufficient to build an application, the image build fails
# here rather than a user discovering it.
	@$(MAKE) --no-print-directory sdk >/dev/null
	@for ex in hello-app gui-app; do \
	    $(MAKE) --no-print-directory -C examples/$$ex \
	            AURALITE_SDK=$(CURDIR)/$(SDK_DIR) >/dev/null; \
	    strip -s examples/$$ex/$$ex.elf -o $(INITRD_DIR)/apps/$$ex; \
	done
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
                $(BUILD_DIR)/test_gltex $(BUILD_DIR)/test_gltex2 \
                $(BUILD_DIR)/test_glarray \
                $(BUILD_DIR)/test_glu $(BUILD_DIR)/test_glbackend \
                $(BUILD_DIR)/test_glfbo $(BUILD_DIR)/test_glsl \
                $(BUILD_DIR)/test_glslexec $(BUILD_DIR)/test_glprog \
                $(BUILD_DIR)/test_glcoexist $(BUILD_DIR)/test_glvirgl \
                $(BUILD_DIR)/test_gpu_syscall \
                $(BUILD_DIR)/test_initrd_dirs \
                $(BUILD_DIR)/test_initrd_allocfail \
                $(BUILD_DIR)/test_execpolicy \
                $(BUILD_DIR)/test_progpath \
                $(BUILD_DIR)/test_apkg \
                $(BUILD_DIR)/test_printf_fmt \
                $(BUILD_DIR)/test_pmm $(BUILD_DIR)/test_heap \
                $(BUILD_DIR)/test_string $(BUILD_DIR)/test_string_ops \
                $(BUILD_DIR)/test_uart_ring $(BUILD_DIR)/test_tlb_policy \
                $(BUILD_DIR)/test_bitmap \
                $(BUILD_DIR)/test_net $(BUILD_DIR)/test_kprintf \
                $(BUILD_DIR)/test_libc $(BUILD_DIR)/test_3d \
                $(BUILD_DIR)/test_usb $(BUILD_DIR)/test_wm \
                $(BUILD_DIR)/test_usb_audio $(BUILD_DIR)/test_usb_cdc \
                $(BUILD_DIR)/test_usb_full $(BUILD_DIR)/test_usb_hub \
                $(BUILD_DIR)/test_usb_isoc \
                $(BUILD_DIR)/test_vfs $(BUILD_DIR)/test_network \
                $(BUILD_DIR)/test_dns \
                $(BUILD_DIR)/test_tcp_x5 \
                $(BUILD_DIR)/test_tcp_m6 \
                $(BUILD_DIR)/test_tcp_m6c \
                $(BUILD_DIR)/test_tcp_m6d \
                $(BUILD_DIR)/test_tcp_m6e \
                $(BUILD_DIR)/test_ip_reasm \
                $(BUILD_DIR)/test_xhci_ring \
                $(BUILD_DIR)/test_ipv6_addr \
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
                $(BUILD_DIR)/test_stdio_seek \
                $(BUILD_DIR)/test_printf_format \
                $(BUILD_DIR)/test_stdio_ext \
                $(BUILD_DIR)/test_stdlib_ext \
                $(BUILD_DIR)/test_q11_new \
                $(BUILD_DIR)/test_posix_spawn \
                $(BUILD_DIR)/test_q10_stubs \
                $(BUILD_DIR)/test_ipc \
                $(BUILD_DIR)/test_mq_notify \
                $(BUILD_DIR)/test_q16_tail \
                $(BUILD_DIR)/test_sysvipc \
                $(BUILD_DIR)/test_keymap \
                $(BUILD_DIR)/test_rng \
                $(BUILD_DIR)/test_atls_hash $(BUILD_DIR)/test_atls_aead \
                $(BUILD_DIR)/test_atls_x25519 $(BUILD_DIR)/test_atls_ed25519 \
                $(BUILD_DIR)/test_atls_x509 \
                $(BUILD_DIR)/test_atls_tls \
                $(BUILD_DIR)/test_atls_certval \
                $(BUILD_DIR)/test_atls_ecdsa \
                $(BUILD_DIR)/test_atls_pem \
                $(BUILD_DIR)/test_ahttp_https \
                $(BUILD_DIR)/test_ahttp \
                $(BUILD_DIR)/test_wv_html \
                $(BUILD_DIR)/test_wv_dom \
                $(BUILD_DIR)/test_wv_layout \
                $(BUILD_DIR)/test_wv_paint \
                $(BUILD_DIR)/test_wv_css \
                $(BUILD_DIR)/test_wv_http \
                $(BUILD_DIR)/test_wv_canvas \
                $(BUILD_DIR)/test_uaccess \
                $(BUILD_DIR)/test_ahci_serialisation \
                $(BUILD_DIR)/test_vma_m4 \
                $(BUILD_DIR)/test_w32_utf \
                $(BUILD_DIR)/test_w32_pe \
                $(BUILD_DIR)/test_w32_abi \
                $(BUILD_DIR)/test_w32_kernel32 \
                $(BUILD_DIR)/test_w32_argv \
                $(BUILD_DIR)/test_w32_exports

# WIN32_PLAN.md phases W32-1/W32-2: the w32 personality's host-side gates.
# Both test files #include the implementation directly, so there is no w32
# archive to build first and these stay pure host tests.
W32_INC := -I w32/include

$(BUILD_DIR)/test_w32_utf: tests/unit/test_w32_utf.c w32/src/w32_utf.c \
                           w32/include/w32/w32_utf.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 $(W32_INC) -I . $< -o $@

$(BUILD_DIR)/test_w32_pe: tests/unit/test_w32_pe.c w32/src/w32_pe.c \
                          w32/include/w32/w32_pe.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 $(W32_INC) -I . $< -o $@

# The ABI test is NOT built with sanitizers: ASan's prologue assumes a System V
# frame and faults inside an ms_abi callee entered from the hand-written
# Windows-ABI caller.  See the note in the test.  It is verified at -O0..-O3.
$(BUILD_DIR)/test_w32_abi: tests/unit/test_w32_abi.c \
                           w32/include/w32/w32_abi.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 $(W32_INC) -I . $< -o $@

$(BUILD_DIR)/test_w32_kernel32: tests/unit/test_w32_kernel32.c \
                                w32/src/kernel32.c w32/src/w32_handle.c \
                                w32/src/w32_errno.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 $(W32_INC) -I . $< -o $@

# W32-6: command-line splitting.  Pure string handling with no syscalls, so
# it runs under the sanitizers -- which is where an off-by-one in the
# backslash-run logic shows up as a real overflow rather than a wrong string.
$(BUILD_DIR)/test_w32_argv: tests/unit/test_w32_argv.c w32/src/w32_argv.c \
                            w32/include/w32/w32_argv.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -g \
	          -fsanitize=address,undefined $(W32_INC) -I . $< -o $@

# W32-7: the export directory -- the structure GetProcAddress walks, and one
# whose name-ordinal array indexes another array with a value out of the file.
# Under ASan, because "refused" and "read out of bounds first" look the same
# otherwise.
$(BUILD_DIR)/test_w32_exports: tests/unit/test_w32_exports.c w32/src/w32_pe.c \
                               w32/include/w32/w32_pe.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -g \
	          -fsanitize=address,undefined $(W32_INC) -I . \
	          tests/unit/test_w32_exports.c w32/src/w32_pe.c -o $@

# Host tool: dump a PE image (WIN32_PLAN.md W32-2).  Also the fixture for the
# llvm-readobj cross-check gate below.
.PHONY: w32-peinfo
w32-peinfo: $(BUILD_DIR)/w32_peinfo
$(BUILD_DIR)/w32_peinfo: w32/tools/peinfo.c w32/src/w32_pe.c \
                         w32/include/w32/w32_pe.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 $(W32_INC) \
	    w32/tools/peinfo.c w32/src/w32_pe.c -o $@

test-unit: $(UNIT_TESTS) $(BUILD_DIR)/w32_peinfo
	@for t in $(UNIT_TESTS); do echo "[unit] running $$t"; ./$$t || exit 1; done
# Shell-based unit tests.  test_userlibs inspects the built archives rather
# than compiled code, so it is a script rather than a C binary and cannot join
# $(UNIT_TESTS), which is a list of executables to build.  It skips cleanly
# when the archives have not been built.
	@echo "[unit] running tests/unit/test_userlibs.sh"
	@bash tests/unit/test_userlibs.sh || exit 1

# WIN32_PLAN.md W32-0: provenance/licensing enforcement, plus its negative
# control -- a checker that never fails is indistinguishable from a clean tree.
	@echo "[unit] running tools/check_provenance.sh"
	@bash tools/check_provenance.sh || exit 1
	@bash tools/check_provenance.sh --selftest || exit 1
# W32-8: docs/win32.md's function table is generated from the export table,
# so it cannot drift.  Same idea as sdk-check: a hand-maintained API list is
# wrong the moment somebody adds an export, and nobody notices.
	@echo "[unit] running tools/gen_w32_api_table.py --check"
	@python3 tools/gen_w32_api_table.py --check || exit 1
# W32-4: prove the ABI test would fail without ms_abi.
	@echo "[unit] running tests/unit/test_w32_abi_negctl.sh"
	@bash tests/unit/test_w32_abi_negctl.sh || exit 1
# WIN32_PLAN.md W32-2: cross-check peinfo against llvm-readobj on the
# project's own BOOTX64.EFI.  Skips cleanly if either is unavailable.
	@echo "[unit] running tests/unit/test_w32_peinfo.sh"
	@bash tests/unit/test_w32_peinfo.sh || exit 1

# I386_PLAN I6: the pointer-width sweep gates — three ratchets
# ((uint64_t) casts in portable code, direct x86_64 includes, cross-arch
# includes), the checker's own self-test, the cross-width boot_info_t
# offset contract at both targets, and the negative control that
# re-detects the I1 -malign-double ABI bug.
	@echo "[unit] running tests/unit/test_width_sweep.sh"
	@bash tests/unit/test_width_sweep.sh || exit 1

# I386_PLAN I8: the crypto stack's RFC vectors at 32-bit width (the
# symmetric subset; the __int128 boundary in atls_fe.c is measured and
# recorded, not hidden).  Skips cleanly without gcc-multilib.
	@echo "[unit] running tests/unit/test_libatls_m32.sh"
	@bash tests/unit/test_libatls_m32.sh || exit 1
	@echo "[unit] running tests/unit/test_libatls_rv64.sh"
	@bash tests/unit/test_libatls_rv64.sh || exit 1

# I386_PLAN I9: the plan cannot drift from the tree -- each phase's
# claims are tied to artefacts that only exist if the phase happened,
# and the header is checked against the phase table.  With the usual
# negative control (a checker that never fails checks nothing).
	@echo "[unit] running tools/check_i386_claims.py"
	@python3 tools/check_i386_claims.py || exit 1
	@python3 tools/check_i386_claims.py --selftest || exit 1
	@echo "[unit] running tools/check_riscv_claims.py"
	@python3 tools/check_riscv_claims.py || exit 1
	@python3 tools/check_riscv_claims.py --selftest || exit 1
	@echo "[unit] running tools/check_arm64_claims.py"
	@python3 tools/check_arm64_claims.py || exit 1
	@python3 tools/check_arm64_claims.py --selftest || exit 1

# Q12 (POSIX2024_PLAN.md): the POSIX.1-2024 conformance harness, host layer —
# header self-containment sweep, matrix->archive drift check, negative
# control, and the Q-family unit sub-suites.  Skips cleanly when the libc
# archives have not been built (same convention as test_userlibs.sh above).
	@echo "[unit] running tests/posix2024/run_host.sh"
	@bash tests/posix2024/run_host.sh || exit 1

$(BUILD_DIR)/test_pmm: tests/unit/test_pmm.c kernel/lib/bitmap.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# N0 (INTERNET_PLAN.md): the ChaCha20 CSPRNG core, RFC 8439 vectors + stats.
$(BUILD_DIR)/test_rng: tests/unit/test_rng.c kernel/rng_core.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- libatls host unit tests (INTERNET_PLAN.md N1) ----
# One shared source list so every RFC-vector battery always links the SAME
# library the guest ships — the GL tests learned this the hard way
# (LIBGL_TEST_SRCS comment): a per-rule copy drifts, a single variable
# cannot.  Note: test_atls_hash additionally greps these sources for the
# D7 rule (no memcmp on secrets), so the list must stay explicit.
LIBATLS_SRCS := lib/libatls/src/atls_common.c lib/libatls/src/atls_sha256.c \
                lib/libatls/src/atls_sha512.c lib/libatls/src/atls_hmac.c \
                lib/libatls/src/atls_hkdf.c lib/libatls/src/atls_chacha20.c \
                lib/libatls/src/atls_poly1305.c lib/libatls/src/atls_aead.c \
                lib/libatls/src/atls_fe.c lib/libatls/src/atls_x25519.c \
                lib/libatls/src/atls_ed25519.c \
                lib/libatls/src/atls_der.c lib/libatls/src/atls_x509.c \
                lib/libatls/src/atls_tls_keys.c lib/libatls/src/atls_tls.c \
                lib/libatls/src/atls_rsa.c lib/libatls/src/atls_certval.c \
                lib/libatls/src/atls_ecdsa.c lib/libatls/src/atls_pem.c
LIBATLS_TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -I lib/libatls/include

$(BUILD_DIR)/test_atls_hash: tests/unit/test_atls_hash.c $(LIBATLS_SRCS) \
                             lib/libatls/include/atls/atls.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBATLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

$(BUILD_DIR)/test_atls_aead: tests/unit/test_atls_aead.c $(LIBATLS_SRCS) \
                             lib/libatls/include/atls/atls.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBATLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

$(BUILD_DIR)/test_atls_x25519: tests/unit/test_atls_x25519.c $(LIBATLS_SRCS) \
                               lib/libatls/include/atls/atls.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBATLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

$(BUILD_DIR)/test_atls_ed25519: tests/unit/test_atls_ed25519.c $(LIBATLS_SRCS) \
                                lib/libatls/include/atls/atls.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBATLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

# X.509 (N2): additionally links the crafted-DER builders and needs the
# internal atls_der.h on the include path (testdata drives the skipper).
# -I . lets it reach tests/atls_test_certs.h, shared with the guest test.
# TLS 1.3 handshake (N3): links the REAL libatls sources plus the internal
# atls_tls_int.h header, and exercises the full handshake against a local
# openssl s_server with an Ed25519 cert.
TLS_TEST_CFLAGS := $(LIBATLS_TEST_CFLAGS) -I lib/libatls/src
TLS_TEST_DEPS   := $(LIBATLS_SRCS) lib/libatls/include/atls/tls.h \
                    lib/libatls/src/atls_tls_int.h

$(BUILD_DIR)/test_atls_tls: tests/unit/test_atls_tls.c $(TLS_TEST_DEPS)
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(TLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

# Certificate validation (N5): links libatls + atls_rsa + atls_certval.
$(BUILD_DIR)/test_atls_certval: tests/unit/test_atls_certval.c $(LIBATLS_SRCS) \
                                lib/libatls/include/atls/certval.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(TLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

# ECDSA P-256 (REALINTERNET_PLAN X1): direct verify against the embedded
# openssl-derived vector plus the negative cases.  Links the REAL libatls
# sources, as every other atls host test does.
$(BUILD_DIR)/test_atls_ecdsa: tests/unit/test_atls_ecdsa.c $(LIBATLS_SRCS) \
                              lib/libatls/include/atls/ecdsa.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(TLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

# PEM trust-store decoding (REALINTERNET_PLAN X2).
$(BUILD_DIR)/test_atls_pem: tests/unit/test_atls_pem.c $(LIBATLS_SRCS) \
                            lib/libatls/include/atls/pem.h etc/ssl/roots.pem
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(TLS_TEST_CFLAGS) $(LIBATLS_SRCS) $< -o $@

# HTTP client (N6): URL parsing tests.
$(BUILD_DIR)/test_ahttp: tests/unit/test_ahttp.c lib/libahttp/src/ahttp.c \
                         lib/libahttp/include/ahttp/http.h \
                         $(LIBATLS_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 \
	           -I lib/libahttp/include -I lib/libatls/include -I lib/libatls/src \
	           lib/libahttp/src/ahttp.c $(LIBATLS_SRCS) $< -o $@

# HTTPS client end-to-end (REALINTERNET_PLAN X2): real libahttp + libatls
# against a local openssl s_server, with chain validation on and off.
$(BUILD_DIR)/test_ahttp_https: tests/unit/test_ahttp_https.c \
                               lib/libahttp/src/ahttp.c \
                               lib/libahttp/include/ahttp/http.h $(LIBATLS_SRCS)
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 \
	           -I lib/libahttp/include -I lib/libatls/include -I lib/libatls/src \
	           lib/libahttp/src/ahttp.c $(LIBATLS_SRCS) $< -o $@

# Web view HTML tokeniser (WEBVIEW_PLAN W1): the REAL userspace source is
# compiled into the host test, never a copy.
$(BUILD_DIR)/test_wv_html: tests/unit/test_wv_html.c \
                           userspace/apps/gbrowser/wv_html.c \
                           userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	           tests/unit/test_wv_html.c userspace/apps/gbrowser/wv_html.c -o $@

# Web view DOM builder (WEBVIEW_PLAN W2): the REAL userspace sources are
# compiled into the host test, never copies.
$(BUILD_DIR)/test_wv_dom: tests/unit/test_wv_dom.c \
                          userspace/apps/gbrowser/wv_dom.c \
                          userspace/apps/gbrowser/wv_dom.h \
                          userspace/apps/gbrowser/wv_html.c \
                          userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	           tests/unit/test_wv_dom.c userspace/apps/gbrowser/wv_dom.c \
	           userspace/apps/gbrowser/wv_html.c -o $@

# Web view block layout (WEBVIEW_PLAN W3): the REAL userspace sources are
# compiled into the host test, never copies.
$(BUILD_DIR)/test_wv_layout: tests/unit/test_wv_layout.c \
                             userspace/apps/gbrowser/wv_layout.c \
                             userspace/apps/gbrowser/wv_layout.h \
                             userspace/apps/gbrowser/wv_css.c \
                             userspace/apps/gbrowser/wv_css.h \
                             userspace/apps/gbrowser/wv_dom.c \
                             userspace/apps/gbrowser/wv_dom.h \
                             userspace/apps/gbrowser/wv_html.c \
                             userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	           tests/unit/test_wv_layout.c userspace/apps/gbrowser/wv_layout.c \
	           userspace/apps/gbrowser/wv_css.c userspace/apps/gbrowser/wv_dom.c \
	           userspace/apps/gbrowser/wv_html.c -o $@

# Web view URL + HTTP (WEBVIEW_PLAN W6): the REAL userspace sources are
# compiled into the host test, never copies.
$(BUILD_DIR)/test_wv_http: tests/unit/test_wv_http.c \
                           userspace/apps/gbrowser/wv_url.c \
                           userspace/apps/gbrowser/wv_url.h \
                           userspace/apps/gbrowser/wv_http.c \
                           userspace/apps/gbrowser/wv_http.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	           tests/unit/test_wv_http.c userspace/apps/gbrowser/wv_url.c \
	           userspace/apps/gbrowser/wv_http.c -o $@

# Web view <canvas> renderer (WEBVIEW_PLAN W7): the REAL wv_canvas.c
# links against the REAL libgl sources (LIBGL_TEST_SRCS + the auragui
# stub), exactly like the GL phase tests.
$(BUILD_DIR)/test_wv_canvas: tests/unit/test_wv_canvas.c \
                             userspace/apps/gbrowser/wv_canvas.c \
                             userspace/apps/gbrowser/wv_canvas.h \
                             $(LIBGL_TEST_SRCS) $(LIBGL_TEST_STUB)
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBGL_TEST_CFLAGS) -I userspace/apps/gbrowser \
	           tests/unit/test_wv_canvas.c userspace/apps/gbrowser/wv_canvas.c \
	           $(LIBGL_TEST_SRCS) $(LIBGL_TEST_STUB) -o $@ -lm

# M3 (MATURITY_PLAN.md): fault-recovering uaccess validation layer.
$(BUILD_DIR)/test_uaccess: tests/unit/test_uaccess.c kernel/proc/usercopy.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# A2-R1: the AHCI command slot and DMA bounce buffer are per-port shared
# state and were unlocked.  Models the race; the control must corrupt.
$(BUILD_DIR)/test_ahci_serialisation: tests/unit/test_ahci_serialisation.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -pthread -I . $< -o $@

# M4 (MATURITY_PLAN.md): demand-paged and shared VMA extensions.
$(BUILD_DIR)/test_vma_m4: tests/unit/test_vma_m4.c kernel/mm/vma.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# Web view inline CSS (WEBVIEW_PLAN W5): the REAL userspace sources are
# compiled into the host test, never copies.
$(BUILD_DIR)/test_wv_css: tests/unit/test_wv_css.c \
                          userspace/apps/gbrowser/wv_css.c \
                          userspace/apps/gbrowser/wv_css.h \
                          userspace/apps/gbrowser/wv_layout.c \
                          userspace/apps/gbrowser/wv_layout.h \
                          userspace/apps/gbrowser/wv_paint.c \
                          userspace/apps/gbrowser/wv_paint.h \
                          userspace/apps/gbrowser/wv_dom.c \
                          userspace/apps/gbrowser/wv_dom.h \
                          userspace/apps/gbrowser/wv_html.c \
                          userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	           tests/unit/test_wv_css.c userspace/apps/gbrowser/wv_css.c \
	           userspace/apps/gbrowser/wv_layout.c userspace/apps/gbrowser/wv_paint.c \
	           userspace/apps/gbrowser/wv_dom.c \
	           userspace/apps/gbrowser/wv_html.c -o $@

# Web view painter (WEBVIEW_PLAN W4): the REAL userspace sources are
# compiled into the host test, never copies.
$(BUILD_DIR)/test_wv_paint: tests/unit/test_wv_paint.c \
                            userspace/apps/gbrowser/wv_paint.c \
                            userspace/apps/gbrowser/wv_paint.h \
                            userspace/apps/gbrowser/wv_layout.c \
                            userspace/apps/gbrowser/wv_layout.h \
                            userspace/apps/gbrowser/wv_css.c \
                            userspace/apps/gbrowser/wv_css.h \
                            userspace/apps/gbrowser/wv_dom.c \
                            userspace/apps/gbrowser/wv_dom.h \
                            userspace/apps/gbrowser/wv_html.c \
                            userspace/apps/gbrowser/wv_html.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	           tests/unit/test_wv_paint.c userspace/apps/gbrowser/wv_paint.c \
	           userspace/apps/gbrowser/wv_layout.c userspace/apps/gbrowser/wv_css.c \
	           userspace/apps/gbrowser/wv_dom.c \
	           userspace/apps/gbrowser/wv_html.c -o $@

$(BUILD_DIR)/test_atls_x509: tests/unit/test_atls_x509.c \
                             tests/unit/atls_x509_testdata.c $(LIBATLS_SRCS) \
                             lib/libatls/include/atls/x509.h tests/atls_test_certs.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBATLS_TEST_CFLAGS) -I lib/libatls/src -I . \
	           $(LIBATLS_SRCS) tests/unit/atls_x509_testdata.c \
	           tests/unit/test_atls_x509.c -o $@

$(BUILD_DIR)/test_heap: tests/unit/test_heap.c kernel/mm/heap.c kernel/mm/heap.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . tests/unit/test_heap.c kernel/mm/heap.c -o $@

$(BUILD_DIR)/test_string: tests/unit/test_string.c kernel/lib/string.c kernel/lib/string.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# OPT_PLAN O1: the fast rep-string backend + word-wide memcmp/strlen,
# across the alignment × size × overlap matrix.
$(BUILD_DIR)/test_string_ops: tests/unit/test_string_ops.c \
                              kernel/arch/x86_64/string_fast.c \
                              kernel/lib/string.c kernel/lib/string.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# OPT_PLAN O3: the UART TX ring index core — wrap, full, empty, and the
# 2^32 counter crossing.
$(BUILD_DIR)/test_uart_ring: tests/unit/test_uart_ring.c drivers/uart/uart_ring.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# OPT_PLAN O5: the TLB shootdown decision core — seq gaps, npages
# boundaries, and the sender-side skip filter.
$(BUILD_DIR)/test_tlb_policy: tests/unit/test_tlb_policy.c kernel/arch/x86_64/tlb_policy.h
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
LIBGL_TEST_SRCS := lib/libgl/src/auraglx.c lib/libgl/src/glstate.c \
                   lib/libgl/src/glmath.c lib/libgl/src/glmatrix.c \
                   lib/libgl/src/glimm.c lib/libgl/src/glraster.c \
                   lib/libgl/src/glclip.c lib/libgl/src/gllight.c \
                   lib/libgl/src/gltexture.c lib/libgl/src/glfrag.c \
                   lib/libgl/src/glarray.c lib/libgl/src/gllist.c \
                   lib/libgl/src/glu.c lib/libgl/src/glbackend.c \
                   lib/libgl/src/glvirgl.c lib/libgl/src/glfbo.c \
                   lib/libgl/src/glsl_lex.c lib/libgl/src/glsl_type.c \
                   lib/libgl/src/glsl_parse.c lib/libgl/src/glsl_sema.c \
                   lib/libgl/src/glsl_exec.c \
                   lib/libgl/src/glshader.c lib/libgl/src/glshaderpipe.c

LIBGL_TEST_HDRS := lib/libgl/src/glcontext.h lib/libgl/src/glvertex.h \
                   lib/libgl/src/glsl.h \
                   lib/libgl/include/GL/glu.h lib/libgl/include/GL/glbackend.h \
                   lib/libgl/include/GL/gl.h lib/libgl/include/GL/glmath.h \
                   lib/libgl/include/GL/auraglx.h

LIBGL_TEST_STUB := tests/unit/glstub/auragui_stub.c
# `-I .` is for glvirgl.c, which shares kernel/gpu/gpu_syscalls.h with the
# kernel rather than duplicating the ABI structs.  The stub directory supplies
# the unistd.h that declares syscall(), which the host's own header does not
# declare compatibly.
LIBGL_TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -I . \
                     -I lib/libgl/include -I lib/libgl/src -I tests/unit/glstub

# What each test covers:
#   test_glmath   vector/matrix math (no context needed, links glmath.c alone)
#   test_glstate  context lifecycle, GL error contract, glClear, presentation
#   test_glimm    matrix stacks, immediate mode, the transform pipeline
#   test_glraster filled rasterizer, depth buffer, culling, top-left fill rule
#   test_glclip   frustum clipping and the glPushAttrib/glPopAttrib stack
#   test_gllight  the GL 1.1 lighting equation and materials
#   test_gltex    texture objects, sampling, perspective correction, blending,
#                 the alpha test and fog
#   test_gltex2   GL 1.2/1.3: mipmaps, multitexturing, 3D textures, cube maps
#   test_glfbo    framebuffer objects, renderbuffers and glReadPixels
#   test_glsl     the GLSL ES 1.0 front end: lexer, parser, type checker
#   test_glslexec the GLSL execution engine, checked numerically
#   test_glprog   the shader pipeline: programs, attributes, uniforms, pixels
#   test_glcoexist  fixed function and shaders side by side, and their limits
#   test_glvirgl  the VirGL backend: declining cleanly, and the wire format
#
# libauragui cannot be built for the host (it needs AuraLite's freestanding
# libc), so tests/unit/glstub/ provides a recording stand-in for ag_blit() and
# ag_render_now() -- the code under test is still the real auraglx.c.
LIBGL_TESTS := test_glstate test_glimm test_glraster test_glclip \
               test_gllight test_gltex test_gltex2 test_glarray test_glu \
               test_glbackend test_glfbo test_glsl test_glslexec \
               test_glprog test_glcoexist

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
# test_glvirgl links the REAL kernel command-stream validator alongside libgl,
# so the backend's encoding is checked against the code that would reject it.
$(BUILD_DIR)/test_glvirgl: tests/unit/test_glvirgl.c $(LIBGL_TEST_SRCS) \
                          $(LIBGL_TEST_HDRS) $(LIBGL_TEST_STUB) \
                          kernel/gpu/gpu_cmdcheck.c drivers/gpu/virgl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) $(LIBGL_TEST_CFLAGS) $< $(LIBGL_TEST_SRCS) \
	          kernel/gpu/gpu_cmdcheck.c $(LIBGL_TEST_STUB) -o $@ -lm

$(BUILD_DIR)/test_gpu_syscall: tests/unit/test_gpu_syscall.c \
                               kernel/gpu/gpu_cmdcheck.c \
                               kernel/gpu/gpu_syscalls.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	          tests/unit/test_gpu_syscall.c kernel/gpu/gpu_cmdcheck.c -o $@

$(BUILD_DIR)/test_glmath: tests/unit/test_glmath.c lib/libgl/src/glmath.c \
                          lib/libgl/include/GL/glmath.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I lib/libgl/include \
	          tests/unit/test_glmath.c lib/libgl/src/glmath.c -o $@ -lm

$(BUILD_DIR)/test_virgl: tests/unit/test_virgl.c drivers/gpu/virgl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_virtio_net: tests/unit/test_virtio_net.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_stack_guard: tests/unit/test_stack_guard.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_select_stack: tests/unit/test_select_stack.c kernel/fs/select.c
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

# DOOM_PLAN D1: fseek/ftell/rewind, memmove and abs.  Unlike test_stdio_ext,
# this one tests AURALITE'S implementations, extracted and renamed by the
# script so GCC's builtins cannot stand in for them.  Under ASan because a
# direction bug in memmove is an overlapping-buffer bug.
$(BUILD_DIR)/test_stdio_seek: tests/unit/test_stdio_seek.c \
                              lib/libc/src/string_extra.c \
                              lib/libc/src/stdlib_extra.c \
                              tools/extract_libc_impls.py
	@mkdir -p $(BUILD_DIR)
	@python3 tools/extract_libc_impls.py $(BUILD_DIR)/libc_impls_gen.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -g \
	          -fsanitize=address,undefined -I . \
	          tests/unit/test_stdio_seek.c $(BUILD_DIR)/libc_impls_gen.c -o $@

# The printf conversions, checked against the host's glibc.  Not -Werror on
# the generated file: it is a verbatim extract of libc.c compiled in a host
# context, and warnings there are about that context, not about the code.
$(BUILD_DIR)/test_printf_format: tests/unit/test_printf_format.c \
                              lib/libc/src/libc.c \
                              tools/extract_libc_impls.py
	@mkdir -p $(BUILD_DIR)
	@python3 tools/extract_libc_impls.py $(BUILD_DIR)/libc_impls_gen.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -g \
	          -fsanitize=address,undefined -I . \
	          tests/unit/test_printf_format.c \
	          -Wno-error=format-truncation \
	          $(BUILD_DIR)/libc_impls_gen.c -o $@



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

# ---- Phase Q15: mq_notify state machine / queue format unit test ----
$(BUILD_DIR)/test_mq_notify: tests/unit/test_mq_notify.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q16: Issue-8 tail unit test (sig2str/str2sig table) ----
$(BUILD_DIR)/test_q16_tail: tests/unit/test_q16_tail.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

# ---- Phase Q14: System V IPC unit test (find-or-create, mtype, ABI) ----
$(BUILD_DIR)/test_sysvipc: tests/unit/test_sysvipc.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_keymap: tests/unit/test_keymap.c drivers/keyboard/keymap.c \
                          drivers/keyboard/keymap.h drivers/keyboard/keyboard.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	    tests/unit/test_keymap.c drivers/keyboard/keymap.c -o $@

$(BUILD_DIR)/test_q1_headers: tests/unit/test_q1_headers.c \
                               lib/libc/include/stdarg.h lib/libc/include/stddef.h lib/libc/include/stdint.h \
                               lib/libc/include/float.h lib/libc/include/inttypes.h lib/libc/include/iso646.h \
                               lib/libc/include/stdalign.h lib/libc/include/stdnoreturn.h lib/libc/include/tgmath.h \
                               lib/libc/include/complex.h lib/libc/include/fenv.h lib/libc/include/stdatomic.h \
                               lib/libc/include/wctype.h lib/libc/include/strings.h lib/libc/include/uchar.h \
                               lib/libc/include/setjmp.h lib/libc/include/threads.h
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

# printf's conversion specifiers, compared byte for byte against expected
# output.  The '-' flag was unparsed and printed literally for a long time
# because nothing checked formatting -- only a person reading a log would
# notice.  libc.c is LINKED here rather than #included: it declares main()
# for __libc_start_main, which would collide with the test's own.
#
# ONLY snprintf/vsnprintf ARE LEFT GLOBAL, and that is not tidiness.
#
# AuraLite's libc.c also defines printf, puts, stdout, stderr, fflush, exit
# and friends.  Linking the whole object into a host program makes THOSE the
# definitions the harness itself uses -- so the test printed its own results
# through the code under test, and at exit glibc tried to flush a `stdout`
# that was AuraLite's unrelated object.  On this machine it happened to work;
# in CI it aborted with "glibc detected an invalid stdio handle".
#
# objcopy -G localises everything else, so the harness gets glibc's stdio and
# the unit under test is reached only through the one symbol being tested.
$(BUILD_DIR)/libc_fmt_full.o: lib/libc/src/libc.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I lib/libc/include -c $< -o $@

$(BUILD_DIR)/libc_fmt.o: $(BUILD_DIR)/libc_fmt_full.o
	objcopy -G snprintf -G vsnprintf $< $@

$(BUILD_DIR)/test_printf_fmt: tests/unit/test_printf_fmt.c $(BUILD_DIR)/libc_fmt.o
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . -c $< -o $(BUILD_DIR)/test_printf_fmt.o
	$(HOST_CC) $(BUILD_DIR)/test_printf_fmt.o $(BUILD_DIR)/libc_fmt.o -o $@

# The package parser reads attacker-controlled input, so the shipping source
# is compiled in and exercised directly with malformed files.
$(BUILD_DIR)/test_apkg: tests/unit/test_apkg.c lib/libc/src/apkg.c lib/libc/include/apkg.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . -I lib/libc/include \
	          tests/unit/test_apkg.c lib/libc/src/apkg.c -o $@

# The search path is tested with a stub filesystem, so the search ORDER is
# observable — the real filesystem only shows which lookup happened to win.
# -I tests/unit/pathstub comes FIRST and is not optional: progpath.c includes
# "unistd.h" and "fcntl.h" meaning AuraLite's headers, and with only -I . those
# resolved to glibc's.  That worked until a CI machine with _FORTIFY_SOURCE on
# by default made glibc's open() an inline definition, which collided with the
# test's stub.  The stub directory keeps the include path under the test's
# control instead of the distribution's default flags.
#
# Only unistd.h and fcntl.h are stubbed.  There is deliberately no string.h
# stub: -I directories are searched for <angle> includes too, so one would
# also shadow the real <string.h> that the TEST needs for strcmp/snprintf.
# progpath.c's include of "string.h" therefore reaches glibc's, which is
# harmless -- it uses nothing from it.
$(BUILD_DIR)/test_progpath: tests/unit/test_progpath.c lib/libc/src/progpath.c \
                            tests/unit/pathstub/unistd.h \
                            tests/unit/pathstub/fcntl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 \
	          -I tests/unit/pathstub -I . $< -o $@

# The installation allowlist is a security predicate, so the test compiles the
# shipping source rather than a copy.
$(BUILD_DIR)/test_execpolicy: tests/unit/test_execpolicy.c \
                              kernel/fs/execpolicy.c kernel/fs/execpolicy.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	          tests/unit/test_execpolicy.c kernel/fs/execpolicy.c -o $@

# The initrd directory view is derived from a flat file table, so it is tested
# against the real parser rather than a reimplementation.
$(BUILD_DIR)/test_initrd_dirs: tests/unit/test_initrd_dirs.c kernel/fs/initrd.c \
                               kernel/fs/initrd.h kernel/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	          tests/unit/test_initrd_dirs.c kernel/fs/initrd.c -o $@

# Same shape as test_initrd_dirs, but the kmalloc stub can be told to fail:
# FIX_R4's gate is that initrd_init() reports the pool allocation failure
# instead of dereferencing the NULL pool.
$(BUILD_DIR)/test_initrd_allocfail: tests/unit/test_initrd_allocfail.c kernel/fs/initrd.c \
                                    kernel/fs/initrd.h kernel/fs/vfs.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
	          tests/unit/test_initrd_allocfail.c kernel/fs/initrd.c -o $@

$(BUILD_DIR)/test_vfs: tests/unit/test_vfs.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

$(BUILD_DIR)/test_network: tests/unit/test_network.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . $< -o $@

# X3: DNS reliability — compiles the real resolver/parser modules with the
# kernel environment stubbed (fake clock, scripted transport).
$(BUILD_DIR)/test_dns: tests/unit/test_dns.c kernel/net/dns_parse.c kernel/net/dns.c \
		kernel/net/dns_parse.h kernel/net/dns.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -DAURALITE_DNS_HOST_TEST -I . \
		tests/unit/test_dns.c kernel/net/dns_parse.c kernel/net/dns.c -o $@

# X5: TCP hardening policy — pure header (RTO/PMTUD ladder/scheduler/sequencer).
$(BUILD_DIR)/test_tcp_x5: tests/unit/test_tcp_x5.c kernel/net/tcp_x5.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . \
		tests/unit/test_tcp_x5.c -o $@

# M6 (MATURITY_PLAN.md): fast retransmit/recovery, Nagle, delayed ACK and
# TIME_WAIT — pure policy header, testable without a NIC.
$(BUILD_DIR)/test_tcp_m6: tests/unit/test_tcp_m6.c kernel/net/tcp_m6.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
		tests/unit/test_tcp_m6.c -o $@

# M6c (MATURITY_PLAN.md): SACK prerequisites — TCP option codec and a
# multi-segment retransmit queue.  Pure policy, testable without a NIC.
$(BUILD_DIR)/test_tcp_m6c: tests/unit/test_tcp_m6c.c kernel/net/tcp_m6c.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
		tests/unit/test_tcp_m6c.c -o $@

# M6d (MATURITY_PLAN.md): SACK (RFC 2018) — block codec, queue marking,
# hole selection.  Pure policy on top of the M6c prerequisites.
$(BUILD_DIR)/test_tcp_m6d: tests/unit/test_tcp_m6d.c kernel/net/tcp_m6d.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
		tests/unit/test_tcp_m6d.c -o $@

# M6e (MATURITY_PLAN.md): listen backlog, SO_REUSEADDR, keepalive.
$(BUILD_DIR)/test_tcp_m6e: tests/unit/test_tcp_m6e.c kernel/net/tcp_m6e.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . \
		tests/unit/test_tcp_m6e.c -o $@

# X4: IPv4 fragment reassembly — pure engine, injected clock.
$(BUILD_DIR)/test_ip_reasm: tests/unit/test_ip_reasm.c kernel/net/ip_reasm.c \
		kernel/net/ip_reasm.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . tests/unit/test_ip_reasm.c -o $@

# USB_PLAN U1: xHCI event-ring cycle/wrap arithmetic.  Pure logic, tested
# off-hardware -- this is where the classic "works once, then hangs" xHCI
# bug lives, and a guest test alone would not isolate it.
$(BUILD_DIR)/test_xhci_ring: tests/unit/test_xhci_ring.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . tests/unit/test_xhci_ring.c -o $@

# X7: IPv6 address helpers — pure engine, compiled straight in.
$(BUILD_DIR)/test_ipv6_addr: tests/unit/test_ipv6_addr.c kernel/net/ipv6_addr.c \
		kernel/net/ipv6_addr.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -O2 -I . tests/unit/test_ipv6_addr.c -o $@

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

$(BUILD_DIR)/test_errno: tests/unit/test_errno.c lib/libc/include/errno.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_ctype: tests/unit/test_ctype.c lib/libc/include/ctype.h \
                         lib/libc/include/limits.h lib/libc/include/stdbool.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_open_flags: tests/unit/test_open_flags.c lib/libc/include/fcntl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_lseek: tests/unit/test_lseek.c lib/libc/include/sys/uio.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_signals: tests/unit/test_signals.c lib/libc/include/signal.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_termios: tests/unit/test_termios.c lib/libc/include/termios.h lib/libc/include/sys/ioctl.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_jobcontrol: tests/unit/test_jobcontrol.c lib/libc/include/sys/wait.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_permissions: tests/unit/test_permissions.c lib/libc/include/unistd.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_cow: tests/unit/test_cow.c
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_slab: tests/unit/test_slab.c kernel/mm/slab.c kernel/mm/slab.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . tests/unit/test_slab.c kernel/mm/slab.c -o $@

# ---- Package tool (SDK_PLAN phase S4) ----
#
# mkapkg links the SAME parser the OS uses (lib/libc/src/apkg.c), so the writer
# and the reader cannot disagree about the format.  The two translation units
# are compiled SEPARATELY and with different include paths on purpose:
# apkg.c is AuraLite code and needs lib/libc/include, while mkapkg.c is a host
# program that needs the HOST's stdio.h -- putting lib/libc/include ahead of it
# would hide fseek/ftell behind AuraLite's freestanding subset.
MKAPKG := $(BUILD_DIR)/mkapkg

$(BUILD_DIR)/apkg_host.o: lib/libc/src/apkg.c lib/libc/include/apkg.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I lib/libc/include -c $< -o $@

$(BUILD_DIR)/mkapkg.o: tools/mkapkg.c lib/libc/include/apkg.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . -c $< -o $@

$(MKAPKG): $(BUILD_DIR)/mkapkg.o $(BUILD_DIR)/apkg_host.o
	$(HOST_CC) $^ -o $@
	@echo "[host] $@"


# ---- DOOM (DOOM_PLAN.md) ----
#
# The engine is NOT vendored into this repository.  DOOM's source is
# GPL-2.0 and AuraLite is Apache-2.0; the FSF considers those incompatible
# (Apache's patent-termination and indemnity clauses are "further
# restrictions" GPLv2 forbids).  So `make doom` FETCHES doomgeneric into
# build/, compiles it there, and the repository ships only
# doom/doomgeneric_auralite.c -- AuraLite's own platform layer under
# AuraLite's own licence.  A user who runs this target builds a GPL-2.0
# binary on their own machine, which is fine; what AuraLite never does is
# distribute one.
#
# Everything below is therefore opt-in and off the default build path.
DOOM_DIR     := $(BUILD_DIR)/doom
DOOM_SRC     := $(DOOM_DIR)/doomgeneric/doomgeneric
DOOM_REPO    := https://github.com/ozkl/doomgeneric.git
DOOM_ELF     := $(USER_BUILD)/doom.elf
DOOM_WAD_URL := https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
DOOM_WAD     := $(DOOM_DIR)/freedoom1.wad

$(DOOM_SRC):
	@mkdir -p $(DOOM_DIR)
	@echo "[doom] fetching doomgeneric (GPL-2.0, not vendored -- see the"
	@echo "[doom] licensing note in doom/doomgeneric_auralite.c)"
	@git clone -q --depth 1 $(DOOM_REPO) $(DOOM_DIR)/doomgeneric
	@echo "[doom] $$(ls $(DOOM_SRC)/*.c | wc -l) engine sources"

# Freedoom's data is BSD-licensed and freely redistributable, unlike the
# original DOOM WADs.  Fetched rather than committed: it is ~20 MB of
# generated data that would bloat every clone of this repository.
$(DOOM_WAD):
	@mkdir -p $(DOOM_DIR)
	@echo "[doom] fetching Freedoom (BSD-licensed game data)"
	@curl -sL $(DOOM_WAD_URL) -o $(DOOM_DIR)/freedoom.zip
	@cd $(DOOM_DIR) && unzip -qo freedoom.zip && 	    cp freedoom-*/freedoom1.wad . && rm -rf freedoom-* freedoom.zip
	@echo "[doom] $$(du -h $(DOOM_WAD) | cut -f1) $(DOOM_WAD)"

# No platform #defines and no patches: the engine builds against
# AuraLite's libc as it stands.  Getting there meant filling real gaps
# rather than working around them -- fseek/ftell/rewind, memmove, abs and
# system() were missing, and SEEK_* lived only in <unistd.h> instead of
# <stdio.h>.  Every one is a plain C-library conformance gap that any
# ported program would hit, so they were fixed in libc where they belong.
# The tempting shortcut, -D__DJGPP__, silences the system() call but then
# demands a DOS <go32.h> -- which is how one workaround becomes two.
#
# -w because these are third-party sources: their warnings are not ours to
# fix, and -Werror on code we do not own turns every upstream change into a
# build break.
# through system().  That is the only part of the engine AuraLite's libc
# cannot satisfy, and the upstream source already guards it behind a
# platform check -- so no patching of GPL sources is needed, which keeps
# this a clean "compile, don't modify" boundary.
DOOM_CFLAGS := $(USER_CFLAGS) -w -I lib/libauragui/include -I $(DOOM_SRC)

# The engine's own sources, minus the backends for other platforms.
# The engine's source list, taken from upstream's own Makefile rather than
# guessed at with a wildcard-and-filter.
#
# That is the difference between a list that is right and one that happens
# to work: the tree also ships backends for SDL, X11, Allegro, Windows,
# emscripten and the Linux VT, plus their sound and music drivers, and a
# filter has to enumerate every one of them correctly or the build dies on
# a missing <allegro/base.h>.  Upstream already maintains the correct list
# in SRC_DOOM; this reads it and swaps their xlib backend for ours.
#
# Recursively expanded (=, not :=) on purpose: the sources do not exist
# until the fetch rule has run, so this must be evaluated when the recipe
# runs rather than when the Makefile is parsed.
DOOM_UPSTREAM_OBJS = $(shell sed -n 's/^SRC_DOOM = //p' \
                       $(DOOM_SRC)/Makefile 2>/dev/null)
# `dummy.o` is NOT filtered out, and that is deliberate.  Upstream's
# dummy.c is not a placeholder: it holds the definitions of `drone` and
# `net_client_connected` for builds without net_client.c (which
# doomgeneric does not ship).  Dropping it linked cleanly until upstream
# commit dcb7a8d ("boolean fix"), after which the link fails with
# `undefined symbol: drone` / `net_client_connected` from d_loop.c and
# d_main.c.  Its only other content, I_InitTimidityConfig(), is guarded by
# `#ifndef FEATURE_SOUND` and the sole competing definition lives in
# i_sdlmusic.c, which SRC_DOOM does not list -- so there is no duplicate
# symbol to avoid.  Only the xlib backend is genuinely ours to replace.
DOOM_ENGINE_SRCS = $(patsubst %.o,$(DOOM_SRC)/%.c, \
                     $(filter-out doomgeneric_xlib.o, \
                       $(DOOM_UPSTREAM_OBJS)))

doom: $(DOOM_ELF)

$(DOOM_ELF): $(DOOM_SRC) doom/doomgeneric_auralite.c $(USER_COMMON)              $(USER_GUI_OBJ) lib/libc/user.ld
	@mkdir -p $(DOOM_DIR)/obj $(USER_BUILD)
	@echo "[doom] compiling the engine ($$(echo $(DOOM_ENGINE_SRCS) | wc -w) files)"
	@for f in $(DOOM_ENGINE_SRCS); do 	    o=$(DOOM_DIR)/obj/$$(basename $$f .c).o; 	    $(HOST_CC) $(DOOM_CFLAGS) -c $$f -o $$o || exit 1; 	done
	@$(HOST_CC) $(DOOM_CFLAGS) -c doom/doomgeneric_auralite.c 	    -o $(DOOM_DIR)/obj/doomgeneric_auralite.o
	$(LD) $(USER_LDFLAGS) $(DOOM_DIR)/obj/*.o 	    $(USER_COMMON_LNK) $(USER_GUI_OBJ) -o $@
	@echo "[link] $@ ($$(du -h $@ | cut -f1))"

# The WAD travels on its own FAT32 disk, not in the initrd.
#
# Not a preference -- a constraint.  The BIOS loader reserves a 16 MiB slot
# for initrd.tar (tools/mkisoimage_dual.sh enforces it), the initrd is
# already ~7.6 MiB, and the smallest Freedoom IWAD is 22 MiB.  The kernel
# already mounts a FAT32 volume found at LBA 64 of the first AHCI disk as
# /fat, so the image below is built to exactly that layout: 64 empty sectors
# and then the filesystem.  Getting that offset wrong is not a mount
# failure, it is worse -- the kernel sees no signature and FORMATS the disk,
# silently destroying the WAD.
DOOM_DISK := $(DOOM_DIR)/doomdisk.img

$(DOOM_DISK): $(DOOM_WAD) $(DOOM_ELF)
	@echo "[doom] building the WAD disk (FAT32 at LBA 64)"
	@rm -f $@ $(DOOM_DIR)/fatpart.img
	@dd if=/dev/zero of=$(DOOM_DIR)/fatpart.img bs=1M count=64 status=none
	@mformat -i $(DOOM_DIR)/fatpart.img -F -h 32 -s 32 -t 128 ::
	@mmd -i $(DOOM_DIR)/fatpart.img ::/doom
	@mcopy -i $(DOOM_DIR)/fatpart.img $(DOOM_WAD) ::/doom/freedoom1.wad
# The binary rides on the same disk as its data, rather than in the initrd.
# Two reasons, and the first is decisive: doom.elf is ~490 KiB and the
# initrd is already ~8.0 MiB of a 16 MiB BIOS-loader budget that
# mkisoimage_dual.sh enforces, so it simply does not fit.  The second is
# that it keeps GPL-2.0-derived build output out of the default image
# entirely -- `make iso` produces exactly what it did before.
	@strip -s $(DOOM_ELF) -o $(DOOM_DIR)/doom.stripped
	@mcopy -i $(DOOM_DIR)/fatpart.img $(DOOM_DIR)/doom.stripped ::/doom/doom
	@dd if=/dev/zero of=$@ bs=512 count=64 status=none
	@cat $(DOOM_DIR)/fatpart.img >> $@
	@rm -f $(DOOM_DIR)/fatpart.img
	@echo "[doom] $$(du -h $@ | cut -f1) $@"

# Build an ISO with doom.elf installed, then boot it with the WAD disk
# attached.  Separate from `make run` so the default path stays free of a
# 28 MB download and of any GPL-licensed build output.
run-doom: $(DOOM_DISK) iso
	@echo "[doom] booting; at the shell type: run /fat/doom/doom"
	qemu-system-x86_64 \
	    -drive file=$(BUILD_DIR)/auralite.iso,format=raw,if=ide,snapshot=on \
	    -drive id=wad,file=$(DOOM_DISK),format=raw,if=none,snapshot=on \
	    -device ahci,id=ahci -device ide-hd,drive=wad,bus=ahci.0 \
	    -boot order=c -m 512M -smp 2 -cpu qemu64 -no-reboot \
	    -vga std

.PHONY: doom run-doom

# ---- SDK (SDK_PLAN phase S1) ----
#
# `make sdk` assembles everything an out-of-tree application needs into
# build/sdk.  It is ASSEMBLED FROM THE REAL SOURCES, never a copy kept in the
# repository: a duplicated header is wrong the moment either side changes, and
# nobody notices until a user's program misbehaves.
#
# `make sdk-check` then builds examples/ AGAINST THE STAGED SDK -- not against
# the source tree.  That distinction is the whole point.  If the examples could
# reach into lib/libc/include directly they would keep building after the SDK
# stopped being sufficient, and the check would prove nothing.
SDK_DIR := $(BUILD_DIR)/sdk

sdk: libs $(USER_BUILD)/crt0.o
	@rm -rf $(SDK_DIR)
	@mkdir -p $(SDK_DIR)/include $(SDK_DIR)/lib
	@cp -r lib/libc/include/. $(SDK_DIR)/include/
	@cp lib/libauragui/include/auragui.h $(SDK_DIR)/include/
	@mkdir -p $(SDK_DIR)/include/GL
	@cp lib/libgl/include/GL/*.h $(SDK_DIR)/include/GL/
	@mkdir -p $(SDK_DIR)/include/atls
	@cp lib/libatls/include/atls/*.h $(SDK_DIR)/include/atls/
	@cp $(LIBAURAC) $(LIBAURAGUI) $(LIBAGL) $(LIBATLS) $(SDK_DIR)/lib/
	@cp $(USER_BUILD)/crt0.o $(SDK_DIR)/lib/
	@cp lib/libc/user.ld $(SDK_DIR)/
	@bash tools/mksdk.sh $(SDK_DIR)
	@echo "[sdk] $(SDK_DIR) ($$(find $(SDK_DIR) -type f | wc -l) files)"

sdk-check: sdk
	@bash tools/sdk_check.sh $(SDK_DIR)

# ---- Win32 SDK (WIN32_PLAN.md W32-8) ----
#
# Mirrors `make sdk`, but for building Win32 programs against this
# personality.  What it stages is deliberately small: the import libraries
# and the examples.  There are no headers to ship -- a Win32 program uses
# mingw-w64's <windows.h> on the host, which is the whole point of a
# personality, and vendoring a copy would duplicate something that already
# exists and would go stale.
W32_SDK_DIR := $(BUILD_DIR)/w32-sdk

w32-sdk: $(K32_IMPLIB) $(U32_IMPLIB) $(G32_IMPLIB)
	@rm -rf $(W32_SDK_DIR)
	@mkdir -p $(W32_SDK_DIR)/lib $(W32_SDK_DIR)/examples
	@cp $(K32_IMPLIB) $(U32_IMPLIB) $(G32_IMPLIB) $(W32_SDK_DIR)/lib/
	@cp -r w32/examples/. $(W32_SDK_DIR)/examples/
	@cp docs/win32.md $(W32_SDK_DIR)/
	@cp w32/PROVENANCE.md w32/LICENSING.md $(W32_SDK_DIR)/ 2>/dev/null || true
	@bash tools/mkw32sdk.sh $(W32_SDK_DIR)
	@echo "[w32-sdk] $(W32_SDK_DIR) ($$(find $(W32_SDK_DIR) -type f | wc -l) files)"

# Build every example against the staged SDK, exactly as sdk-check does:
# if they could reach into the source tree they would keep building after
# the SDK stopped being sufficient, and the check would prove nothing.
w32-sdk-check: w32-sdk
	@bash tools/w32_sdk_check.sh $(W32_SDK_DIR)

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