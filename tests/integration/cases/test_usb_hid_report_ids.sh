#!/usr/bin/env bash
# test_usb_hid_report_ids.sh — RESIDUE3 C5b: HID descriptors with report IDs.
#
# WHY THIS EXISTS
#
# The HID driver used to remember ONE report ID: the last `0x85` item in the
# report descriptor.  Everything about the driver then assumed that ID: which
# reports to accept, and where the report body starts.  A device that declares
# two report IDs (a very ordinary thing — a keyboard that also sends consumer
# volume keys under ID 2) parsed into a single blob, so report 1 was either
# dropped (its ID did not match the remembered one) or, worse, decoded with
# report 2's field layout.
#
# C5b splits the descriptor parser out into drivers/usb/hid_report.c, gives
# every field its own report_id and its own bit origin, and keeps a per-report
# body length.  Two independent checks cover that:
#
#   1. tests/unit/test_usb_hid_report.c — eight descriptors, host-compiled
#      against the same translation unit the kernel links.
#   2. the kernel-side selftest below — proves the *kernel build* reaches the
#      same conclusions, including the two-ID split and per-report decoding.
#
# Why not a multi-ID *device*: none of QEMU's HID devices declare a Report ID
# item at all.  That was measured, not assumed — dumping the descriptors sent
# by usb-kbd, usb-tablet and usb-mouse shows zero 0x85 items (receipt in
# docs/plans/RESIDUE3_PLAN.md, section C5b).  A device case here would have
# tested the single-report path and called it multi-ID coverage.
#
# What the real devices ARE used for: the parsed input length must equal the
# endpoint wMaxPacketSize, and that is asserted per device.  It also pins the
# bug this phase found on its own: output-report bits folded into the input
# length made QEMU's 9-byte keyboard look like an 8-byte packet with a reserved
# byte and an LED report, i.e. it computed 10 bytes for a 9-byte endpoint.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "HID report descriptors: report IDs, bodies, wire lengths (RESIDUE3 C5b)"

run_case() {
    # $1 = label, $2.. = qemu device arguments
    local label="$1"; shift
    local log="$IL_LOGDIR/hid_rid_${label}.log"
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

# --- Case 1: kernel-side selftest, no USB device needed -------------------
# The selftest runs from usb_hid_init(), so it executes even on a machine with
# no HID hardware.  That is deliberate: the multi-ID contract must hold on a
# bare boot, not only when a particular device happens to be attached.
SLOG=$(run_case selftest)
IL_LAST_LOG="$SLOG"

il_assert_grep "$SLOG" "\[hid\] selftest: two-ID descriptor -> ids=2 \(1,2\)" \
    "selftest: one descriptor yields two report IDs, both remembered"
il_assert_grep "$SLOG" "input bits: r1=64 r2=4 wire=9" \
    "selftest: per-report body sizes (8 bytes under ID 1, 4 bits under ID 2) and one wire length"
il_assert_grep "$SLOG" "\[hid\] selftest: QEMU-shaped keyboard -> report_id=none input bits=64 wire=8" \
    "selftest: LED output report shares the ID but does NOT lengthen the input report"
il_assert_grep "$SLOG" "\[hid\] selftest: PASS report-ID split and per-report decoding" \
    "selftest: body split after the prefix, per-report decode, unknown ID rejected"
il_assert_no_grep "$SLOG" "\[hid\] selftest: FAIL" \
    "selftest: no failed check"

# --- Case 2: three real devices, parsed length == endpoint packet size -----
# kbd: 8 mods + 1 reserved + 6 keys = 64 bits = 8 bytes (endpoint 8).
# tablet: 3 buttons + 5 pad + X 16 + Y 16 + wheel 8 = 48 bits = 6 bytes (6).
# mouse: 5 buttons + 3 pad + X 8 + Y 8 + wheel 8 = 32 bits = 4 bytes (4).
DLOG=$(run_case devices -device qemu-xhci,id=x \
                            -device usb-kbd,bus=x.0 \
                            -device usb-mouse,bus=x.0 \
                            -device usb-tablet,bus=x.0)
IL_LAST_LOG="$DLOG"

il_assert_grep "$DLOG" "\[hid\] parsed generic keyboard report: fields=14 input_bits=64 report_id=none" \
    "QEMU keyboard: 14 fields, 64 input bits (LED output bits excluded)"
il_assert_grep "$DLOG" "\[hid\] parsed generic pointer report: fields=8 input_bits=32 report_id=none" \
    "QEMU mouse: 8 fields, 32 input bits"
il_assert_grep "$DLOG" "\[hid\] parsed generic pointer report: fields=6 input_bits=48 report_id=none" \
    "QEMU tablet: 6 fields including the wheel, 48 input bits"
il_assert_grep "$DLOG" "\[hid\] keyboard ready: addr=. iface=0 ep=0x81 poll_len=8 ep_maxpkt=8" \
    "QEMU keyboard: polled length 8 exactly fills the 8-byte endpoint"
il_assert_grep "$DLOG" "\[hid\] mouse ready: addr=. iface=0 ep=0x81 poll_len=4 ep_maxpkt=4" \
    "QEMU mouse: polled length 4 exactly fills the 4-byte endpoint"
il_assert_grep "$DLOG" "\[hid\] generic-mouse ready: addr=. iface=0 ep=0x81 poll_len=6 ep_maxpkt=8" \
    "QEMU tablet: 6-byte descriptor inside an 8-byte endpoint (device over-allocates)"
il_assert_no_grep "$DLOG" "\[hid\] note: addr .* descriptor wire length" \
    "no descriptor asks for more input than one packet can carry"
il_assert_grep "$DLOG" "\[hid\] PASS: 3 USB HID input device\(s\) active" \
    "all three devices bound through the report-ID-aware parser"

il_summary
