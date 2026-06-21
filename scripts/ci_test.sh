#!/usr/bin/env bash
# ci_test.sh — integration gate: boot the ISO in QEMU, send shell commands via
# serial, and assert the expected output appears.
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

# Create a test disk image for AHCI if it doesn't exist.
if [ ! -f "build/disk.img" ]; then
    echo "[ci] creating test disk image..."
    dd if=/dev/zero of=build/disk.img bs=1M count=4 status=none
    python3 -c "
data = bytearray(512)
data[0:4] = b'AURO'
data[510] = 0x55
data[511] = 0xAA
with open('build/disk.img', 'r+b') as f:
    f.write(data)
"
fi

echo "[ci] booting $ISO with shell commands (15s budget)..."

# Send commands with delays so the polling-based serial read doesn't miss chars.
set +e
(sleep 5; printf 'ls\n'; sleep 1; printf 'exit\n') | \
timeout 15 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512M -smp 4 -vga std -display none \
    -serial stdio -no-reboot -cpu qemu64 -boot order=d \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -drive file=build/disk.img,format=raw,if=none,id=ahcidisk \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=ahcidisk,bus=ahci0.0 > "$SERIAL" 2>&1
set -e

echo "[ci] serial output (tail):"
tail -20 "$SERIAL"

pass=1
grep -q "Hello from AuraLite OS kernel!" "$SERIAL" || pass=0
grep -q "IDT installed: 256 gates" "$SERIAL" || pass=0
grep -q "PIC remapped"             "$SERIAL" || pass=0
grep -q "HHDM offset: 0xffff800000000000" "$SERIAL" || pass=0
grep -q "\[pmm\] PASS: 1000 unique frames, no leak, contiguous alloc OK" "$SERIAL" || pass=0
grep -q "\[vmm\] PASS: map / read / write / unmap all correct" "$SERIAL" || pass=0
grep -q "\[heap\] PASS: 10000 cycles, no corruption, no leak, realloc OK" "$SERIAL" || pass=0
grep -q "\[timer\] PASS:" "$SERIAL" || pass=0
grep -q "\[sched\] PASS: two threads interleaved correctly" "$SERIAL" || pass=0
grep -q "entering Ring 3" "$SERIAL" || pass=0
grep -q "loaded .* segment" "$SERIAL" || pass=0
grep -q "\[initrd\] parsed .* file" "$SERIAL" || pass=0
grep -q "\[vfs\] PASS: VFS layer functional" "$SERIAL" || pass=0
grep -q "auralite#" "$SERIAL" || pass=0
grep -q "/init" "$SERIAL" || pass=0
grep -q "/hello" "$SERIAL" || pass=0
grep -q "init shell running in Ring 3" "$SERIAL" || pass=0
grep -q "\[smp\].*PASS:" "$SERIAL" || pass=0
grep -q "\[net\] PASS: ping 10.0.2.2 successful" "$SERIAL" || pass=0
grep -q "\[tcp\] PASS: TCP connect + send + close all worked" "$SERIAL" || pass=0
grep -q "\[gfx\] framebuffer GUI + window manager rendered" "$SERIAL" || pass=0

rm -f "$SERIAL"

if [ "$pass" -eq 1 ]; then
    echo "[ci] PASS — all phase gate criteria met."
    exit 0
else
    echo "[ci] FAIL — expected strings not found on serial."
    exit 1
fi
