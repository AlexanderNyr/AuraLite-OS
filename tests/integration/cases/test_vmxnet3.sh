#!/usr/bin/env bash
# test_vmxnet3.sh — RESIDUE2 T6 gate: the VMware paravirtual NIC data path
# (ledger RES-46's NIC half; e1000.c is the plan's reference driver).
#
# Boots the guest with -device vmxnet3 on the SLIRP user network and
# proves the full data path end to end: PCI discovery → VRRS revision
# handshake → shared-ring ACTIVATE (read-back 0 = device active) → MAC
# → INTx IRQ → DHCP DORA through SLIRP → ARP → ICMP ping → a UDP DNS
# resolve through 10.0.2.3 (the udptest DNS lane).  "Compiles" never
# passes for "works" (plan design rule).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "vmxnet3 paravirtual NIC data path (RESIDUE2 T6)"

LOG="$IL_LOGDIR/vmxnet3.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

IL_NIC="vmxnet3"

il_send_delay 12
il_send "ping 10.0.2.2"
il_send_delay 6
il_send "run udptest"
il_send_delay 10
il_send "echo gate-end"
il_send "exit"

il_run_qemu "$LOG" 70

il_assert_grep "$LOG" "\\[vmxnet3\\] found at" \
    "PCI discovery found the device"
il_assert_grep_fixed "$LOG" "[vmxnet3] device activated (rev 1)" \
    "shared-ring ACTIVATE accepted (read-back 0)"
il_assert_grep "$LOG" "\\[vmxnet3\\] MAC [0-9a-f:]{17}" \
    "the device MAC was read"
il_assert_grep_fixed "$LOG" "[vmxnet3] link up" "link reported up"
il_assert_grep "$LOG" "\\[vmxnet3\\] IRQ line [0-9]+ enabled" \
    "INTx interrupt armed"
il_assert_grep "$LOG" "\\[vmxnet3\\] TX/RX rings initialised \\(tx=128/128 rx=128/256\\)" \
    "all four rings configured"
il_assert_grep_fixed "$LOG" "[net] using NIC: vmxnet3" \
    "the netdev layer selected vmxnet3"
il_assert_grep_fixed "$LOG" "[dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2" \
    "DHCP DORA completed over the new data path"
il_assert_grep "$LOG" "PASS: ping 10.0.2.2 successful" \
    "ICMP round-trip through the rings"
il_assert_grep "$LOG" "UDPTEST PASS|\\[dns\\] PASS" \
    "a UDP DNS resolve rode the RX completions"
il_assert_grep_fixed "$LOG" "gate-end" "the shell survived"
il_assert_no_grep "$LOG" "vmxnet3\\] FAIL|TX ring stuck" \
    "no vmxnet3 failure marker"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_summary
