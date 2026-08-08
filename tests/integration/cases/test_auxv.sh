#!/usr/bin/env bash
# test_auxv.sh -- M5 (MATURITY_PLAN.md) auxiliary-vector gate.
#
# Until M5 the kernel's auxv held only an AT_NULL terminator, so getauxval()
# returned 0 for every type: no AT_PAGESZ, no AT_RANDOM (the 16-byte seed a
# stack-canary crt0 wants), no AT_EXECFN, no AT_PHDR/AT_ENTRY.  /auxvtest reads
# the auxv via getauxval() and checks the entries a real program (and a future
# dynamic loader) depend on.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "M5 auxiliary vector (auxvtest / getauxval)"

LOG="$IL_LOGDIR/auxv.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run auxvtest"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_grep    "$LOG" "AUXV PASS"                "getauxval returned the expected entries"
il_assert_grep    "$LOG" "\\[auxv\\] pagesz=4096"    "AT_PAGESZ == 4096"
il_assert_no_grep "$LOG" "AUXV FAIL"                 "no auxv entry missing/wrong"
il_assert_no_grep "$LOG" "\\[auxv\\] FAIL"           "no per-entry failure"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"       "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                     "no kernel panic"

il_summary
