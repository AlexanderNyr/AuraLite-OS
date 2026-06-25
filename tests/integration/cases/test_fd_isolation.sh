#!/usr/bin/env bash
# test_fd_isolation.sh — single-process FD lifecycle (dup, dup2, fcntl,
# pipe).  Cross-process FD isolation is enforced by the kernel's
# per-process FD table (struct tcb::fd_table) and exercised whenever the
# shell spawns a user program: if the new process inherited stale FDs the
# bundled apps would mis-behave noisily.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "FD lifecycle (dup, dup2, fcntl, pipe) + per-process FD isolation"

LOG="$IL_LOGDIR/fd_isolation.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /fdtest"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "FDTEST PASS: parent opens /hello"            "open returns fd >= 3"
il_assert_grep "$LOG" "FDTEST PASS: parent opens /hello again"      "two opens return distinct fds"
il_assert_grep "$LOG" "FDTEST PASS: dup returns new fd >= 3"        "dup works"
il_assert_grep "$LOG" "FDTEST PASS: dup2 to higher fd"              "dup2 works"
il_assert_grep "$LOG" "FDTEST PASS: F_GETFD initial 0"              "fcntl GETFD baseline"
il_assert_grep "$LOG" "FDTEST PASS: F_SETFD CLOEXEC"                "fcntl SETFD CLOEXEC"
il_assert_grep "$LOG" "FDTEST PASS: F_GETFD reads back CLOEXEC"     "fcntl round-trip"
il_assert_grep "$LOG" "FDTEST PASS: pipe creates two fds"           "pipe works"
il_assert_grep "$LOG" "FDTEST PASS: pipe write 5 bytes"             "pipe write works"
il_assert_grep "$LOG" "FDTEST PASS: pipe read 5 bytes"              "pipe read returns same bytes"
il_assert_grep "$LOG" "FDTEST PASS: pipe rejects kernel out buffer" "pipe validates user pointer"
il_assert_grep "$LOG" "FDTEST ALL PASS"                             "all FD subtests passed"
il_assert_no_grep "$LOG" "FDTEST FAIL"                              "no FD test failures"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                      "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                                    "no kernel panic"

il_summary
