#!/usr/bin/env bash
# mkvbox.sh — create/configure a VirtualBox VM for AuraLite OS.
#
# Usage: mkvbox.sh <auralite.iso> [vm-name]
#
# The important compatibility choices are:
#   - BIOS firmware: Limine BIOS path is well tested.
#   - VBoxSVGA graphics: provides a VBE framebuffer for Limine.
#   - Intel PRO/1000 MT Desktop (82540EM): supported by AuraLite's e1000 driver.
#   - COM1 redirected to serial.log for kernel/debug output.
set -euo pipefail

ISO="${1:?usage: $0 <auralite.iso> [vm-name]}"
VM_NAME="${2:-AuraLite-OS}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$ROOT/vm/virtualbox"
mkdir -p "$OUT_DIR"

if [ ! -f "$ISO" ]; then
    echo "[vbox] ISO not found: $ISO" >&2
    exit 1
fi
ISO_ABS="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"
VM_ISO="$OUT_DIR/auralite.iso"
cp "$ISO_ABS" "$VM_ISO"
VM_ISO_ABS="$(cd "$(dirname "$VM_ISO")" && pwd)/$(basename "$VM_ISO")"

INSTRUCTIONS="$OUT_DIR/README-VirtualBox.txt"
cat > "$INSTRUCTIONS" <<EOF
AuraLite OS — VirtualBox setup
==============================

ISO: $VM_ISO_ABS

Recommended manual settings if you do not use VBoxManage:
  Type: Other / Other 64-bit
  RAM: 512 MiB or more
  CPUs: 4
  Firmware: BIOS (not required, but the best-tested path)
  Chipset: PIIX3
  Graphics Controller: VBoxSVGA
  Video RAM: 32 MiB
  Optical Drive: $VM_ISO_ABS
  Network Adapter 1: NAT
  Adapter Type: Intel PRO/1000 MT Desktop (82540EM)
  Serial Port 1: COM1, file: serial.log

Important: do NOT select PCnet, virtio-net, e1000e, or virtio graphics unless
AuraLite has a matching driver. For networking choose the Intel PRO/1000 MT
Desktop adapter.
EOF

if ! command -v VBoxManage >/dev/null 2>&1; then
    echo "[vbox] VBoxManage not found; wrote manual setup notes: $INSTRUCTIONS"
    exit 0
fi

if VBoxManage showvminfo "$VM_NAME" >/dev/null 2>&1; then
    echo "[vbox] VM '$VM_NAME' already exists; updating ISO + core settings."
else
    VBoxManage createvm --name "$VM_NAME" --ostype Other_64 \
        --basefolder "$OUT_DIR" --register >/dev/null
fi

# Core VM settings.
VBoxManage modifyvm "$VM_NAME" \
    --memory 512 \
    --cpus 4 \
    --ioapic on \
    --pae on \
    --firmware bios \
    --chipset piix3 \
    --rtcuseutc on \
    --boot1 dvd --boot2 none --boot3 none --boot4 none \
    --graphicscontroller vboxsvga \
    --vram 32 \
    --audio none \
    --usb on --usbohci on --usbehci on \
    --nic1 nat \
    --nictype1 82540EM \
    --cableconnected1 on \
    --uart1 0x3F8 4 \
    --uartmode1 file "$OUT_DIR/serial.log" >/dev/null

# Create/replace the IDE controller used for the boot ISO.
if ! VBoxManage showvminfo "$VM_NAME" --machinereadable | grep -q '^storagecontrollername0="IDE"'; then
    VBoxManage storagectl "$VM_NAME" --name IDE --add ide --controller PIIX4 --bootable on >/dev/null 2>&1 || true
fi
VBoxManage storageattach "$VM_NAME" --storagectl IDE --port 1 --device 0 \
    --type dvddrive --medium none >/dev/null 2>&1 || true
VBoxManage storageattach "$VM_NAME" --storagectl IDE --port 1 --device 0 \
    --type dvddrive --medium "$VM_ISO_ABS" >/dev/null

# Optional AHCI disk so the AHCI driver sees a virtual SATA controller/disk.
if ! VBoxManage showvminfo "$VM_NAME" --machinereadable | grep -q 'storagecontrollername.*="SATA"'; then
    VBoxManage storagectl "$VM_NAME" --name SATA --add sata --controller IntelAhci --portcount 1 >/dev/null 2>&1 || true
fi
DISK="$OUT_DIR/auralite-ahci.vdi"
if [ ! -f "$DISK" ]; then
    VBoxManage createmedium disk --filename "$DISK" --size 64 --format VDI >/dev/null
fi
VBoxManage storageattach "$VM_NAME" --storagectl SATA --port 0 --device 0 \
    --type hdd --medium "$DISK" >/dev/null 2>&1 || true

echo "[vbox] VirtualBox VM ready: $VM_NAME"
echo "[vbox] Start with: VBoxManage startvm '$VM_NAME'"
echo "[vbox] Serial log: $OUT_DIR/serial.log"
echo "[vbox] Notes: $INSTRUCTIONS"
