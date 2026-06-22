#!/usr/bin/env bash
# run_qemu_usb_msc.sh — boot AuraLite with a UHCI USB mass-storage device.
#
# Usage: run_qemu_usb_msc.sh <auralite.iso> [disk.img]
set -euo pipefail

ISO="${1:?usage: $0 <auralite.iso> [disk.img]}"
DISK="${2:-$(dirname "$ISO")/usb-stick.img}"

mkdir -p "$(dirname "$DISK")"
if [ ! -f "$DISK" ]; then
    echo "[usb-msc] creating test USB disk image: $DISK"
    dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
    python3 - "$DISK" <<'PY'
import sys
p=sys.argv[1]
sector=bytearray(512)
sector[0:8]=b'AURALUSB'
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
    -boot order=d \
    -cpu qemu64 \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -usb \
    -drive file="$DISK",format=raw,if=none,id=usbstick \
    -device usb-storage,drive=usbstick
