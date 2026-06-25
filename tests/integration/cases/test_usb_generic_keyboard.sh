#!/usr/bin/env bash
# test_usb_generic_keyboard.sh — generic keyboard HID report parser path.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB generic keyboard HID report parser"

LOG="$IL_LOGDIR/usb_generic_keyboard.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

# QEMU usb-kbd is boot-capable, but the driver now parses its report descriptor
# and uses the generic keyboard path instead of relying only on Boot Protocol.
il_run_qemu "$LOG" 30 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-kbd,bus=xhci.0,port=1"

il_assert_grep "$LOG" "\[usb\]   HID descriptor: report_len=" "keyboard HID descriptor parsed"
il_assert_grep "$LOG" "\[hid\] parsed generic keyboard report" "generic keyboard report parsed"
il_assert_grep "$LOG" "\[hid\] keyboard ready" "keyboard registered"
il_assert_grep "$LOG" "\[hid\] PASS: 1 USB HID input device\(s\) active" "keyboard HID active"
il_assert_no_grep "$LOG" "unsupported generic HID|Page Fault|kernel panic|\[xhci\].*FAIL" \
    "no generic keyboard HID faults"

il_summary
