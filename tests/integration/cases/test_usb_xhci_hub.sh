#!/usr/bin/env bash
# test_usb_xhci_hub.sh — xHCI route string / downstream hub enumeration.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB xHCI hub downstream HID enumeration"

LOG="$IL_LOGDIR/usb_xhci_hub.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 35 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-hub,bus=xhci.0,port=1" \
    -device "usb-mouse,bus=xhci.0,port=1.1"

il_assert_grep "$LOG" "\[usb\] addr .*xHCI.*class=Hub" "xHCI hub enumerated"
il_assert_grep "$LOG" "\[hub\] addr .*downstream port" "hub descriptor read"
il_assert_grep "$LOG" "\[hub\] addr .*enumerated .* downstream device" \
    "downstream device enumerated through xHCI route string"
il_assert_grep "$LOG" "\[hid\] mouse ready" "mouse behind xHCI hub ready"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[hub\].*failed|command completion cc=|SET_ADDRESS failed" \
    "no xHCI hub enumeration faults"

il_summary
