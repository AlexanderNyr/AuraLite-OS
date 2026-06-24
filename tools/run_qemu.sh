#!/usr/bin/env bash
# run_qemu.sh — launch AuraLite OS in QEMU with the serial console on stdio.
#
# Usage: run_qemu.sh <auralite.iso>
#
# Two AHCI disks are attached:
#   port 0 -> $BUILD/disk.img   (used by /fat = FAT32 + tiny diskfs at /disk)
#   port 1 -> $BUILD/ext2.img   (used by /ext2 = ext2)
#
# Both images are auto-created on first run and survive reboots so that
# persistent filesystem state (FAT32 logs, ext2 contents) is preserved.
set -euo pipefail
ISO="${1:?usage: $0 <auralite.iso>}"
BUILD="$(dirname "$ISO")"
DISK0="$BUILD/disk.img"
DISK1="$BUILD/ext2.img"

if [ ! -f "$DISK0" ]; then
    echo "[qemu] creating AHCI test disk: $DISK0" >&2
    dd if=/dev/zero of="$DISK0" bs=1M count=16 status=none
    python3 - "$DISK0" <<'PY'
import sys
p=sys.argv[1]
sector=bytearray(512)
sector[0:8]=b'AURALHCI'
sector[510]=0x55
sector[511]=0xAA
with open(p,'r+b') as f:
    f.write(sector)
PY
fi

if [ ! -f "$DISK1" ]; then
    echo "[qemu] creating ext2 test disk: $DISK1" >&2
    dd if=/dev/zero of="$DISK1" bs=1M count=8 status=none
    # If mkfs.ext2 is available, pre-format with a small standard ext2 so
    # AuraLite uses its "mount existing" path.  Otherwise the kernel will
    # format it on first mount.
    for p in /sbin/mkfs.ext2 /usr/sbin/mkfs.ext2; do
        if [ -x "$p" ]; then
            "$p" -q -b 1024 -I 128 -F "$DISK1" 2>/dev/null || true
            break
        fi
    done
fi

exec qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 512M \
    -smp 4 \
    -vga std \
    -display none \
    -serial stdio \
    -no-reboot \
    -no-shutdown \
    -cpu qemu64 \
    -boot order=d \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -drive file="$DISK0",format=raw,if=none,id=ahcidisk \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=ahcidisk,bus=ahci0.0 \
    -drive file="$DISK1",format=raw,if=none,id=ext2disk \
    -device ide-hd,drive=ext2disk,bus=ahci0.1
