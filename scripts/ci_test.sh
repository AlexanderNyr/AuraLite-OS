#!/usr/bin/env bash
# ci_test.sh — integration gate: boot the ISO in QEMU and assert that the
# kernel prints the expected banner on the serial console.
#
# Exit 0 on success, non-zero on failure.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ISO="${1:-build/auralite.iso}"
SERIAL="$(mktemp)"

if [ ! -f "$ISO" ]; then
    echo "[ci] building ISO first..."
    make iso >/dev/null
    ISO="build/auralite.iso"
fi

echo "[ci] booting $ISO (10s budget)..."
set +e
timeout 10 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512M -vga std -display none \
    -serial file:"$SERIAL" -no-reboot -cpu qemu64 >/dev/null 2>&1
# A clean halt never exits QEMU on its own, so a timeout (124) is expected.
set -e

echo "[ci] serial output:"
cat "$SERIAL"

pass=1
grep -q "Hello from AuraLite OS kernel!" "$SERIAL" || pass=0
grep -q "GDT loaded"               "$SERIAL" || pass=0
grep -q "IDT installed: 256 gates" "$SERIAL" || pass=0
grep -q "PIC remapped"             "$SERIAL" || pass=0
grep -q "HHDM offset: 0xffff800000000000" "$SERIAL" || pass=0
# Phase 2 (structural): IDT and PIC must initialise.
grep -q "IDT installed: 256 gates" "$SERIAL" || pass=0
grep -q "PIC remapped"             "$SERIAL" || pass=0
# Phase 3 gate: PMM must initialise and allocate 1000 unique frames.
grep -q "\[pmm\] PASS: 1000 unique frames, no leak, contiguous alloc OK" "$SERIAL" || pass=0
# Phase 4 gate: VMM must map/unmap correctly.
grep -q "\[vmm\] PASS: map / read / write / unmap all correct" "$SERIAL" || pass=0
# Phase 5 gate: kernel heap must pass 10000 alloc/free cycles.
grep -q "\[heap\] PASS: 10000 cycles, no corruption, no leak, realloc OK" "$SERIAL" || pass=0
# Phase 6 gate: PIT timer must measure ~1 second accurately (within +/-5%).
grep -q "\[timer\] PASS:" "$SERIAL" || pass=0
# Phase 7 gate: two threads must interleave and both complete.
grep -q "\[sched\] PASS: two threads interleaved correctly" "$SERIAL" || pass=0
# Phase 8 gate: user mode (Ring 3) + syscall + ELF loader.
grep -q "entering Ring 3" "$SERIAL" || pass=0
grep -q "loaded .* segment" "$SERIAL" || pass=0
# Phase 9 gate: compiled ELF binary runs in Ring 3, write() works.
grep -q "^hello$" "$SERIAL" || pass=0
grep -q "\[user\] PASS: compiled ELF ran in Ring 3, write() worked" "$SERIAL" || pass=0
# Phase 10 gate: VFS with initrd + devfs.
grep -q "\[initrd\] parsed .* file" "$SERIAL" || pass=0
grep -q "/init: opened" "$SERIAL" || pass=0
grep -q "\[vfs\] PASS: VFS layer functional" "$SERIAL" || pass=0

rm -f "$SERIAL"

if [ "$pass" -eq 1 ]; then
    echo "[ci] PASS — Phase 0/1 gate criteria met."
    exit 0
else
    echo "[ci] FAIL — expected strings not found on serial."
    exit 1
fi
