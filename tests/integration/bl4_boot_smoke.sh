#!/usr/bin/env bash
# tests/integration/bl4_boot_smoke.sh -- full BIOS boot end-to-end.
#
# MBR -> Stage 2 -> pmode -> longmode -> kernel _start -> kmain ->
# the kernel prints "Hello from AuraLite OS kernel!" on COM1.
# That banner is the pass gate for BL4.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
IMG="$BUILD/bl4_boot.img"
LOG="$BUILD/bl4_boot.log"

# 1. Build all three artefacts + the kernel.
make -C "$ROOT" mbr stage2 kernel >/dev/null

# 2. Build a bootable disk with kernel.elf inside the FAT32 partition.
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
dd if="$BUILD/boot/mbr.bin"    of="$IMG" bs=512 count=1 conv=notrunc status=none
dd if="$BUILD/boot/stage2.bin" of="$IMG" bs=512 seek=1 conv=notrunc status=none

FAT_IMG="$BUILD/bl4_boot_part.img"
dd if=/dev/zero of="$FAT_IMG" bs=1M count=15 status=none
mformat -i "$FAT_IMG" -F -h 32 -s 32 -t 480 ::
mcopy -i "$FAT_IMG" "$BUILD/kernel.elf" ::KERNEL.ELF
dd if="$FAT_IMG" of="$IMG" bs=512 seek=128 conv=notrunc status=none

python3 - "$IMG" <<'PY'
import struct, sys
img = open(sys.argv[1], 'r+b')
img.seek(0x1BE)
img.write(struct.pack('<BBBBBBBBII',
    0x80, 0, 2, 0,
    0x0C,
    0, 0, 0,
    128, 15*2048))
img.close()
PY

# 3. Run QEMU.  Give the kernel plenty of time to print its banner
#    (the kernel initialises pmm/paging/heap/vfs/etc, which under TCG
#    can take a good few seconds).
rm -f "$LOG"
timeout 30 qemu-system-x86_64 \
    -drive format=raw,file="$IMG",if=ide \
    -m 256M \
    -display none -serial file:"$LOG" -no-reboot \
    >/dev/null 2>&1 || true

# 4. Assertions.
fail=0

# BL4 Stage 2 milestones.
for want in "[BL4] ELF PT_LOAD segments copied to phys" \
            "[BL4] page tables built at 0x01000000" \
            "[BL4] entering long mode; jumping to kernel _start"; do
    if grep -qF "$want" "$LOG"; then
        printf '  [bl4-boot] serial OK   %s\n' "$want"
    else
        printf '  [bl4-boot] serial MISS %s\n' "$want"
        fail=1
    fi
done

# Kernel-side proof that _start was reached and kmain(boot_info) ran.
if grep -qE 'Hello from AuraLite OS kernel!' "$LOG"; then
    echo "  [bl4-boot] serial OK   kernel banner reached"
else
    echo "  [bl4-boot] serial MISS kernel banner"
    fail=1
fi

# The kernel prints "booted via BIOS" (BL1 change) if boot_info_t
# reached kmain intact and boot_from_uefi = 0.
if grep -qE 'booted via BIOS' "$LOG"; then
    echo "  [bl4-boot] serial OK   boot_info handoff correct (BIOS path)"
else
    echo "  [bl4-boot] serial MISS boot_info handoff"
    fail=1
fi

# HHDM print.
if grep -qE 'HHDM offset: 0xffff800000000000' "$LOG"; then
    echo "  [bl4-boot] serial OK   HHDM offset propagated to kernel"
else
    echo "  [bl4-boot] serial MISS HHDM offset"
    fail=1
fi

# Absence of panic.
if grep -qE '(PANIC|panic:)' "$LOG"; then
    echo "  [bl4-boot] serial FAIL kernel PANIC observed"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "[bl4-boot] PASS -- full BIOS boot chain works end to end"
    exit 0
else
    echo "[bl4-boot] FAIL"
    echo "--- last 60 lines of serial log ---"
    tail -60 "$LOG" | sed 's/^/    /'
    exit 1
fi
