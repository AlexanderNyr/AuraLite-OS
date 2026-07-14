#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB Audio (usb-audio) — UAC1 isoc"
LOG="$IL_LOGDIR/usb_audio_full.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_send_delay 6
il_send "exit"
il_run_qemu "$LOG" 35 \
    -audiodev none,id=audio0 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-audio,audiodev=audio0,bus=xhci.0"
il_assert_grep "$LOG" "\[xhci\] controller running" "xHCI running"
il_assert_grep "$LOG" "\[audio\] full USB Audio driver initialized" "Audio driver init"
il_assert_grep "$LOG" "\[isoc\] full isoc framework init" "Isoc framework for audio"
il_assert_grep "$LOG" "\[audio\] PASS: USB Audio full support ready" "Audio PASS"
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no audio faults"
il_summary
