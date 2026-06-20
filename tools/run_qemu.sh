#!/usr/bin/env bash
# run_qemu.sh — launch NovOS in QEMU with the serial console on stdio.
#
# Usage: run_qemu.sh <novos.iso>
set -euo pipefail
ISO="${1:?usage: $0 <novos.iso>}"

exec qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 512M \
    -vga std \
    -display none \
    -serial stdio \
    -no-reboot \
    -no-shutdown \
    -cpu qemu64
