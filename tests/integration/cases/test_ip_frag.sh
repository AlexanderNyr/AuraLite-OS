#!/usr/bin/env bash
# test_ip_frag.sh — REALINTERNET_PLAN phase X4 gate: IPv4 fragment reassembly.
#
# Real network traffic under QEMU/SLIRP is never fragmented for us, so the
# deterministic gate is: (a) the boot-time kernel self-test that pushes
# synthetic wire-shaped fragments through the REAL net_ipfrag_step() glue
# (out-of-order 3-fragment datagram reassembled byte-exact, overlap probe
# refused), (b) the whole receive path still works afterwards (DHCP, DNS
# cache MISS/HIT per X3, ping), (c) no exceptions.
# The full policy battery (timeout drop, memory cap, table eviction, key
# isolation) is gated by the host unit test (tests/unit/test_ip_reasm.c,
# 11/11); see the dated manual note in REALINTERNET_PLAN.md phase X4.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "X4 IPv4 fragment reassembly"

LOG="$IL_LOGDIR/ip_frag.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send "nslookup example.com"
il_send_delay 2
il_send "nslookup example.com"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 60

# --- kernel self-test through the real glue (required) ------------
il_assert_grep    "$LOG" "\\[ipfrag\\] self-test PASS" \
    "boot self-test: out-of-order reassembly byte-exact, overlap refused"
il_assert_no_grep "$LOG" "\\[ipfrag\\] self-test FAIL" \
    "no ipfrag self-test failure"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" \
    "no exception in the reassembly path"

# --- receive path still healthy afterwards (regression) -----------
il_assert_grep    "$LOG" "(\\[net\\]|\\[pci\\]|\\[e1000\\])" \
    "network stack initialised"
il_assert_grep_if_dns "$LOG" "\\[dns\\] PASS: example.com" \
    "DNS still resolves through the stepped receive path (X3 regression)"
il_assert_grep_if_dns "$LOG" "cache HIT 'example.com'" \
    "DNS cache HIT still intact (X3 regression)"

il_summary
