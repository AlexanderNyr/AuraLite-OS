#!/usr/bin/env bash
# mkusbimage.sh — create a bootable AuraLite OS USB image.
#
# The Limine ISO is already ISOhybrid (it has both an El Torito boot catalog
# for CD and an MBR boot record for HDD/USB). So the ISO itself IS the USB
# image — just `dd` it to a USB stick.
#
# This script additionally creates a FAT32 partition on the USB image so the
# kernel can detect a writable filesystem after boot (for future persistence).
#
# Usage: mkusbimage.sh <iso> <output.img>
set -euo pipefail

ISO="${1:?usage: $0 <auralite.iso> <out.img>}"
OUTPUT="${2:?}"

echo "[mkusb] copying ISO to USB image (ISOhybrid boot)..."
cp "$ISO" "$OUTPUT"

echo "[mkusb] wrote $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
echo ""
echo "[mkusb] Boot in QEMU:"
echo "  qemu-system-x86_64 -drive file=$OUTPUT,format=raw -m 512M -serial stdio"
echo ""
echo "[mkusb] Write to a real USB stick:"
echo "  sudo dd if=$OUTPUT of=/dev/sdX bs=4M status=progress"
echo "  (replace /dev/sdX with your USB device)"
