#!/usr/bin/env bash
# test_usb_ehci_hid.sh — high-speed HID keyboard/mouse via EHCI async polled IN.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB EHCI high-speed HID keyboard/mouse"

LOG="$IL_LOGDIR/usb_ehci_hid.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 35 \
    -device "usb-ehci,id=ehci" \
    -device "usb-kbd,bus=ehci.0,port=1" \
    -device "usb-mouse,bus=ehci.0,port=2"

il_assert_grep "$LOG" "\[usb\] addr .*EHCI.*class=HID" "EHCI HID devices enumerated"
il_assert_grep "$LOG" "\[hid\] parsed generic keyboard report" "EHCI keyboard report parsed"
il_assert_grep "$LOG" "\[hid\] parsed generic pointer report" "EHCI mouse report parsed"
il_assert_grep "$LOG" "\[hid\] keyboard ready" "EHCI keyboard ready"
il_assert_grep "$LOG" "\[hid\] mouse ready"    "EHCI mouse ready"
il_assert_grep "$LOG" "\[hid\] PASS: 2 USB HID input device\(s\) active" "both EHCI HID devices active"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[ehci\].*FAIL|transfer timeout" \
    "no EHCI HID faults"

il_summary
