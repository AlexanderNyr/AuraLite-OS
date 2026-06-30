#!/usr/bin/env bash
# test_virtio_net.sh — virtio-net data path (ARP + ICMP + DNS + TCP).
#
# Background:
#   The networking stack selects a NIC backend through the netdev layer:
#   e1000 by default, falling back to modern virtio-net (1af4:1041) when e1000
#   is absent.  This test swaps the QEMU NIC to virtio-net-pci so the kernel
#   brings up the virtio-net driver and runs the whole IP stack (DHCP/ICMP/
#   DNS/TCP) over it.
#
#   As with the e1000 test, DHCP may not complete under QEMU SLIRP timing.  The
#   test therefore branches:
#     • If DHCP succeeded → assert ping/DNS/TCP all PASSed over virtio-net.
#     • If it fell back to static IP → assert virtio-net at least came up and
#       the stack did not crash.

set -u

# Drive the IP stack over virtio-net instead of the default e1000.
export IL_NIC="virtio-net-pci"

cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "virtio-net (data path: ARP + ICMP + DNS + TCP)"

LOG="$IL_LOGDIR/virtio_net.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 25

# The active NIC must be virtio-net (proves the netdev backend selection and
# that e1000 was genuinely absent under this QEMU NIC model).
il_assert_grep    "$LOG" "\\[virtio-net\\] ready"              "virtio-net brought up"
il_assert_grep    "$LOG" "\\[netdev\\] active NIC: virtio-net" "virtio-net is the active NIC"
il_assert_grep    "$LOG" "\\[net\\] using NIC: virtio-net"     "stack runs over virtio-net"
il_assert_no_grep "$LOG" "\\[tcp\\] FAIL"                      "no TCP failure"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                 "no exception in net path"
il_assert_no_grep "$LOG" "PANIC"                               "no panic in net path"

# Branch on whether DHCP actually succeeded over virtio-net.
if grep -qE '\[dhcp\] PASS:' "$LOG"; then
    echo "  ${C_DIM}(DHCP succeeded over virtio-net → asserting online self-tests)${C_RESET}"
    il_assert_grep "$LOG" "\\[net\\] PASS: ping 10\\.0\\.2\\.2"  "ICMP echo over virtio-net"
    il_assert_grep "$LOG" "(\\[net\\] dns PASS|\\[dns\\] PASS)"  "DNS resolver over virtio-net"
    il_assert_grep "$LOG" "\\[tcp\\] PASS:"                      "TCP connect/send/close over virtio-net"
else
    echo "  ${C_DIM}(DHCP didn't complete → using fallback-IP path)${C_RESET}"
    il_assert_grep "$LOG" "our IP: 10\\.0\\.2\\.15"             "static IP assigned over virtio-net"
fi

il_summary
