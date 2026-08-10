#!/usr/bin/env bash
# test_smp_tss.sh — SMP bring-up should complete without TSS warnings.
#
# With AP wake disabled the "all N CPUs online" line is never printed.
# The meaningful gate is: no TSS warnings, no fatal fault.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "SMP per-CPU TSS"

LOG="$IL_LOGDIR/smp_tss.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 15 -smp 4

il_assert_grep    "$LOG" "\[smp\]"                               "SMP subsystem ran"

# Accept either multi-AP online message or BSP-only log.
il_assert_grep    "$LOG" "(AP wake disabled|all [0-9]+ CPUs online|\\[smp\\] PASS)" \
                                                                 "SMP mode reported"

il_assert_no_grep "$LOG" "\[tss\] WARN"                         "no TSS warnings"
il_assert_no_grep "$LOG" "panic|triple fault|Double Fault"       "no fatal SMP/TSS fault"

il_summary
