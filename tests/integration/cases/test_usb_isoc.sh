#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB Isochronous — bandwidth, sync types, isoc TRB"
LOG="$IL_LOGDIR/usb_isoc.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_send_delay 6
il_send "exit"
il_run_qemu "$LOG" 40 \
    -audiodev none,id=audio0 \
    -device "pci-ohci,id=ohci" \
    -device "qemu-xhci,id=xhci" \
    -device "usb-audio,audiodev=audio0,bus=ohci.0" \
    -device "usb-audio,audiodev=audio0,bus=xhci.0"
il_assert_grep "$LOG" "\[isoc\] full isoc framework init" "Isoc framework init"
il_assert_grep "$LOG" "max.*packets/transfer.*transfers.*BW" "Isoc max packets/BW logged"
# AUDIT_A2: USB_PLAN U0 deleted "[isoc] PASS: isoc full support ready".
# It was printed unconditionally, with no isochronous transfer ever issued,
# and U0's whole purpose was to stop the log claiming work it had not done.
# The honest replacement is a SKIP, so that is what this asserts.  U9 made
# the Isoch TRB itself real (type 5 + SIA); proving samples actually move is
# still open, and the SKIP says so.
il_assert_grep "$LOG" "\[isoc\] SKIP: no isochronous endpoint exercised" \
    "isoc reports honestly that no endpoint was exercised"
il_assert_grep "$LOG" "ohci.*full support.*PASS" "OHCI isoc full"
# Was "\[xhci\] self-test:.*full support" -- a string that never existed
# after U0.  AUDIT_A4 replaced the stale "(bring-up only)" wording with a
# description of what the driver genuinely does.
il_assert_grep "$LOG" "\[xhci\] self-test:.*control/bulk/interrupt real" \
    "xHCI self-test states what actually works"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|bandwidth exceeded" "no isoc faults"
il_summary
