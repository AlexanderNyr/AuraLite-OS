#!/usr/bin/env bash
# test_ipv6_ping6.sh — REALINTERNET_PLAN X7 (IPv6) boot gate.
#
# Asserts the IPv6 landing that is deterministic under QEMU/SLIRP:
#   1. the kernel derives and prints a link-local address from the NIC MAC;
#   2. the offline IPv6 helper self-test (pton/ntop, EUI-64, checksum,
#      echo-responder) reports PASS at boot;
#   3. `ping6 <our-own-link-local>` succeeds (loopback) — the deterministic
#      "ping6 to a link-local address" gate.  QEMU's SLIRP user networking has
#      a long-standing IPv6 filtering limitation (Launchpad #1724590), so the
#      gateway/peer-echo path is documented as a manual D6 run instead of a
#      CI assertion; the self-ping exercises the full ICMPv6 echo path.
#
# The link-local address is a pure function of the MAC, so the expected value
# is deterministic for the default QEMU MAC 52:54:00:12:34:56:
#   fe80::5054:ff:fe12:3456

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "IPv6 (link-local + NDP + ICMPv6 echo)"

LOG="$IL_LOGDIR/ipv6_ping6.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Wait for the shell, then ping our own deterministic link-local address.
il_send_delay 8
il_send "ping6 fe80::5054:ff:fe12:3456"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 25

il_assert_grep_fixed "$LOG" "[net6] link-local fe80::5054:ff:fe12:3456" \
    "kernel derives the link-local address from the NIC MAC"
il_assert_grep_fixed "$LOG" "[net6] self-test PASS" \
    "IPv6 helper self-test (pton/ntop, EUI-64, checksum, echo-responder) passed"
il_assert_grep_fixed "$LOG" "ping6 fe80::5054:ff:fe12:3456" \
    "ping6 command invoked"
il_assert_grep_fixed "$LOG" "Reply received from fe80::5054:ff:fe12:3456" \
    "self-ping of the link-local address answered (deterministic IPv6 echo gate)"
il_assert_no_grep_fixed "$LOG" "[net6] self-test FAIL" \
    "no IPv6 self-test failure"

il_summary
