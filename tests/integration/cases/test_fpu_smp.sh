#!/usr/bin/env bash
# test_fpu_smp.sh — M1 (MATURITY_PLAN.md) FPU/SSE context-switch gate.
#
# Until M1 the context switch saved no FPU/SSE state, so a thread resumed on
# another CPU continued its floating-point computation with that CPU's stale
# xmm registers.  /fpustress spawns four FP-heavy pthreads that keep double
# accumulators live in xmm registers across hundreds of preemptions, each with
# a distinct base so cross-contamination yields a wrong value.  Under -smp 4
# this failed before M1 (another thread's partial sums leaked in) and must pass
# after it.  This is the dedicated regression for the class of corruption
# gltest exposed ("one run in three fails a DIFFERENT arbitrary check under
# -smp 2").

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "M1 FPU/SSE context switch (fpustress @ -smp 4)"

LOG="$IL_LOGDIR/fpu_smp.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run fpustress"
il_send_delay 15
il_send "exit"

il_run_qemu "$LOG" 60 -smp 4

# R5 (residue ledger RES-15): the runqueues have been per-CPU since
# SMP 3.2 -- this pins the once-printed receipt that a USER thread
# actually ran on an AP, which the stale status row denied.
il_assert_grep "$LOG" "R5 receipt: user thread pid=.* on AP cpu=" "R5: user thread observed on an AP"

# The four threads each finished and matched their single-threaded reference.
il_assert_grep    "$LOG" "FPSTRESS PASS"                "all four FP threads matched their reference"
il_assert_grep    "$LOG" "\\[fpustress\\] 4/4 threads correct" "no thread lost FPU state to a context switch"
il_assert_no_grep "$LOG" "MISMATCH"                     "no thread produced a contaminated result"
il_assert_no_grep "$LOG" "FPSTRESS FAIL"                "fpustress did not report failure"

# No kernel fault while switching FP state.
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"          "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                        "no kernel panic"
il_assert_no_grep "$LOG" "triple fault"                 "no triple fault"

il_summary
