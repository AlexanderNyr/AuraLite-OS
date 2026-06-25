#!/usr/bin/env bash
# test_gui_usb.sh — USB Manager GUI app reads /usb info and opens cleanly.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "GUI USB Manager"

USB="$IL_BUILD/gui-usb-test.img"
il_make_disk "$USB" 8 "GUIUSB!!"

LOG="$IL_LOGDIR/gui_usb.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run /gusb"

il_run_qemu "$LOG" 35 \
    -device "qemu-xhci,id=xhci" \
    -drive "file=$USB,format=raw,if=none,id=gusbstick" \
    -device "usb-storage,bus=xhci.0,drive=gusbstick"

il_assert_grep "$LOG" "\[usbfs\] device available at /usb" "usbfs media available"
il_assert_grep "$LOG" "running /gusb" "shell launched USB GUI app"
il_assert_grep "$LOG" "\[gusb\] usb info loaded" "USB Manager loaded /usb info"
il_assert_grep "$LOG" "\[elf\].*entry" "GUI app ELF loaded"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|run: failed|\[gusb\] /usb unavailable" \
    "no GUI USB faults"

il_summary
