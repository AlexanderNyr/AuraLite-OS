#!/usr/bin/env bash
# test_e1000e.sh — RESIDUE2 T6 gate: the Intel 82574L (e1000e) data path
# (ledger RES-46's NIC half, together with vmxnet3).
#
# Boots the guest with -device e1000e on the SLIRP user network. The
# 82574L keeps the 8254x register file and the legacy 16-byte descriptor
# formats (RFCTL.EXTEN clear), so the driver is the e1000.c data path
# pointed at 8086:10D3 with the MAC read from RAL/RAH (no EERD under
# QEMU). Proves: PCI discovery → MMIO → link → MAC → INTx IRQ →
# DHCP DORA → ARP → ICMP ping → a UDP DNS resolve through 10.0.2.3.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "e1000e (82574L) NIC data path (RESIDUE2 T6)"

LOG="$IL_LOGDIR/e1000e.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

IL_NIC="e1000e"

il_send_delay 12
il_send "ping 10.0.2.2"
il_send_delay 7
il_send "run udptest"
il_send_delay 10
il_send "echo gate-end"
il_send "exit"

il_run_qemu "$LOG" 70

il_assert_grep "$LOG" "\\[e1000e\\] found 82574L \\(0x10d3\\) at PCI" \
    "PCI discovery found the device"
il_assert_grep "$LOG" "\\[e1000e\\] MAC from RAL/RAH" \
    "the MAC came from the reset-loaded RAL/RAH (no EERD)"
il_assert_grep "$LOG" "\\[e1000e\\] MAC [0-9a-f:]{17}" "MAC printed"
il_assert_grep "$LOG" "STATUS=0x[0-9a-f]+ \\(LU=1\\)" "link reported up"
il_assert_grep "$LOG" "\\[e1000e\\] IRQ line [0-9]+ enabled \\(IMS=0x[0-9a-f]+\\)" \
    "INTx interrupt armed"
il_assert_grep "$LOG" "\\[e1000e\\] TX/RX rings initialised, RCTL=0x[0-9a-f]+" \
    "rings initialised"
il_assert_grep_fixed "$LOG" "[net] using NIC: e1000e" \
    "the netdev layer selected e1000e"
il_assert_grep_fixed "$LOG" "[dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2" \
    "DHCP DORA completed over the new data path"
il_assert_grep "$LOG" "PASS: ping 10.0.2.2 successful" \
    "ICMP round-trip through the rings"
il_assert_grep "$LOG" "UDPTEST PASS" \
    "a UDP DNS resolve rode the RX path"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived"
il_assert_no_grep "$LOG" "e1000e\\] FAIL|TX timeout" \
    "no e1000e failure marker"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
