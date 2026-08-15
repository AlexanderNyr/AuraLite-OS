#!/usr/bin/env bash
# test_xhci_address.sh — USB_PLAN U3 gate: real Enable Slot / Address Device.
#
# Before U3 the driver invented slot IDs from a `static uint8_t fake_slot`
# counter and never issued either command, so the controller knew nothing
# about any of it.  The assertions below are chosen so that a fabricated
# implementation cannot satisfy them:
#
#   - "Slot State=Addressed" is read back out of the *device context*, which
#     only the controller writes.  A driver that made the slot up cannot put
#     that value there.
#   - the slot IDs must be distinct and controller-issued.
#   - a device that was never addressed cannot report real VID/PID, so the
#     values are cross-checked against what QEMU was told to emulate.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "xHCI Enable Slot / Address Device (USB_PLAN U3)"

LOG="$IL_LOGDIR/xhci_address.log"
DISK="$IL_LOGDIR/xhci_address_disk.img"
rm -f "$LOG" "$DISK"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

dd if=/dev/zero of="$DISK" bs=1M count=8 status=none

il_run_qemu "$LOG" 45 \
    -device "qemu-xhci,id=xhci" \
    -device "usb-kbd,bus=xhci.0" \
    -device "usb-mouse,bus=xhci.0" \
    -drive "if=none,id=xdisk,file=$DISK,format=raw" \
    -device "usb-storage,bus=xhci.0,drive=xdisk"

# The command ring must still be sound -- everything below depends on it.
il_assert_grep_fixed "$LOG" "[xhci] command ring: No Op -> Success (cc=1)" \
    "U1 command ring still healthy"

# The controller, not the driver, decided these.
il_assert_grep "$LOG" "\[xhci\] slot 1 addressed .*Slot State=Addressed" \
    "slot 1 reports Slot State=Addressed (written by the controller)"
il_assert_grep "$LOG" "\[xhci\] slot 2 addressed .*Slot State=Addressed" \
    "slot 2 reports Slot State=Addressed"
il_assert_grep "$LOG" "\[xhci\] slot 3 addressed .*Slot State=Addressed" \
    "slot 3 reports Slot State=Addressed"

# Three devices, three distinct slots.
n_slots=$(grep -c "\[xhci\] slot . addressed" "$LOG" || true)
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$n_slots" -eq 3 ]; then
    il_pass "three devices addressed, three distinct slots"
else
    il_fail "expected 3 addressed slots, saw $n_slots"
fi

# SuperSpeed EP0 must be 512 bytes.  bMaxPacketSize0 is an exponent for SS
# (09h = 2^9), and taking it literally programmed a 9-byte EP0.
# U9 reworded this line: the location is now reported decoded, as
# "root port N, route=0x..., tier=N", instead of the raw encoded value.
il_assert_grep "$LOG" "slot . addressed \(root port ., route=0x00000, tier=0, speed super-speed .*mps0=512" \
    "SuperSpeed EP0 max packet is 512, not the raw exponent"

# Descriptors now come from the devices: the storage device and the HID
# devices must report *different* vendor IDs, which the deleted forgery
# (identity from dev_addr % 3) could not do for this attach order.
il_assert_grep "$LOG" "class=Mass Storage VID=0x46f4" \
    "mass storage reports its real VID (0x46f4)"
il_assert_grep "$LOG" "class=HID VID=0x0627" \
    "HID devices report their real VID (0x0627)"

# No slot may be enabled and then abandoned.
il_assert_no_grep "$LOG" "Enable Slot returned invalid slot" \
    "no invalid slot IDs"
il_assert_no_grep "$LOG" "not Addressed after Address Device" \
    "no slot left un-addressed"
il_assert_no_grep_fixed "$LOG" "[xhci] Address Device failed" \
    "no Address Device failures"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|PANIC" \
    "no faults"

il_summary
