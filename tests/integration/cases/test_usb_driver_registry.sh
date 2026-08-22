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
# usb-serial is an FTDI-class device, NOT CDC ACM -- the acm driver
# honestly reports 0 attached; what this REGISTRY case can pin is the
# registration itself (its own theme) plus the honest skip.
il_assert_grep_fixed "$LOG" "[usb] driver 'cdc_acm' registered (class 0x02)" "cdc_acm registered"
il_assert_grep_fixed "$LOG" "[cdc-acm] SKIP: no CDC ACM devices attached" "cdc_acm honest skip (usb-serial is FTDI, not ACM)"
il_assert_grep "$LOG" "\[audio\] PASS" "Audio PASS"
il_assert_grep "$LOG" "\[hub\] PASS" "Hub PASS"
il_assert_grep "$LOG" "\[isoc\] full isoc framework init" "isoc framework initialised (real line)"
il_assert_grep "$LOG" "\[usb-string\] PASS" "String PASS"
il_assert_grep_fixed "$LOG" "[usb] driver 'usb_printer' registered (class 0x07)" "usb_printer registered"
il_assert_grep_fixed "$LOG" "[usb] PASS: 5 device(s) enumerated" "all five attached devices enumerated (real line)"
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no driver registry faults"
il_summary
