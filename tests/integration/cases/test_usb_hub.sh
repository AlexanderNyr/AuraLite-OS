#!/usr/bin/env bash
# test_usb_hub.sh — USB hub downstream enumeration.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB hub downstream HID enumeration"

LOG="$IL_LOGDIR/usb_hub.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

# QEMU's legacy `-usb` topology commonly places one HID device on a root port
# and the second behind a full-speed hub. This exercises hub descriptor/status,
# port power/reset and child enumeration.
il_run_qemu "$LOG" 35 \
    -usb \
    -device "usb-kbd" \
    -device "usb-mouse"

il_assert_grep "$LOG" "\[usb\] addr .*class=Hub" "USB hub enumerated"
il_assert_grep "$LOG" "\[hub\] addr .*downstream port" "hub descriptor read"
il_assert_grep "$LOG" "\[hub\] addr .*enumerated .* downstream device" \
    "downstream device enumerated"
il_assert_grep "$LOG" "\[hid\] keyboard ready" "keyboard ready"
il_assert_grep "$LOG" "\[hid\] mouse ready"    "mouse behind hub ready"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[hub\].*failed|\[uhci\] TD chain timeout" \
    "no hub enumeration faults"

il_summary
