#!/usr/bin/env bash
# test_usb_generic_hid.sh — generic HID report descriptor parser (QEMU tablet).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB generic HID report parser (tablet)"

LOG="$IL_LOGDIR/usb_generic_hid.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 35 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-tablet,bus=xhci.0,port=1"

il_assert_grep "$LOG" "\[usb\]   HID descriptor: report_len=" "HID descriptor parsed from config"
il_assert_grep "$LOG" "\[hid\] parsed generic pointer report" "generic report descriptor parsed"
il_assert_grep "$LOG" "\[hid\] generic-mouse ready" "generic HID tablet registered as mouse"
il_assert_grep "$LOG" "\[hid\] PASS: 1 USB HID input device\(s\) active" "generic HID active"
il_assert_no_grep "$LOG" "unsupported generic HID|Page Fault|kernel panic|\[xhci\].*FAIL" \
    "no generic HID faults"

il_summary
