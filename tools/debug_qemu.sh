#!/usr/bin/env bash
# debug_qemu.sh — boot AuraLite OS paused, waiting for a GDB attach on port 1234.
#
# Usage: debug_qemu.sh <auralite.iso>
# Then in another terminal:
#   gdb build/kernel.elf -ex "target remote :1234" -ex "hbreak kmain" -ex "c"
set -euo pipefail
ISO="${1:?usage: $0 <auralite.iso>}"

echo "[debug] QEMU waiting for GDB on localhost:1234 (kernel start halted)."
exec qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 512M \
    -vga std \
    -display none \
    -serial stdio \
    -S -s \
    -no-reboot \
    -cpu qemu64
