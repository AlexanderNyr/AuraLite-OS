#!/usr/bin/env bash
# mkisoimage.sh — assemble a hybrid BIOS/UEFI bootable ISO containing the
# AuraLite OS kernel plus the Limine bootloader, then install Limine's BIOS stages.
#
# Usage: mkisoimage.sh <kernel.elf> <out.iso> <limine_dir>
set -euo pipefail

KERNEL_ELF="${1:?usage: $0 <kernel.elf> <out.iso> <limine_dir>}"
ISO_IMAGE="${2:?}"
LIMINE_DIR="${3:?}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO_ROOT="$ROOT/build/iso_root"

echo "[mkisoimage] staging ISO tree at $ISO_ROOT"
rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"

cp -v "$KERNEL_ELF"                 "$ISO_ROOT/boot/kernel.elf"
cp -v "$ROOT/boot/limine/limine.conf"   "$ISO_ROOT/boot/limine/"
cp -v "$LIMINE_DIR/limine-bios.sys"    "$ISO_ROOT/boot/limine/"
cp -v "$LIMINE_DIR/limine-bios-cd.bin" "$ISO_ROOT/boot/limine/"
cp -v "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_ROOT/boot/limine/"
cp -v "$LIMINE_DIR/BOOTX64.EFI"        "$ISO_ROOT/EFI/BOOT/"

echo "[mkisoimage] creating ISO with xorriso (El Torito + EFI)"
xorriso -as mkisofs -quiet \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -apm-block-size 2048 \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "$ISO_IMAGE"

echo "[mkisoimage] installing Limine BIOS boot stages"
"$LIMINE_DIR/limine" bios-install "$ISO_IMAGE"

echo "[mkisoimage] wrote $ISO_IMAGE ($(du -h "$ISO_IMAGE" | cut -f1))"
