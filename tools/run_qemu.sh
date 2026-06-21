#!/usr/bin/env bash
# run_qemu.sh — launch AuraLite OS in QEMU with the serial console on stdio.
#
# Usage: run_qemu.sh <auralite.iso>
set -euo pipefail
ISO="${1:?usage: $0 <auralite.iso>}"

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
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -drive file="$(dirname "$ISO")/disk.img",if=none,id=ahcidisk \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=ahcidisk,bus=ahci0.0
