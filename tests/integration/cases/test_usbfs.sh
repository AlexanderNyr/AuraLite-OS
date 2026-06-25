#!/usr/bin/env bash
# test_usbfs.sh — /usb VFS view of active USB Mass Storage.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "usbfs VFS mount for USB mass storage"

USB="$IL_BUILD/usbfs-test.img"
il_make_disk "$USB" 8 "USBFS!!!"

LOG="$IL_LOGDIR/usbfs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "ls /usb"
il_send "cat /usb/info"
il_send "exit"

il_run_qemu "$LOG" 40 \
    -device "qemu-xhci,id=xhci" \
    -drive "file=$USB,format=raw,if=none,id=usbstick" \
    -device "usb-storage,bus=xhci.0,drive=usbstick"

il_assert_grep "$LOG" "\[vfs\] mounted '/usb'" "/usb mounted"
il_assert_grep "$LOG" "\[usbfs\] device available at /usb" "usbfs device attach notification"
il_assert_grep "$LOG" "sector0\.bin" "usbfs exposes sector0.bin"
il_assert_grep "$LOG" "disk\.img" "usbfs exposes disk.img"
il_assert_grep "$LOG" "AuraLite usbfs" "cat /usb/info works"
il_assert_grep "$LOG" "status: ready" "usbfs reports ready media"
il_assert_grep "$LOG" "sectors: 16384" "usbfs reports sector count"
il_assert_grep "$LOG" "55 53 42 46 53 21 21 21" "MSC read saw USBFS magic"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|\[msc\] FAIL" "no usbfs/MSC faults"

il_summary
