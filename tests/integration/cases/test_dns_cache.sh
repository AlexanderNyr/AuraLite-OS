#!/usr/bin/env bash
# test_dns_cache.sh — REALINTERNET_PLAN phase X3 gate: DNS cache, failover,
# CNAME chase, and the new shell commands (dnscache/dnsset/dnsflush).
#
# What this run exercises, in order:
#   1. boot: kernel logs '[dns] init: ...' and the DNS self-test PASSes
#      (regression: old behaviour must survive).
#   2. shell: two consecutive 'nslookup example.com' — first must be a
#      '[dns] cache MISS', the repeat a '[dns] cache HIT' (served without
#      a query while the TTL is valid).
#   3. shell: 'dnscache' prints the server list and the cache snapshot,
#      'dnsset' blackholes the primary server, 'dnsflush' clears the cache.
#   4. shell: with the primary blackholed, 'nslookup db2.test' must log
#      'trying secondary' and fail visibly (fail-closed, D7).
#   5. best-effort: 'nslookup www.wikipedia.org' through its alias chain
#      logs 'CNAME' on most days (lenient, like test_networking.sh: answer
#      shape depends on the day/TTL=0 answers under SLIRP).
#
# Manual real-internet witness for this phase: build/X3_manual_dns_run.txt.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "X3 DNS reliability (cache + failover + CNAME)"

LOG="$IL_LOGDIR/dns_cache.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Boot reprobes everything; then shell exercises the cache in order.
il_send "nslookup example.com"
il_send_delay 2
il_send "nslookup example.com"
il_send_delay 1
il_send "nslookup www.wikipedia.org"
il_send_delay 2
il_send "dnscache"
il_send "dnsset 10.0.2.97 10.0.2.3"
il_send "dnsflush"
il_send "nslookup db2.test"
il_send_delay 2
il_send "nslookup db2.test"
il_send_delay 1
il_send "dnsset 10.0.2.3"
il_send "exit"

il_run_qemu "$LOG" 90

# --- boot-visible wiring (required) -------------------------------
il_assert_grep    "$LOG" "\\[dns\\] init: cache"            "dns_init ran at boot"
il_assert_grep    "$LOG" "\\[dns\\] PASS: example.com"      "boot DNS self-test passes (regression)"

# --- cache behaviour (required) -----------------------------------
il_assert_grep    "$LOG" "cache MISS 'example.com'"         "first lookup is a cache MISS"
il_assert_grep    "$LOG" "cache HIT 'example.com'"          "repeat lookup is a cache HIT"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"               "no exception in the DNS path"

# --- shell commands (required) ------------------------------------
il_assert_grep    "$LOG" "DNS servers \\("                    "dnscache printed the server list"
il_assert_grep    "$LOG" "DNS cache \\("                      "dnscache printed the cache snapshot"
il_assert_grep    "$LOG" "dnsflush: cache cleared"          "dnsflush ran"

# --- failover, fail-closed (required) -----------------------------
il_assert_grep    "$LOG" "trying secondary"                 "blackholed primary drove failover"
# db2.test does not exist: the visible result must be a failure, not a
# bogus address.  (NXDOMAIN reaches us via the secondary or all-silent.)
il_assert_grep    "$LOG" "nslookup: failed to resolve db2.test" "NXDOMAIN/all-silent fails visibly"

# --- best-effort day-shape checks (lenient, per test_networking.sh) --
if grep -q "CNAME" "$LOG"; then
    il_pass "CNAME chain chased/logged (www.wikipedia.org)"
else
    echo "  ${C_DIM}(no CNAME record seen today — direct A answer; lenient)${C_RESET}"
fi

il_summary
