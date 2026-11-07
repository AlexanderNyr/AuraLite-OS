#!/usr/bin/env bash
# test_usb_ehci_split.sh — RESIDUE3 C5b: EHCI split transactions, and the
# measurement that says why the split branch has no device case.
#
# WHAT C5b FOUND
#
# The EHCI periodic path built its queue head with two shortcuts:
#   - port speed was a stub that returned "high-speed" for every port, so a
#     full-speed device on a high-speed root port was enumerated as high-speed
#     (`enumerate_device(USB_CTRL_EHCI, p, USB_SPEED_HIGH)` in usb_core.c) and
#     its QH said eps=high-speed;
#   - the split fields were computed inline, and the C-mask for a start in
#     micro-frame 7 came out empty, so that endpoint would never complete.
#
# Both are fixed: the port's PORTSC.PSPD is decoded, the speed travels into
# enumeration and into the QH, and the capability words are built by
# drivers/usb/ehci_qh.c, which tests/unit/test_usb_ehci_qh.c pins bit by bit
# (including all eight start micro-frames).
#
# WHY THERE IS NO "TT DEVICE" CASE
#
# The plan asked for an FS/LS device behind a high-speed hub on EHCI.  QEMU
# cannot express that: its only hub is `usb-hub`, a full-speed device, and QEMU
# refuses to attach it to a high-speed bus.  Case 2 asserts that refusal
# directly, so the gap is a measured property of the emulator rather than a
# case that was quietly dropped.  What CAN be measured on QEMU is the branch
# that matters for the machine in front of you: a full-speed device on an EHCI
# root port, which must be full-speed with no split (a split without a TT
# address would be a lie to the controller).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "EHCI: port speed, split decision, and the TT gap (RESIDUE3 C5b)"

run_case() {
    local label="$1"; shift
    local log="$IL_LOGDIR/ehci_split_${label}.log"
    rm -f "$log"
    IL_LAST_LOG="$log"
    set +e
    timeout --foreground 75 "$IL_QEMU" \
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
        -m 512M -smp 2 -display none \
        -serial "file:$log" \
        -no-reboot -cpu qemu64 -boot order=c \
        "$@" >/dev/null 2>&1
    set -e
    echo "$log"
}

# --- Case 1: full-speed keyboard on an EHCI root port ---------------------
ELOG=$(run_case fsport -device usb-ehci,id=e -device usb-kbd,bus=e.0)
IL_LAST_LOG="$ELOG"

il_assert_grep "$ELOG" "\[ehci\] port 0: full-speed device \(PSPD=0\)" \
    "EHCI: PORTSC.PSPD is decoded, not assumed (a full-speed device is not high-speed)"
il_assert_grep "$ELOG" "\[usb\] device at addr .: .* speed=full \(12 Mbps\)" \
    "EHCI: the port's speed reaches enumeration (used to be hardcoded high-speed)"
il_assert_grep "$ELOG" "\[ehci\] interrupt endpoint 0x81 dev=. on periodic schedule .*eps=0, split=no" \
    "EHCI: queue head carries eps=full and no split (no TT on a root port)"
il_assert_grep "$ELOG" "S-mask=0x01 C-mask=0x00" \
    "EHCI: S-mask set for the periodic schedule, C-mask empty without a TT"
il_assert_no_grep "$ELOG" "\[ehci\]   TT:" \
    "EHCI: no TT receipt is printed for a root-port device"
# Delivery itself is asserted by test_usb_hid_input.sh case 3, which injects
# keystrokes; QEMU's keyboard sends nothing on its own, so asserting on a
# report line here would pass only by accident.  What belongs here is that the
# corrected queue head does not break the poll: no qTD errors, no timeouts.
il_assert_no_grep "$ELOG" "interrupt qTD error|async qTD timeout" \
    "EHCI: the corrected queue head does not error out on every poll"
il_assert_grep "$ELOG" "\[hid\] PASS: 1 USB HID input device\(s\) active" \
    "EHCI: the keyboard stays bound after the speed correction"
il_assert_no_grep "$ELOG" "PANIC|Page Fault" \
    "EHCI: no faults on the periodic path"

# --- Case 2: QEMU refuses to build the TT topology ------------------------
# This is the receipt for the missing device case: a full-speed hub cannot sit
# on a high-speed bus, so no FS/LS device can be placed behind a TT on EHCI.
rm -f "$IL_LOGDIR/ehci_split_hub.log"
set +e
HUBOUT=$(timeout --foreground 20 "$IL_QEMU" \
    -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
    -m 512M -smp 2 -display none -serial null \
    -no-reboot -cpu qemu64 -boot order=c \
    -device usb-ehci,id=e -device usb-hub,bus=e.0,port=1 \
    -device usb-kbd,bus=e.0,port=1.2 2>&1)
HUBRC=$?
set -e
printf '%s\n' "$HUBOUT" > "$IL_LOGDIR/ehci_split_hub.log"

IL_ASSERT_COUNT=$(( ${IL_ASSERT_COUNT:-0} + 1 ))
if [ "$HUBRC" -ne 0 ]; then
    il_pass "TT topology: QEMU exits rather than attach a full-speed hub to a high-speed bus"
else
    il_fail "TT topology: QEMU exits rather than attach a full-speed hub to a high-speed bus (rc=$HUBRC)"
fi
il_assert_grep "$IL_LOGDIR/ehci_split_hub.log" "speed mismatch trying to attach usb device .QEMU USB Hub. .full speed. to bus .e.0., port .1. .high speed." \
    "TT topology: the refusal names the reason (full-speed hub, high-speed bus)"
il_assert_grep "$IL_LOGDIR/ehci_split_hub.log" "usb-hub,bus=e.0,port=1" \
    "TT topology: the command line that cannot be built is recorded verbatim"

il_summary
