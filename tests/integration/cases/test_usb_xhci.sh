#!/usr/bin/env bash
# test_usb_xhci.sh — xHCI slots/contexts/rings with HID + MSC.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB xHCI HID + MSC"

USB="$IL_BUILD/usb-xhci-test.img"
il_make_disk "$USB" 8 "AURALXHC"

LOG="$IL_LOGDIR/usb_xhci.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 40 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-kbd,bus=xhci.0,port=1" \
    -device "usb-mouse,bus=xhci.0,port=2" \
    -drive "file=$USB,format=raw,if=none,id=xhcistick" \
    -device "usb-storage,bus=xhci.0,port=3,drive=xhcistick"

il_assert_grep "$LOG" "\[xhci\] addressed device" "xHCI Address Device command works"
il_assert_grep "$LOG" "\[usb\] addr .*xHCI.*Mass Storage" "xHCI MSC enumerated"
il_assert_grep "$LOG" "\[usb\] addr .*xHCI.*HID" "xHCI HID enumerated"
il_assert_grep "$LOG" "\[hid\] keyboard ready" "xHCI HID keyboard ready"
il_assert_grep "$LOG" "\[hid\] mouse ready"    "xHCI HID mouse ready"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage ready" "xHCI MSC ready"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage READ\(10\) works" \
    "xHCI MSC READ(10) works"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[xhci\].*FAIL|\[msc\] FAIL" \
    "no xHCI/MSC boot faults"

il_summary
