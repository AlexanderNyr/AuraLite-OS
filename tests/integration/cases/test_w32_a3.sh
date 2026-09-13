#!/usr/bin/env bash
# test_w32_a3.sh — W32APP_PLAN.md phase W32A-3 gate.
#
# The threading claim: two nasm fixtures, run directly like the W32A-2
# group, each asserting REAL behaviour in-guest.  w32a3_threads.exe (45
# KERNEL32 imports) walks birth/join/codes, CS/SRW/CV/Once/SList,
# events, mutexes (incl. abandonment), semaphores, waits, SleepEx+APC,
# the threadpool, a printed perf lane, and — last — preemptive
# TerminateThread of a pure spinner; it exits 66 only if every check
# passed.  w32a3_tls.exe (17 imports) walks Tls/Fls slots, the loader's
# TLS-template registration (index cell + attach/detach counts), and
# main/worker isolation; it exits 67 on a clean run.  Either exits 1
# on any failure.
#
# No SKIP guard: nasm is an unconditional build requirement (like the
# W32-4/5/6 gates), so absent fixtures fail at build time, and the
# workflow asserts their initrd presence separately.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-3 integration: threads + TLS guest fixtures"

LOG="$IL_LOGDIR/w32_a3.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run /tests/w32a3_threads.exe"
il_send_delay 8
il_send "run /tests/w32a3_tls.exe"
il_send_delay 4
il_send "exit"

# The threads fixture runs 2M CS pairs (one lane contended x4) under TCG.
il_run_qemu "$LOG" 150

# --- every threads-section marker ------------------------------------------------
for m in IDS JOIN SUSP TIMES FLIB EXIT33 CS SRW CV ONCE SLIST EV MUTEX SEM WAIT APC POOL PERF TERM; do
    il_assert_grep "$LOG" "$m-OK" \
        "the threads fixture passed its $m section"
done
il_assert_grep "$LOG" "PERF-CS1M-MS:" \
    "the uncontended perf lane printed"
il_assert_grep "$LOG" "PERF-CS4X-MS:" \
    "the contended perf lane printed"
il_assert_grep "$LOG" "W32A3-THREADS-OK" \
    "the threads fixture passed every section"

# --- every TLS-section marker ----------------------------------------------------
for m in TLS-IDX TLS-ISO TLS-FREE FLS TLS-CB; do
    il_assert_grep "$LOG" "$m-OK" \
        "the TLS fixture passed its $m section"
done
il_assert_grep "$LOG" "W32A3-TLS-OK" \
    "the TLS fixture passed every section"

# --- and each exited through ExitProcess with its own success code ----------------
il_assert_grep "$LOG" "'/tests/w32a3_threads\\.exe' \\(tid [0-9]+\\) exited \\(code=66\\)" \
    "w32a3_threads.exe exited 66"
il_assert_grep "$LOG" "'/tests/w32a3_tls\\.exe' \\(tid [0-9]+\\) exited \\(code=67\\)" \
    "w32a3_tls.exe exited 67"
il_assert_no_grep "$LOG" "exited \\(code=1\\)" \
    "no in-guest assertion failed"

# --- nothing broke ----------------------------------------------------------------
il_assert_no_grep "$LOG" "unresolved import" \
    "no import was left unbound"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no kernel fault"
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived and exited cleanly"

il_summary
