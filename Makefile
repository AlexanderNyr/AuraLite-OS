# =============================================================================
# AuraLite OS — Top-level Makefile
# Toolchain: Clang (--target=x86_64-elf) + LLD + NASM, booted by Limine.
# =============================================================================

ARCH        := x86_64
TARGET      := $(ARCH)-elf
CC          := clang
LD          := ld.lld
AS          := nasm

BUILD_DIR   := build
LIMINE_DIR  := limine
KERNEL_ELF  := $(BUILD_DIR)/kernel.elf
ISO_IMAGE   := $(BUILD_DIR)/auralite.iso

# -mcmodel=kernel: code lives in the top 2 GiB (negative addresses); required so
#   clang emits relocations valid for the higher-half link address.
# -mno-red-zone: the 128-byte red zone is unsafe under interrupts.
# -mno-sse/mmx: we never initialise the FPU/SSE unit in the kernel.
CFLAGS      := --target=$(TARGET) \
               -std=c11 -ffreestanding -fno-stack-protector \
               -fno-pie -fno-pic -mcmodel=kernel -mno-red-zone \
               -mno-mmx -mno-sse -mno-sse2 \
               -fno-omit-frame-pointer \
               -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
               -O2 -g \
               -DARCH_X86_64 -I . -I $(LIMINE_DIR) -I $(BUILD_DIR)

ASFLAGS     := -f elf64

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

.PHONY: all kernel iso run clean

all: iso

kernel: $(KERNEL_ELF)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) kernel.ld $(USER_BIN_H)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(KERNEL_OBJS) -o $@
	@echo "[link] $(KERNEL_ELF)"

# ---- User-space program build (compiled with host cc, linked with LLD) ----
# The hello binary is a freestanding static ELF linked at 0x40000000, embedded
# into the kernel as a C array via tools/gen_user_binary.py.
USER_BUILD   := $(BUILD_DIR)/user
USER_ELF     := $(USER_BUILD)/hello.elf
USER_BIN_H   := $(BUILD_DIR)/hello_bin.h

USER_CFLAGS  := -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
                -O2 -Wall -Wextra -Werror -I libc/include
USER_LDFLAGS := -nostdlib -static -T libc/user.ld -z max-page-size=4096

USER_OBJS := $(USER_BUILD)/hello.o $(USER_BUILD)/crt0.o \
             $(USER_BUILD)/syscall.o $(USER_BUILD)/libc.o

user: $(USER_ELF)

$(USER_BUILD)/hello.o: userspace/hello/hello.c libc/include/unistd.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/libc.o: libc/src/libc.c libc/include/unistd.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/crt0.o: libc/crt/crt0.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_BUILD)/syscall.o: libc/src/syscall.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_ELF): $(USER_OBJS) libc/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) $(USER_OBJS) -o $@
	@echo "[link] $(USER_ELF)"

$(USER_BIN_H): $(USER_ELF) tools/gen_user_binary.py
	@mkdir -p $(dir $@)
	python3 tools/gen_user_binary.py $(USER_ELF) $@ hello_bin

# user.c includes the generated hello_bin.h; ensure it exists first.
$(BUILD_DIR)/kernel/proc/user.o: $(USER_BIN_H)

iso: kernel $(BUILD_DIR)/initrd.tar
	@bash tools/mkisoimage.sh $(KERNEL_ELF) $(ISO_IMAGE) $(LIMINE_DIR)

# Build the initrd (USTAR tarball of userspace binaries).
INITRD_DIR := $(USER_BUILD)/initrd_root
$(BUILD_DIR)/initrd.tar: $(USER_ELF)
	@mkdir -p $(INITRD_DIR)
	@cp $(USER_ELF) $(INITRD_DIR)/init
	@bash tools/mkinitrd.sh $(INITRD_DIR) $@

run: iso
	@bash tools/run_qemu.sh $(ISO_IMAGE)

debug: iso
	@echo "Attach with: gdb $(KERNEL_ELF) -ex 'target remote :1234'"
	@bash tools/debug_qemu.sh $(ISO_IMAGE)

# ---- Host-side unit tests (built with the host compiler, no freestanding) ----
HOST_CC      := cc
UNIT_TESTS   := $(BUILD_DIR)/test_pmm $(BUILD_DIR)/test_heap

test-unit: $(UNIT_TESTS)
	@for t in $(UNIT_TESTS); do echo "[unit] running $$t"; ./$$t || exit 1; done

$(BUILD_DIR)/test_pmm: tests/unit/test_pmm.c kernel/lib/bitmap.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . $< -o $@

$(BUILD_DIR)/test_heap: tests/unit/test_heap.c kernel/mm/heap.c kernel/mm/heap.h
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I . tests/unit/test_heap.c kernel/mm/heap.c -o $@

clean:
	rm -rf $(BUILD_DIR)
