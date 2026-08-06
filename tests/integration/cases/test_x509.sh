#!/usr/bin/env bash
# test_x509.sh — INTERNET_PLAN.md phase N2 in-guest gate.
#
# The field-level RFC battery runs host-side (tests/unit/test_atls_x509.c,
# 61 checks).  This case boots the OS and runs /tests/x509test, which is
# the half of the gate that MUST run in QEMU: the hostile inputs (every
# truncated prefix, a bit-flip battery, indefinite/huge lengths, and a
# 10 000-deep nesting) are thrown at the parser while it sits on the
# guest's real 64 KiB user stack.  If the parser recursed over attacker
# nesting instead of walking it iteratively, this is where it would die
# on the stack guard page — the test asserts no exception and ALL PASS.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N2: X.509 parsing in-guest gate"

LOG="$IL_LOGDIR/x509.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run x509test"
il_send_delay 15
il_send "exit"

il_run_qemu "$LOG" 70

il_assert_grep "$LOG" "auralite#"                      "shell reached"
il_assert_grep "$LOG" "\[x509test\] PASS: example.com leaf parses in guest"   "real leaf parses"
il_assert_grep_fixed "$LOG" "[x509test] PASS: local RSA CA: pathlen + keyUsage correct" "local CA fields"
il_assert_grep "$LOG" "\[x509test\] PASS: every truncated prefix refused"       "truncation sweep"
il_assert_grep "$LOG" "\[x509test\] PASS: bit-flip battery"                     "bit-flip battery"
il_assert_grep "$LOG" "\[x509test\] PASS: indefinite length refused"            "indefinite length refused"
il_assert_grep "$LOG" "\[x509test\] PASS: 4 GiB length claim refused"           "huge length refused"
il_assert_grep "$LOG" "\[x509test\] PASS: 10 000-deep nesting refused"          "10 000-deep nesting refused at depth budget"
il_assert_grep "$LOG" "\[x509test\] PASS: 20-deep nesting accepted"             "modest nesting accepted"
il_assert_grep "$LOG" "\[x509test\] ALL PASS"                                   "x509test reports ALL PASS"

il_assert_no_grep "$LOG" "\[x509test\] FAIL"     "no in-guest X.509 failure"
il_assert_no_grep "$LOG" "KERNEL EXCEPTION"      "no kernel exception (no stack death)"
il_assert_no_grep "$LOG" "GUARD"                 "no stack guard page hit"
il_assert_no_grep "$LOG" "PANIC"                 "no kernel panic"

il_summary
