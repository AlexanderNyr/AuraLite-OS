#!/usr/bin/env bash
# tests/integration/a64_image_smoke.sh -- ARM64_PLAN A5a smoke test.
#
# The Image exit ramp: llvm-objcopy -O binary of kernela64.elf plus the
# 64-byte arm64 Image header in boot.S IS a Linux-boot-protocol Image,
# and that protocol is what makes two things real that the ELF path
# never had (A1's measured facts, both pinned by a64_boot_smoke.sh):
#
#   * x0 = DTB physical address at entry -- the kernel VERIFIES the
#     magic there before trusting it, and prints the source it chose;
#   * -initrd actually loads: /chosen carries initrd-start/end, and the
#     kernel proves the BYTES are present ("ustar" at offset 257 of the
#     first tar header), not just advertised.
#
# The whole A4 gauntlet must still pass on this path -- the header is
# 64 bytes of data plus one branch, not a second kernel.
#
# Skips cleanly when qemu-system-aarch64 or llvm-objcopy is absent
# (the optional-tool convention).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/kernela64.img"
INITRD="$BUILD/initrd.tar"
LOG="$BUILD/a64_image.log"

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "[a64-image] SKIP: qemu-system-aarch64 not installed" >&2
    exit 0
fi
if ! command -v llvm-objcopy >/dev/null 2>&1; then
    echo "[a64-image] SKIP: llvm-objcopy not installed" >&2
    exit 0
fi

[ -s "$IMG" ]    || make -C "$ROOT" kernela64-img >/dev/null
[ -s "$INITRD" ] || { echo "[a64-image] SKIP: build/initrd.tar absent (run 'make iso' first)" >&2; exit 0; }

fail=0
assert_grep() {
    local log="$1" pat="$2" desc="$3"
    if grep -qa "$pat" "$log"; then
        printf '  [a64-image] OK   %s\n' "$desc"
    else
        printf '  [a64-image] FAIL %s\n' "$desc"
        fail=1
    fi
}

rm -f "$LOG"
timeout 60 qemu-system-aarch64 -machine virt -cpu cortex-a72 \
    -m 256M -display none -serial file:"$LOG" -no-reboot \
    -kernel "$IMG" -initrd "$INITRD" >/dev/null 2>&1 || true

# --- the Image path's own facts ---
assert_grep "$LOG" "Hello from AuraLite OS kernel (aarch64)!"          "banner via the Image path"
assert_grep "$LOG" "(DTB pointer, Image path -- magic verified)"       "x0 carried the DTB and its magic verified"
assert_grep "$LOG" "DTB source: x0 (Image boot protocol)"              "the kernel chose the x0 source, named"
assert_grep "$LOG" "initrd magic: ustar OK"                            "-initrd bytes are REALLY in RAM (tar magic read back)"
assert_grep "$LOG" "initrd: [0-9]* bytes at phys"                      "/chosen initrd-start/end parsed by the shared walker"

# --- the gauntlet still runs to the end on this path ---
assert_grep "$LOG" "\[timer\] PASS"                                    "timer gauntlet"
assert_grep "$LOG" "\[pmm\]  PASS"                                     "PMM gauntlet"
assert_grep "$LOG" "\[heap\] PASS"                                     "heap gauntlet"
assert_grep "$LOG" "\[fpu\]  PASS"                                     "FPU eager-save gauntlet"
assert_grep "$LOG" "A64-U-OK!"                                         "EL0 payload ran"
assert_grep "$LOG" "PASS: EL0 entered, syscalls served"                "EL0 self-test"

# --- never-see ---
if grep -qa "FAIL" "$LOG"; then
    printf '  [a64-image] FAIL kernel log contains FAIL lines\n'
    fail=1
else
    printf '  [a64-image] OK   no FAIL lines in the kernel log\n'
fi

if [ "$fail" -ne 0 ]; then
    echo "[a64-image] FAILED (log: $LOG)"
    exit 1
fi
echo "[a64-image] all A5a assertions passed"
