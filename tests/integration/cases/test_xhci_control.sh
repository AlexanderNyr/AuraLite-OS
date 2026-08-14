#!/usr/bin/env bash
# test_xhci_control.sh — USB_PLAN U4 gate: real control transfers.
#
# U3 made Address Device real, which let the previously-shadowed TRB path
# run.  U4 hardens it: short packets via the event residue, stall recovery,
# and the full-speed EP0 re-read.
#
# The assertions are chosen so the deleted forgery could not have passed
# them.  It answered GET_DESCRIPTOR from hardcoded arrays and guessed the
# device's identity with `dev_addr % 3`, so it could produce neither a
# per-device product string nor a correctly parsed configuration with real
# endpoint addresses and packet sizes.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "xHCI control transfers (USB_PLAN U4)"

LOG="$IL_LOGDIR/xhci_control.log"
DISK="$IL_LOGDIR/xhci_control_disk.img"
rm -f "$LOG" "$DISK"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

dd if=/dev/zero of="$DISK" bs=1M count=8 status=none

il_run_qemu "$LOG" 45 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-kbd,bus=xhci.0" \
    -device "usb-mouse,bus=xhci.0" \
    -drive "if=none,id=xdisk,file=$DISK,format=raw" \
    -device "usb-storage,bus=xhci.0,drive=xdisk"

# String descriptors come from the devices.  The forgery returned the
# literal "QEMU" for every string index of every device; these three are
# distinct and none of them is that.
il_assert_grep_fixed "$LOG" "product: 'QEMU USB HARDDRIVE'" \
    "storage reports its own product string"
il_assert_grep_fixed "$LOG" "product: 'QEMU USB Keyboard'" \
    "keyboard reports its own product string"
il_assert_grep_fixed "$LOG" "product: 'QEMU USB Mouse'" \
    "mouse reports its own product string"

# Configuration descriptors are fetched in two steps -- 9 bytes to learn
# wTotalLength, then the whole thing -- and parsed.  This is the read that
# a bogus residue silently truncated to zero.
il_assert_grep "$LOG" "config 1: 1 interfaces, 44 bytes" \
    "storage configuration descriptor parsed (44 bytes)"
il_assert_grep "$LOG" "config 1: 1 interfaces, 34 bytes" \
    "HID configuration descriptor parsed (34 bytes)"

# Endpoints with real addresses, types and packet sizes.
il_assert_grep "$LOG" "endpoint 0x81: bulk IN, maxpkt=1024" \
    "storage bulk IN endpoint decoded, SuperSpeed packet size"
il_assert_grep "$LOG" "endpoint 0x02: bulk OUT, maxpkt=1024" \
    "storage bulk OUT endpoint decoded"
il_assert_grep "$LOG" "endpoint 0x81: interrupt IN, maxpkt=8" \
    "keyboard interrupt endpoint decoded"
il_assert_grep "$LOG" "endpoint 0x81: interrupt IN, maxpkt=4" \
    "mouse interrupt endpoint decoded"

# Classes now come from the interface descriptors, not from dev_addr % 3.
il_assert_grep "$LOG" "class=Mass Storage VID=0x46f4" \
    "storage classified from its interface descriptor"
il_assert_grep "$LOG" "class=HID VID=0x0627" \
    "HID devices classified from their interface descriptors"

# The class drivers can act on what enumeration found.
il_assert_grep "$LOG" "\[msc\] mass storage candidate: addr=. .* bulk_in=0x81 bulk_out=0x02" \
    "MSC found real bulk endpoints"
il_assert_grep "$LOG" "\[hid\] keyboard ready: addr=. iface=0 ep=0x81" \
    "HID bound the keyboard"
il_assert_grep "$LOG" "\[hid\] mouse ready: addr=. iface=0 ep=0x81" \
    "HID bound the mouse"

il_assert_no_grep "$LOG" "transfer timeout|endpoint stalled|Page Fault|PANIC" \
    "no timeouts, stalls or faults"

il_summary
