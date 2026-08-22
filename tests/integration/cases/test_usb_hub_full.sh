#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB Hub full — multi-tier, TT, SS, power, interrupt EP"
LOG="$IL_LOGDIR/usb_hub_full.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_send_delay 8
il_send "exit"
il_run_qemu "$LOG" 50 \
    -chardev null,id=cdc0 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-hub,bus=xhci.0" \
    -device "usb-kbd,bus=xhci.0" \
    -device "usb-mouse,bus=xhci.0" \
    -device "usb-serial,chardev=cdc0,bus=xhci.0"
il_assert_grep "$LOG" "\[hub\] full hub driver initialized.*max.*hubs.*ports.*depth" "Hub driver full init with max"
il_assert_grep "$LOG" "\[xhci\] starting port scan" "xHCI port scan"
il_assert_grep "$LOG" "device attached" "Device attached"
# The kernel's REAL pass line (a phantom "full support ready" phrase
# was asserted here for months and never existed -- caught the first
# time this AUDIT_A0 case ran on CI, then reproduced locally).
il_assert_grep_fixed "$LOG" "[hub] PASS: 1 hub(s) attached" "Hub PASS (real line: the attached hub counted)"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|hub.*failed|TD chain timeout" "no hub faults"
il_summary
