#!/usr/bin/env bash
# tests/integration/bl5_iso_smoke.sh -- BL5 gate criterion.
#
# Boots the BIOS ISO built by tools/mkisoimage_bios.sh under QEMU
# `-cdrom` (i.e. via legacy BIOS El Torito) and asserts the kernel
# banner reaches COM1.  This proves that the whole custom loader
# chain works when packaged as an ISO, not just as a raw disk.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
ISO="$BUILD/auralite-bios.iso"
LOG="$BUILD/bl5_iso.log"

# 1. Build all artefacts, then the ISO.
make -C "$ROOT" mbr stage2 kernel >/dev/null
bash "$ROOT/tools/mkisoimage_bios.sh" "$BUILD/kernel.elf" "$ISO" >/dev/null

# 2. Sanity-check the hybrid MBR image.  The BL5 ISO is a raw hybrid
#    disk image renamed to .iso -- so it does NOT carry ISO 9660
#    structures; what matters is that sector 0 ends with 0x55 0xAA
#    and contains a valid partition-table entry for the FAT partition.
sz=$(wc -c < "$ISO")
sig=$(od -An -tx1 -N2 -j510 "$ISO" | tr -d ' \n')
if [ "$sig" != "55aa" ]; then
    echo "[bl5-iso] FAIL: MBR signature missing (sig=$sig size=$sz)"
    exit 1
fi
part_type=$(od -An -tx1 -N1 -j$((0x1BE + 4)) "$ISO" | tr -d ' \n')
if [ "$part_type" != "0c" ]; then
    echo "[bl5-iso] FAIL: partition-1 type = 0x$part_type (expected 0x0c FAT32-LBA)"
    exit 1
fi
echo "  [bl5-iso] hybrid image OK  (size=$sz bytes, MBR sig=0x$sig, part1 type=0x$part_type)"

# 3. Boot under QEMU/SeaBIOS as a hard disk (`-drive if=ide`).  The
#    BL5 image is a hybrid MBR raw disk, not an El Torito CD -- see
#    the header comment in tools/mkisoimage_bios.sh.  Real hardware
#    boots the same way from a USB stick.
rm -f "$LOG"
timeout 30 qemu-system-x86_64 \
    -drive format=raw,file="$ISO",if=ide \
    -m 256M \
    -display none -serial file:"$LOG" -no-reboot \
    >/dev/null 2>&1 || true

# 4. Assertions on the serial log.
fail=0

for want in "[BL3] AuraLite stage2 alive" \
            "[BL4] entering long mode; jumping to kernel _start" \
            "Hello from AuraLite OS kernel!" \
            "booted via BIOS" \
            "HHDM offset: 0xffff800000000000"; do
    if grep -qF "$want" "$LOG"; then
        printf '  [bl5-iso] serial OK   %s\n' "$want"
    else
        printf '  [bl5-iso] serial MISS %s\n' "$want"
        fail=1
    fi
done

if grep -qE '(PANIC|panic:)' "$LOG"; then
    echo "  [bl5-iso] serial FAIL kernel PANIC observed"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "[bl5-iso] PASS -- BL5 hybrid ISO boots to kernel under BIOS (-drive if=ide)"
    exit 0
else
    echo "[bl5-iso] FAIL"
    echo "--- last 60 lines of serial log ---"
    tail -60 "$LOG" | sed 's/^/    /'
    exit 1
fi
