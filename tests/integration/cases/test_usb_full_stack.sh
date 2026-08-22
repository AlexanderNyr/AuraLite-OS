#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64
il_section "USB Full Stack — all controllers + all classes"
LOG="$IL_LOGDIR/usb_full_stack.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
USB="$IL_BUILD/usb-full-stack.img"
il_make_disk "$USB" 16 "FULLUSB!"
il_send_delay 8
il_send "exit"
il_run_qemu "$LOG" 50 \
    -chardev null,id=cdc0 \
    -audiodev none,id=audio0 \
    -device "piix3-usb-uhci,id=uhci" \
    -device "pci-ohci,id=ohci" \
    -device "usb-ehci,id=ehci" \
    -device "qemu-xhci,id=xhci" \
    -device "usb-kbd,bus=uhci.0" \
    -device "usb-mouse,bus=uhci.0" \
    -device "usb-serial,chardev=cdc0,bus=ohci.0" \
    -device "usb-audio,audiodev=audio0,bus=xhci.0" \
    -drive "file=$USB,format=raw,if=none,id=usbstick" \
    -device "usb-storage,bus=xhci.0,drive=usbstick"
il_assert_grep "$LOG" "\[uhci\] controller running" "UHCI running"
il_assert_grep "$LOG" "\[ohci\] controller at PCI" "OHCI found"
il_assert_grep "$LOG" "\[ehci\] controller running" "EHCI running"
il_assert_grep "$LOG" "\[xhci\] controller running" "xHCI running"
il_assert_grep "$LOG" "\[xhci\] starting port scan" "xHCI port scan"
il_assert_grep "$LOG" "device attached" "Device attached in port scan"
il_assert_grep "$LOG" "\[usb\] enumerating devices across all controllers" "enumeration sweep ran (real line)"
il_assert_grep "$LOG" "cdc_acm.*registered" "CDC ACM driver registered"
il_assert_grep "$LOG" "usb_audio.*registered" "Audio driver registered"
il_assert_grep "$LOG" "usb_printer.*registered" "Printer driver registered"
il_assert_grep "$LOG" "usb_hub.*registered" "Hub driver registered"
il_assert_grep "$LOG" "\[usb-string\] full string descriptor driver ready" "String driver ready"
il_assert_grep "$LOG" "\[hub\] full hub driver initialized" "Hub driver full init"
il_assert_grep "$LOG" "\[isoc\] full isoc framework init" "Isoc framework init"
il_assert_grep "$LOG" "\[cdc-acm\] full CDC ACM driver initialized" "CDC ACM full init"
il_assert_grep "$LOG" "\[audio\] full USB Audio driver initialized" "Audio full init"
il_assert_grep "$LOG" "\[printer\] full USB Printer driver initialized" "Printer full init"
il_assert_grep_fixed "$LOG" "[usb] PASS: 5 device(s) enumerated" "all five attached devices enumerated (real line)"
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no faults"
il_summary
