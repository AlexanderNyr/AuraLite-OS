#!/usr/bin/env bash
# tools/mkw32sdk.sh — stage the Win32 SDK's build glue (WIN32_PLAN.md W32-8).
#
# Writes w32.mk, which an out-of-tree Win32 program includes so it is built
# the same way the examples are.  Mirrors tools/mksdk.sh.
set -euo pipefail
DIR="${1:?usage: mkw32sdk.sh <w32-sdk-dir>}"

cat > "$DIR/w32.mk" <<'MK'
# w32.mk — build glue for Win32 programs targeting AuraLite OS.
#
#   include $(AURALITE_W32_SDK)/w32.mk
#
# mingw-w64 is used as a HOST cross-compiler: it produces the .exe, and none
# of its runtime ships in AuraLite.  Only the compiler's output crosses over,
# which is the licensing distinction recorded in LICENSING.md -- headers are
# public domain, the CRT is not, and -nostdlib keeps it out.

AURALITE_W32_CROSS ?= x86_64-w64-mingw32-
AURALITE_W32_CC    := $(AURALITE_W32_CROSS)gcc

AURALITE_W32_CFLAGS ?= -O2 -Wall -Wextra -m64

# The personality supplies its own startup (TLS callbacks, .CRT$XC*), so
# mingw's CRT is deliberately not linked and the entry point is named
# explicitly.
AURALITE_W32_LDFLAGS ?= -nostdlib -Wl,--entry=winstart

AURALITE_W32_LIBS ?= -lkernel32 -luser32 -lgdi32
MK

cat > "$DIR/README.md" <<'RM'
# AuraLite OS — Win32 SDK

Everything needed to build a Win32 program that runs on AuraLite.

## What you need

`mingw-w64` on the build host:

    # Debian/Ubuntu
    sudo apt-get install mingw-w64

No AuraLite headers are required. A Win32 program uses mingw-w64's
`<windows.h>` — that is the point of a personality: the program does not know
where it will run. Nothing from mingw-w64's runtime ships in AuraLite; only
the compiler's output crosses over.

## Building

    cd examples/console-app
    make
    # copy hello.exe into the AuraLite initrd, then on AuraLite:
    run hello.exe

The shell detects a PE image by its magic number. A `.exe` that imports Win32
DLLs is routed through `/apps/w32run`, which binds the imports and runs the
CRT startup sequence.

## What is supported

See `win32.md` — in particular its generated function table, and its list of
behaviours that are approximations rather than faithful. Read that list
before assuming a program will work: SEH is not table-driven unwinding, TLS
is per-process, and the registry, COM and DirectX are not implemented at all.

## Examples

- `console-app/` — stdout, the command line, and the heap.
- `gui-app/` — a window, a message loop and painting, over the native
  compositor.
- `unsupported-app/` — a program that is deliberately **refused**, to show
  the failure mode: it is rejected at load time with the offending import
  named, rather than crashing later.

## Licensing

See `LICENSING.md` and `PROVENANCE.md`. Declarations were taken from
published documentation and mingw-w64's public-domain headers; no Wine or
ReactOS source was consulted.
RM

echo "  [w32-sdk] w32.mk and README.md written"
