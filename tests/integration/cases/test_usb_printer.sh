#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB Printer — full support"
LOG="$IL_LOGDIR/usb_printer.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_send_delay 6
il_send "exit"
il_run_qemu "$LOG" 30 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-kbd,bus=xhci.0"
il_assert_grep "$LOG" "\[printer\] full USB Printer driver initialized" "Printer driver init"
# QEMU models NO printer-class (7/1/x) device, so attachment cannot be
# tested here; the honest pins are the driver's registration and its
# explicit skip (the phantom "full support ready" line never existed).
il_assert_grep_fixed "$LOG" "[usb] driver 'usb_printer' registered (class 0x07)" "printer driver registered"
il_assert_grep_fixed "$LOG" "[printer] SKIP: no printers attached" "honest skip (no printer device model in QEMU)"
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no printer faults"
il_summary
