#!/usr/bin/env bash
# test_smp_procstress.sh — RESIDUE2 T1 SMP-sweep gate: the stress app hammers
# the three structures the sweep made safe (per-vnode O_APPEND lock, the
# parent/child process table, atomic sig_pending) on 4 CPUs.
#
# The gate: /tests/smpstress runs fork+append+kill storms and verifies the
# invariants (append records intact, waitpid codes precise, signals counted).
# A pre-T1 tree fails the O_APPEND half (lost updates) and races the rest.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE2 T1 SMP process/append/signal stress (-smp 4)"

LOG="$IL_LOGDIR/smp_procstress.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run smpstress"
il_send_delay 20
il_send "exit"

# 240 (was 90, briefly 180): the O_APPEND storm under -smp 4 contends on
# the per-vnode lock, whose wait is the wq yield-poll (wq_wait keeps the
# yield semantics the NIC/pipe/GUI waiters depend on — see wait_queue.c);
# a loaded CI runner therefore needs real headroom, and 90s caught the
# run mid-churn with no markers yet even though the invariants were
# progressing.  The gates themselves are unchanged.
il_run_qemu "$LOG" 240 -smp 4

il_assert_grep "$LOG" "SMPSTRESS: O_APPEND .* intact" \
    "O_APPEND appends intact under 4-cpu concurrency"
il_assert_grep "$LOG" "SMPSTRESS: 100 fork/wait cycles precise" \
    "fork/waitpid churn through the children table"
il_assert_grep "$LOG" "SMPSTRESS: 8 x 10 signal deliveries counted" \
    "sig_pending deliveries counted under storm"
il_assert_grep "$LOG" "SMPSTRESS PASS" "overall stress gate"
il_assert_no_grep "$LOG" "panic|triple fault|Double Fault" "no fatal fault under storm"

il_summary
