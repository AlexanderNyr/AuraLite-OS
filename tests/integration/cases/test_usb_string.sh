#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB String descriptors"
LOG="$IL_LOGDIR/usb_string.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_send_delay 6
il_send "exit"
il_run_qemu "$LOG" 30 \
    -chardev null,id=cdc0 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-serial,chardev=cdc0,bus=xhci.0"
il_assert_grep "$LOG" "\[usb-string\] full string descriptor driver ready" "String driver ready"
il_assert_grep "$LOG" "\[usb-string\] PASS" "String PASS"
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no string faults"
il_summary
