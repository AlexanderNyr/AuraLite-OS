#!/usr/bin/env bash
# test_usb_hub_depth.sh — USB_PLAN U9: a device two hubs deep.
#
# The old device-location encoding packed the whole topology into one byte:
# root port in the high nibble, one hub port in the low nibble.  A hub two
# levels down therefore computed its OWN location for its children --
# usb_loc_child(0x51, 1) == 0x51 -- so usb_find_by_location() matched the
# hub itself and the device behind it was silently dropped as a duplicate.
# The symptom was a boot that enumerated both hubs, never the keyboard, and
# looped over the same two ports forever.
#
# This case pins the fix: three tiers, each with its own slot, and the
# keyboard bound at the bottom.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Nested hubs: a device two hubs deep (USB_PLAN U9)"

LOG="$IL_LOGDIR/usb_hub_depth.log"
rm -f "$LOG"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_run_qemu "$LOG" 55 \
    -device "qemu-xhci,id=x" \
    -device "usb-hub,bus=x.0,port=1,id=h1" \
    -device "usb-hub,bus=x.0,port=1.1,id=h2" \
    -device "usb-kbd,bus=x.0,port=1.1.1"

# Both hubs and the keyboard must all appear, with distinct addresses.
il_assert_grep "$LOG" "\[usb\] addr 1: xHCI .*class=Hub" \
    "first-tier hub enumerated"
il_assert_grep "$LOG" "\[usb\] addr 2: xHCI .*class=Hub" \
    "second-tier hub enumerated (not collapsed onto the first)"
il_assert_grep "$LOG" "\[usb\] addr 3: xHCI .*class=HID VID=0x0627" \
    "the keyboard two hubs deep is enumerated"

# Route strings: the controller must be told the real topology.  tier=N is
# the number of downstream hub ports in the location, and the route string
# gains one 4-bit digit per tier.
il_assert_grep "$LOG" "slot 1 addressed .*route=0x00000, tier=0" \
    "tier-0 hub has an empty route string"
il_assert_grep "$LOG" "slot 2 addressed .*route=0x00001, tier=1" \
    "tier-1 hub routes through one hub port"
il_assert_grep "$LOG" "slot 3 addressed .*route=0x00011, tier=2" \
    "the keyboard routes through two hub ports"

# All three must be genuinely addressed by the controller.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$(grep -c 'Slot State=Addressed' "$LOG")" -ge 3 ]; then
    il_pass "three slots confirmed Addressed by the controller"
else
    il_fail "fewer than three slots reached the Addressed state"
fi

il_assert_grep "$LOG" "\[hid\] keyboard ready: addr=3" \
    "HID bound to the device at the bottom of the chain"

il_assert_no_grep "$LOG" "Address Device failed|child enumeration failed|deeper than the USB limit" \
    "no addressing or depth failures"
il_assert_no_grep "$LOG" "Page Fault|PANIC" \
    "no faults"

il_summary
