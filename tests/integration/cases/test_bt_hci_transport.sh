#!/usr/bin/env bash
# test_bt_hci_transport.sh — RESIDUE3 C6: the Bluetooth HCI transport.
#
# WHAT THE PLAN ASKED FOR, AND WHAT IS ACTUALLY POSSIBLE.
#
# C6's Bluetooth bullet reads: "QEMU's `-device usb-bt-dongle` (CSR-class HCI
# over USB) → a real HCI_Reset/Read_Local_Version/Read_BD_ADDR round trip with
# the controller's own bytes printed; then an inquiry, and a named refusal for
# pairing".  The dongle half cannot be built: QEMU 9.0 deleted its Bluetooth
# subsystem.  Case 1 asserts that measurement instead of assuming it —
# `-bt` is an invalid option and `-device help` lists no bt-* or
# usb-bt-dongle device on this emulator.
#
# So the round trip is proved the way this project proves anything the emulator
# cannot attach: the transport is a seam (bt_transport.h) and the kernel build
# drives it against a scripted CSR-class controller carried in the tree
# (bt_selftest.c).  Case 2 asserts the boot log of that run: three commands
# answered, the controller's own BD_ADDR and version bytes decoded, an inquiry
# returning the two addresses the controller reported, a named refusal for
# pairing, and a negative control (a completion for the wrong opcode is
# refused rather than taken as success).
#
# The host half — ten cases including a silent controller and an all-zero
# BD_ADDR — runs as build/test_bt_transport in `make test-unit`.
#
# The metal half stays RES-61 (PENDING-USER): a real dongle is the user's to
# attach.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Bluetooth HCI transport (RESIDUE3 C6): the emulator has no controller, so one is scripted"

# --- Case 1: the measurement that shapes the phase -------------------------
BTLOG="$IL_LOGDIR/bt_emulator_capability.log"
IL_LAST_LOG="$BTLOG"
{
    echo "== qemu-system-x86_64 -bt help =="
    "$IL_QEMU" -bt help 2>&1 | head -3
    echo "== -device help | grep -i bluetooth =="
    "$IL_QEMU" -device help 2>&1 | grep -i -E "bluetooth|bt-dongle|bt-" || echo "(no Bluetooth device listed)"
} > "$BTLOG" 2>&1

il_assert_grep "$BTLOG" "invalid option" \
    "QEMU has no -bt option: the Bluetooth subsystem it used to expose is gone"
il_assert_grep_fixed "$BTLOG" "(no Bluetooth device listed)" \
    "QEMU lists no usb-bt-dongle / bt-* device, so the dongle case cannot exist"

# --- Case 2: the round trip against the scripted controller ----------------
LOG="$IL_LOGDIR/bt_hci_transport.log"
IL_LAST_LOG="$LOG"
rm -f "$LOG"

set +e
timeout --foreground 80 "$IL_QEMU" \
    -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
    -m 512M -smp 2 -display none \
    -serial "file:$LOG" \
    -no-reboot -cpu qemu64 -boot order=c \
    -nic none >/dev/null 2>&1
set -e

il_assert_grep_fixed "$LOG" "[bt] no Bluetooth controller found" \
    "the driver says plainly that no controller is attached"
il_assert_grep_fixed "$LOG" "[bt] selftest: bring-up -> 3 command(s), HCI version code 0x0c (5.3) rev 0x0f23, manufacturer 0x001d (CSR)" \
    "Reset + Read Local Version + Read BD_ADDR answered; version and manufacturer decoded"
il_assert_grep_fixed "$LOG" "[bt] selftest: BD_ADDR bc:9a:12:34:56:78 read from a Command Complete" \
    "the BD_ADDR is the controller's own answer, in wire order"
il_assert_grep_fixed "$LOG" "[bt] selftest: inquiry -> 2 device(s), GIAC LAP 9e8b33, first 66:55:44:33:22:11" \
    "the inquiry sent the GIAC LAP and returned the addresses the controller reported"
il_assert_grep "$LOG" "\\[bt\\] selftest: pairing -> refused \\(pairing is out of scope" \
    "pairing is a named refusal, not a silent gap"
il_assert_grep_fixed "$LOG" "[bt] selftest: wrong-opcode control -> refused (-1)" \
    "a completion for the wrong opcode is refused instead of accepted"
il_assert_grep_fixed "$LOG" "[bt] selftest: PASS HCI transport round trip (no controller attached; QEMU has no Bluetooth device)" \
    "the transport round trip passes in the kernel build"
il_assert_no_grep_fixed "$LOG" "[bt] selftest: FAIL" "no failed self-test check"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"

# --- Case 3: the host half is registered and green -------------------------
# A case that only greps a boot log would not notice the unit test rotting.
if [ -x "$IL_ROOT/build/test_bt_transport" ]; then
    export PATH="/home/user/.cargo/bin:$PATH"
    set +e
    OUT=$("$IL_ROOT/build/test_bt_transport" 2>&1)
    set -e
    printf '%s\n' "$OUT" > "$IL_LOGDIR/bt_transport_unit.log"
    IL_LAST_LOG="$IL_LOGDIR/bt_transport_unit.log"
    il_assert_grep "$IL_LOGDIR/bt_transport_unit.log" "Results: 10/10 passed, 0 failed" \
        "the host test (silent controller, all-zero BD_ADDR, wrong opcode) is green"
else
    IL_LAST_LOG="$LOG"
    il_fail "build/test_bt_transport is missing (make test-unit builds it)"
fi

il_summary
