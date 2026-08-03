#!/usr/bin/env bash
# mksdk.sh — write the generated parts of the AuraLite SDK.
#
# Usage: mksdk.sh <sdk_dir>
#
# The Makefile has already staged headers, libraries, crt0.o and the linker
# script into <sdk_dir>. This script writes the two files that are generated
# rather than copied: auralite.mk (the build flags) and README.md.
#
# They are generated, not checked in, so that they cannot drift from the
# Makefile that produced them.
set -euo pipefail

SDK="${1:?usage: $0 <sdk_dir>}"

# ---------------------------------------------------------------- auralite.mk
#
# The flags here MUST match USER_CFLAGS / USER_LDFLAGS in the top-level
# Makefile. They are repeated rather than extracted because an out-of-tree
# build has no access to that Makefile, and a three-line include is worth more
# to a user than a clever extraction. tools/sdk_check.sh compares the two and
# fails the build if they diverge, which is what keeps this honest.

cat > "$SDK/auralite.mk" <<'MKEOF'
# auralite.mk — build settings for AuraLite OS applications.
#
# Include this from your own Makefile:
#
#     AURALITE_SDK := /path/to/sdk
#     include $(AURALITE_SDK)/auralite.mk
#
#     myapp.elf: myapp.o
#             $(AURALITE_LD) $(AURALITE_LDFLAGS) $< $(AURALITE_LIBS) -o $@
#
# Everything below is derived from the OS's own build, so an application built
# with it is built exactly the way the shipped programs are.

# Where this SDK lives.  Set AURALITE_SDK before including, or rely on this.
AURALITE_SDK ?= $(dir $(lastword $(MAKEFILE_LIST)))

AURALITE_CC  ?= cc
AURALITE_LD  ?= ld.lld

# -ffreestanding: there is no host libc here.
# -fno-pie -fno-pic: programs link at a fixed address (see below); position-
#   independent code would be pointless overhead and the loader does not
#   process relocations.
# -fno-stack-protector: the OS supplies its own __stack_chk_guard seeding in
#   crt0; the host compiler's default guard would reference symbols libaurac
#   does not provide in the shape GCC expects.
AURALITE_CFLAGS := -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
                   -O2 -Wall -Wextra \
                   -I $(AURALITE_SDK)/include

AURALITE_LDFLAGS := -nostdlib -static -T $(AURALITE_SDK)/user.ld \
                    -z max-page-size=4096

# The C runtime.  crt0.o is named explicitly and is NOT inside libaurac.a: it
# defines only _start, which nothing references -- it is reached through the
# ELF entry point -- so as an archive member it would never be pulled in and
# your program would link with no entry code.
#
# --whole-archive keeps members that nothing references at link time, which
# includes the POSIX stubs that exist so that calling a not-yet-implemented
# function links rather than fails.
AURALITE_CRT0 := $(AURALITE_SDK)/lib/crt0.o
AURALITE_LIBC := $(AURALITE_CRT0) \
                 --whole-archive $(AURALITE_SDK)/lib/libaurac.a --no-whole-archive

# Console applications need only the C library.
AURALITE_LIBS    := $(AURALITE_LIBC)
# GUI applications add AuraGUI.
AURALITE_LIBS_GUI := $(AURALITE_LIBC) $(AURALITE_SDK)/lib/libauragui.a
# OpenGL applications add libaGL (and need AuraGUI to present a window).
AURALITE_LIBS_GL  := $(AURALITE_LIBC) $(AURALITE_SDK)/lib/libauragui.a \
                     $(AURALITE_SDK)/lib/libaGL.a

# ---- The ABI this SDK targets -----------------------------------------------
#
#   Architecture      x86_64
#   Linking           static only; there is no dynamic loader
#   Load address      0x40000000, fixed (see user.ld)
#   Entry             _start, System V AMD64 initial process stack
#   Segments          text RX, rodata R/NX, data+bss RW/NX, page-aligned
#
# The fixed load address is a real constraint, not an accident: the kernel
# maps each process into its own address space, so two programs at the same
# virtual address never collide.  It does mean shared libraries and user-text
# ASLR are not possible without changing it.
AURALITE_LOAD_ADDR := 0x40000000
MKEOF

# ------------------------------------------------------------------- README.md

cat > "$SDK/README.md" <<'RDEOF'
# AuraLite OS SDK

Everything needed to build an application for AuraLite OS from outside the
OS source tree.

## Contents

```
include/        C library, AuraGUI and OpenGL headers
lib/            libaurac.a  libauragui.a  libaGL.a  crt0.o
user.ld         linker script (fixed load address 0x40000000)
auralite.mk     the build flags, as a makefile fragment
```

This directory is **generated** by `make sdk` from the OS sources. Do not edit
it; edit the sources and regenerate.

## A minimal application

```c
#include "stdio.h"

int main(void) {
    printf("hello from my app\n");
    fflush(stdout);
    return 0;
}
```

```make
AURALITE_SDK := /path/to/build/sdk
include $(AURALITE_SDK)/auralite.mk

myapp.elf: myapp.o
	$(AURALITE_LD) $(AURALITE_LDFLAGS) $< $(AURALITE_LIBS) -o $@

myapp.o: myapp.c
	$(AURALITE_CC) $(AURALITE_CFLAGS) -c $< -o $@
```

Use `AURALITE_LIBS_GUI` for an AuraGUI application, `AURALITE_LIBS_GL` for
OpenGL. Worked examples are in `examples/` in the OS source tree.

## Getting the program onto a machine

Build it, wrap it with `tools/mkapkg.sh`, and install it with `apm`. Installed
programs go to `/opt`, which is the only directory besides `/tmp` where the
kernel permits an executable to be created.

## What this SDK does not offer

- **No dynamic linking.** Static only; `dlopen()` is a stub.
- **No ABI stability guarantee.** Syscall numbers are documented in
  `docs/syscall_abi.md`, not frozen.
- **A fixed load address.** Every program links at `0x40000000`.
- **`fflush(stdout)`** — output is buffered; a program that exits without
  flushing can lose its last line.
RDEOF

echo "[mksdk] wrote auralite.mk and README.md"
