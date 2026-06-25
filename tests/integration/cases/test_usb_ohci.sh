#!/usr/bin/env bash
# test_usb_ohci.sh — OHCI control/bulk/interrupt data paths.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB OHCI HID + MSC"

USB="$IL_BUILD/usb-ohci-test.img"
il_make_disk "$USB" 8 "AURALOHC"

LOG="$IL_LOGDIR/usb_ohci.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 35 \
    -device "pci-ohci,id=ohci" \
    -device "usb-kbd,bus=ohci.0,port=1" \
    -device "usb-mouse,bus=ohci.0,port=2" \
    -drive "file=$USB,format=raw,if=none,id=ohcistick" \
    -device "usb-storage,bus=ohci.0,port=3,drive=ohcistick"

il_assert_grep "$LOG" "\[ohci\] PASS: .* USB device" "OHCI root ports detected"
il_assert_grep "$LOG" "\[hid\] keyboard ready"          "OHCI HID keyboard ready"
il_assert_grep "$LOG" "\[hid\] mouse ready"             "OHCI HID mouse ready"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage ready" "OHCI MSC ready"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage READ\(10\) works" \
    "OHCI MSC READ(10) works"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[ohci\].*FAIL|\[msc\] FAIL" \
    "no OHCI/MSC boot faults"

il_summary
