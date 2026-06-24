#!/usr/bin/env bash
# test_usb_msc.sh — USB Mass Storage sector read via UHCI.
#
# Attaches a QEMU usb-storage device on a UHCI controller, asserts the kernel
# enumerates it, INQUIRY/READ-CAPACITY succeed, and READ(10) sector 0 returns
# our magic.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB MSC sector read (UHCI)"

USB="$IL_BUILD/usb-msc-test.img"
il_make_disk "$USB" 8 "AURALUSB"

LOG="$IL_LOGDIR/usb_msc.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

# `-usb` + `usb-uhci` ensures we use the controller AuraLite's UHCI backend
# drives.  QEMU >= 6 keeps the legacy `-usb` switch which adds piix3-usb-uhci.
il_run_qemu "$LOG" 25 \
    -usb \
    -drive "file=$USB,format=raw,if=none,id=usbstick" \
    -device "usb-storage,drive=usbstick"

il_assert_grep "$LOG" "\[uhci\] PASS: .* USB device"        "UHCI device enumeration"
il_assert_grep "$LOG" "\[usb\] PASS:"                       "USB core enumeration"
il_assert_grep "$LOG" "\[msc\] capacity: .* sectors"        "MSC READ CAPACITY ok"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage ready" "MSC ready"
il_assert_grep "$LOG" "\[msc\] PASS: USB mass storage READ\(10\) works" \
                                                            "MSC READ(10) sector 0"

# No transfer errors.
il_assert_no_grep "$LOG" "\[msc\] FAIL"                     "no MSC failure"
il_assert_no_grep "$LOG" "\[uhci\] FAIL"                    "no UHCI failure"

il_summary
