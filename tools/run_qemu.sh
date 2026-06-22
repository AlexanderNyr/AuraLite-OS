#!/usr/bin/env bash
# run_qemu.sh — launch AuraLite OS in QEMU with the serial console on stdio.
#
# Usage: run_qemu.sh <auralite.iso>
set -euo pipefail
ISO="${1:?usage: $0 <auralite.iso>}"
DISK="$(dirname "$ISO")/disk.img"

if [ ! -f "$DISK" ]; then
    echo "[qemu] creating AHCI test disk: $DISK" >&2
    dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
    python3 - "$DISK" <<'PY'
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
    -drive file="$DISK",format=raw,if=none,id=ahcidisk \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=ahcidisk,bus=ahci0.0
