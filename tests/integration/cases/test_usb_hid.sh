#!/usr/bin/env bash
# test_usb_hid.sh — USB HID boot keyboard + mouse enumeration/polling path.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB HID keyboard/mouse (UHCI boot protocol)"

LOG="$IL_LOGDIR/usb_hid.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

# Explicit UHCI + root-port assignment avoids QEMU's legacy `-usb` auto-hub,
# which can hide the second device behind a hub AuraLite does not yet drive.
il_run_qemu "$LOG" 25 \
    -device "piix3-usb-uhci,id=uhci" \
    -device "usb-kbd,bus=uhci.0,port=1" \
    -device "usb-mouse,bus=uhci.0,port=2"

il_assert_grep "$LOG" "\[usb\]   interface 0: class=0x03 .*proto=0x01" \
    "USB boot keyboard interface enumerated"
il_assert_grep "$LOG" "\[usb\]   interface 0: class=0x03 .*proto=0x02" \
    "USB boot mouse interface enumerated"
il_assert_grep "$LOG" "\[hid\] keyboard ready" "HID keyboard driver active"
il_assert_grep "$LOG" "\[hid\] mouse ready"    "HID mouse driver active"
il_assert_grep "$LOG" "\[hid\] PASS: 2 USB HID input device\(s\) active" \
    "both HID input devices active"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[hid\].*FAIL" \
    "no HID boot faults"

il_summary
