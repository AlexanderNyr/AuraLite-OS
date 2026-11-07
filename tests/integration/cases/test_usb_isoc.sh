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
# HISTORY, because this assertion has been on both sides:
#   U0 deleted "[isoc] PASS: isoc full support ready" -- printed
#      unconditionally with no transfer ever issued.
#   U9 made the Isoch TRB itself real (type 5 + SIA) and the case asserted a
#      SKIP, honestly: nothing consumed the endpoint.
#   C5 (RES-60) streams a generated 440 Hz sine through the USB audio isoc END
#      endpoint on xHCI and counts what came back, so the assertion flips for
#      the reason the SKIP existed: frames now move, and the count is printed.
il_assert_grep "$LOG" "\[isoc\] PASS: [0-9]+ packet\(s\) streamed on xHCI" \
    "isoc streams packets through a real isochronous endpoint"
il_assert_grep "$LOG" "\[isoc\]   frames: scheduled [0-9]+, completed [0-9]+" \
    "the frame accounting is printed next to the count"
il_assert_grep "$LOG" "\[isoc\] self-test: framework limits and stream accounting" \
    "the framework self-test reports stream accounting"
il_assert_grep "$LOG" "\[isoc\] PASS: isochronous endpoint exercised" \
    "the aggregate isoc receipt agrees with the producer"
il_assert_no_grep "$LOG" "MFINDEX FROZEN" "the isoclock advanced (frame figures mean something)"
il_assert_no_grep "$LOG" "\[xhci\] isoc timeout" "no isochronous packet timed out"
il_assert_grep "$LOG" "ohci.*full support.*PASS" "OHCI isoc full"
# Was "\[xhci\] self-test:.*full support" -- a string that never existed
# after U0.  AUDIT_A4 replaced the stale "(bring-up only)" wording with a
# description of what the driver genuinely does.
il_assert_grep "$LOG" "\[xhci\] self-test:.*control/bulk/interrupt real" \
    "xHCI self-test states what actually works"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|bandwidth exceeded" "no isoc faults"
il_summary
