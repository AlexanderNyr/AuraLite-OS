#!/usr/bin/env bash
# mkvmware.sh — generate a VMware Workstation/Fusion .vmx for AuraLite OS.
#
# Usage: mkvmware.sh <auralite.iso> [output-dir] [vm-name]
#
# The generated VM intentionally uses the legacy Intel e1000 adapter, not
# vmxnet3/e1000e, because AuraLite currently has an 8254x e1000 driver.
set -euo pipefail

ISO="${1:?usage: $0 <auralite.iso> [output-dir] [vm-name]}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${2:-$ROOT/vm/vmware/AuraLite-OS.vmwarevm}"
VM_NAME="${3:-AuraLite-OS}"

if [ ! -f "$ISO" ]; then
    echo "[vmware] ISO not found: $ISO" >&2
    exit 1
fi
ISO_ABS="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"
mkdir -p "$OUT_DIR"
VM_ISO="$OUT_DIR/auralite.iso"
cp "$ISO_ABS" "$VM_ISO"
VMX="$OUT_DIR/$VM_NAME.vmx"
SERIAL_LOG="serial.log"

cat > "$VMX" <<EOF
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "$VM_NAME"
guestOS = "other-64"
firmware = "bios"

memsize = "512"
numvcpus = "4"
cpuid.coresPerSocket = "4"
rtc.diffFromUTC = "0"

# Boot AuraLite from the Limine hybrid ISO.
ide1:0.present = "TRUE"
ide1:0.deviceType = "cdrom-image"
ide1:0.fileName = "auralite.iso"
ide1:0.startConnected = "TRUE"

# Use legacy Intel e1000. Do not switch to vmxnet3/e1000e unless the OS gains
# a driver for that virtual NIC.
ethernet0.present = "TRUE"
ethernet0.connectionType = "nat"
ethernet0.virtualDev = "e1000"
ethernet0.addressType = "generated"
ethernet0.startConnected = "TRUE"

# VMware SVGA exposes a BIOS/GOP framebuffer that Limine can use.
svga.present = "TRUE"
svga.autodetect = "TRUE"
mks.enable3d = "FALSE"

# PS/2 keyboard/mouse are enabled by default; USB is also exposed for the USB
# controller probes in AuraLite.
usb.present = "TRUE"
uhci.present = "TRUE"
ehci.present = "TRUE"

# Capture COM1 output from AuraLite's UART driver.
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "$SERIAL_LOG"
serial0.tryNoRxLoss = "FALSE"

# Keep the VM simple and deterministic.
sound.present = "FALSE"
floppy0.present = "FALSE"
tools.syncTime = "FALSE"
EOF

cat > "$OUT_DIR/README-VMware.txt" <<EOF
AuraLite OS — VMware setup
==========================

Open this VMX in VMware Workstation/Fusion/Player:
  $VMX

ISO:
  $OUT_DIR/auralite.iso

Important compatibility settings:
  guestOS: other-64
  firmware: BIOS
  NIC: ethernet0.virtualDev = e1000
  Network: NAT
  Serial COM1 log: $SERIAL_LOG

Do not change the NIC to vmxnet3 or e1000e unless AuraLite gets a driver for it.
EOF

echo "[vmware] VMX written: $VMX"
echo "[vmware] Bundled ISO: $OUT_DIR/auralite.iso"
echo "[vmware] Serial log will be: $OUT_DIR/$SERIAL_LOG"
