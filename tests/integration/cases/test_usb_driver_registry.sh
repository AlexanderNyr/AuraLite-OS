#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB Driver Registry — probe/disconnect"
LOG="$IL_LOGDIR/usb_driver_registry.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
USB="$IL_BUILD/usb-driver-reg.img"
il_make_disk "$USB" 8 "DRVREG!!"
il_send_delay 8
il_send "exit"
il_run_qemu "$LOG" 40 \
    -chardev null,id=cdc0 \
    -audiodev none,id=audio0 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-kbd,bus=xhci.0" \
    -device "usb-mouse,bus=xhci.0" \
    -device "usb-serial,chardev=cdc0,bus=xhci.0" \
    -device "usb-audio,audiodev=audio0,bus=xhci.0" \
    -drive "file=$USB,format=raw,if=none,id=usbstick" \
    -device "usb-storage,bus=xhci.0,drive=usbstick"
il_assert_grep "$LOG" "cdc_acm.*registered" "CDC ACM driver registered"
il_assert_grep "$LOG" "usb_audio.*registered" "Audio driver registered"
il_assert_grep "$LOG" "usb_printer.*registered" "Printer driver registered"
il_assert_grep "$LOG" "usb_hub.*registered" "Hub driver registered"
il_assert_grep "$LOG" "\[cdc-acm\] PASS" "CDC ACM PASS"
il_assert_grep "$LOG" "\[audio\] PASS" "Audio PASS"
il_assert_grep "$LOG" "\[hub\] PASS" "Hub PASS"
il_assert_grep "$LOG" "\[isoc\] PASS" "Isoc PASS"
il_assert_grep "$LOG" "\[usb-string\] PASS" "String PASS"
il_assert_grep "$LOG" "\[printer\] PASS" "Printer PASS"
il_assert_grep "$LOG" "\[usb\] PASS: full USB stack ready" "Full stack PASS"
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no driver registry faults"
il_summary
