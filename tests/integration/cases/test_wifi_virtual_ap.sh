#!/usr/bin/env bash
# test_wifi_virtual_ap.sh — RESIDUE2 T6 gate: the Wi-Fi MAC layer's
# reference backend, end to end (ledger RES-39).
#
# QEMU has no 802.11 radio, so a chipset backend can never be QEMU-wire
# gated (D2 loud-skip for silicon).  What this gate pins instead is the
# part that must not rot while waiting for hardware: the boot
# self-test registers wifi_virt (the deterministic virtual AP over the
# wifi_driver_t seam) and drives the FULL management flow — scan
# (probe request → parsed probe response), open authentication,
# association with the AID taken from the wire (transmitted 0xC005,
# masked to 5), and one Ethernet frame through the LLC/SNAP data path.
# The byte layouts themselves are host-pinned by tests/unit/
# test_wifi_proto.c; the host gate runs via `make test-unit`.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Wi-Fi MAC over the virtual AP (RESIDUE2 T6)"

LOG="$IL_LOGDIR/wifi_virtual_ap.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 12
il_send "echo gate-end"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 40

il_assert_grep_fixed "$LOG" "[wifi] PASS: virtual AP: scan found 'AuraVirt' ch6 (parsed from the probe response)" \
    "the scan result came from a parsed probe response"
il_assert_grep_fixed "$LOG" "[wifi] PASS: virtual AP: open-auth + assoc, AID=5 from the wire" \
    "auth + assoc completed with the wire AID (0xC005 masked to 5)"
il_assert_grep_fixed "$LOG" "[wifi] PASS: virtual AP: 78 data bytes through the LLC/SNAP path" \
    "one Ethernet frame rode the data path"
il_assert_no_grep "$LOG" "\\[wifi\\] FAIL" "no Wi-Fi failure marker"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
