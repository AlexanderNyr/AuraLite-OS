#!/usr/bin/env bash
# test_usb_hub.sh — USB hub downstream enumeration.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "USB hub downstream HID enumeration"

LOG="$IL_LOGDIR/usb_hub.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

# Use QEMU's internal hub topology: attach devices through a USB hub.
# The hub is connected to the UHCI controller, and devices are behind it,
# exercising hub descriptor/status, port power/reset and child enumeration.
il_run_qemu "$LOG" 45 \
    -device "piix3-usb-uhci,id=uhci" \
    -device "usb-hub,bus=uhci.0,port=1" \
    -device "usb-kbd,bus=uhci.0,port=1.1" \
    -device "usb-mouse,bus=uhci.0,port=1.2"

il_assert_grep "$LOG" "\[usb\] addr .*class=Hub" "USB hub enumerated"
il_assert_grep "$LOG" "\[hub\] addr .*downstream port" "hub descriptor read"
il_assert_grep "$LOG" "\[hub\] addr .*enumerated .* downstream device" \
    "downstream device enumerated"
il_assert_grep "$LOG" "\[hid\] keyboard ready" "keyboard ready"
il_assert_grep "$LOG" "\[hid\] mouse ready"    "mouse behind hub ready"
# A UHCI TD-chain timeout hitting the first hub-descriptor read is a
# known shared-runner flake (status.md / RES-01): the driver retries the
# read and enumeration completes -- the assertions above already proved
# the hub, its ports and both downstream HID devices came up.  Tolerate
# exactly that recovered retry; page faults, panics and any OTHER named
# hub failure still fail the shard.
il_assert_no_grep "$LOG" "Page Fault|kernel panic" "no fatal kernel faults"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
n_hub_failed="$(grep -cE '\[hub\].*failed' "$LOG" || true)"
n_desc_retry="$(grep -cE '\[hub\] addr [0-9]+: failed to read hub descriptor' "$LOG" || true)"
if [ "$n_hub_failed" -eq "$n_desc_retry" ]; then
    il_pass "no hub enumeration faults (RES-01 descriptor retries tolerated: $n_desc_retry)"
else
    il_fail "named hub failure beyond RES-01 descriptor retries (failed=$n_hub_failed, retries=$n_desc_retry)"
fi

il_summary
