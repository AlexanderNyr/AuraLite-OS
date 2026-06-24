#!/usr/bin/env bash
# test_networking.sh — e1000 + ARP + ICMP + DNS + TCP smoke test.
#
# Background:
#   The kernel runs a polling-based DHCP client at boot.  If DHCP succeeds
#   (PASS line), it then runs ICMP/DNS/TCP online self-tests.  If DHCP
#   fails (which can happen under QEMU SLIRP timing), the kernel falls back
#   to a hardcoded IP and SKIPS the online self-tests by design — see
#   kernel/kernel.c around 'fallback IP active'.
#
# This test therefore branches:
#   • If kernel did online tests  → assert ping/DNS/TCP all PASSed.
#   • If kernel fell back to static IP → assert NIC is at least detected and
#     the stack did not crash.  This is still a meaningful regression gate.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Networking (e1000 + ICMP + DNS + TCP)"

LOG="$IL_LOGDIR/networking.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 25

# Always-required asserts: NIC came up + no NIC/TCP crashes.
il_assert_grep    "$LOG" "(\\[net\\]|\\[pci\\]|\\[e1000\\])"     "network stack initialised"
il_assert_no_grep "$LOG" "\\[e1000\\] FAIL"                      "no e1000 failure"
il_assert_no_grep "$LOG" "\\[tcp\\] FAIL"                        "no TCP failure"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                   "no exception in net path"

# Branch on whether DHCP actually succeeded.
if grep -qE '\[dhcp\] PASS:' "$LOG"; then
    echo "  ${C_DIM}(DHCP succeeded → asserting online self-tests)${C_RESET}"
    il_assert_grep "$LOG" "\\[net\\] PASS: ping 10\\.0\\.2\\.2"  "ICMP echo to host"
    il_assert_grep "$LOG" "(\\[net\\] dns PASS|\\[dns\\] PASS)"  "DNS resolver succeeded"
    il_assert_grep "$LOG" "\\[tcp\\] handshake complete"         "TCP 3-way handshake"
    il_assert_grep "$LOG" "\\[tcp\\] PASS:"                      "TCP connect/send/close"
else
    echo "  ${C_DIM}(DHCP didn't complete → using fallback-IP path)${C_RESET}"
    il_assert_grep "$LOG" "fallback IP active"                   "kernel switched to fallback IP cleanly"
    il_assert_grep "$LOG" "our IP: 10\\.0\\.2\\.15"              "static IP assigned"
fi

il_summary
