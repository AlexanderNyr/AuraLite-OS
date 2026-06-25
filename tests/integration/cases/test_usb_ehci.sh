#!/usr/bin/env bash
# test_usb_ehci.sh — EHCI high-speed control/bulk data path with MSC.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB EHCI high-speed MSC"

USB="$IL_BUILD/usb-ehci-test.img"
il_make_disk "$USB" 8 "AURALEHC"

LOG="$IL_LOGDIR/usb_ehci.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 35 \
    -device "usb-ehci,id=ehci" \
    -drive "file=$USB,format=raw,if=none,id=ehcistick" \
    -device "usb-storage,bus=ehci.0,drive=ehcistick"

il_assert_grep "$LOG" "\[ehci\] .*high-speed device" "EHCI high-speed port detected"
il_assert_grep "$LOG" "\[usb\] addr .*EHCI"          "EHCI device enumerated"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage ready" "EHCI MSC ready"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage READ\(10\) works" \
    "EHCI MSC READ(10) works"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[ehci\].*FAIL|\[msc\] FAIL" \
    "no EHCI/MSC boot faults"

il_summary
