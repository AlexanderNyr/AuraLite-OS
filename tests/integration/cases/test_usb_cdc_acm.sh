#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB CDC ACM (usb-serial)"
LOG="$IL_LOGDIR/usb_cdc_acm.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_send_delay 6
il_send "exit"
il_run_qemu "$LOG" 35 \
    -chardev null,id=cdc0 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-serial,chardev=cdc0,bus=xhci.0"
il_assert_grep "$LOG" "\[xhci\] controller running" "xHCI running"
il_assert_grep "$LOG" "\[xhci\] starting port scan" "Port scan"
il_assert_grep "$LOG" "\[cdc-acm\] full CDC ACM driver initialized" "CDC ACM driver init"
il_assert_grep "$LOG" "cdc_acm.*registered" "CDC ACM registered"
il_assert_grep "$LOG" "\[cdc-acm\] PASS: CDC ACM full support ready" "CDC ACM PASS"
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no CDC faults"
il_summary
