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
il_assert_grep "$LOG" "\[isoc\] PASS: isoc full support ready" "Isoc PASS"
il_assert_grep "$LOG" "ohci.*full support.*PASS" "OHCI isoc full"
il_assert_grep "$LOG" "\[xhci\] self-test:.*full support" "xHCI isoc full"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|bandwidth exceeded" "no isoc faults"
il_summary
