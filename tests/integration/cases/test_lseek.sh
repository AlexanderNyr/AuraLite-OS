#!/usr/bin/env bash
# test_lseek.sh — P3 gate: lseek, pread/pwrite, readv/writev, shared OFDs.
#
# Runs /selftest, whose P3 block exercises:
#   - lseek SEEK_SET/CUR/END,
#   - pread/pwrite not moving the offset,
#   - dup() sharing the OFD seek offset,
#   - lseek on a pipe -> ESPIPE,
#   - readv/writev round-trip.
#
# NOTE: fork()-based offset sharing uses the SAME OFD-refcount path as dup()
# (vfs_fork_inherit bumps the shared OFD refcount), so dup sharing here
# validates the mechanism.  A dedicated fork FD-sharing case is deferred until
# fork() is robust against the per-thread SYSCALL-save-area race (see TODO.md).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "lseek / pread / pwrite / readv / writev / shared OFD (P3)"

LOG="$IL_LOGDIR/lseek.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "SELFTEST PASS: lseek SEEK_SET 0"        "lseek SEEK_SET: OK"
il_assert_grep "$LOG" "SELFTEST PASS: read back after lseek"   "read after lseek"
il_assert_grep "$LOG" "SELFTEST PASS: lseek SEEK_END"          "lseek SEEK_END"
il_assert_grep "$LOG" "SELFTEST PASS: lseek SEEK_CUR -2"       "lseek SEEK_CUR"
il_assert_grep "$LOG" "SELFTEST PASS: pread at offset 0"       "pread"
il_assert_grep "$LOG" "SELFTEST PASS: pread did not move pos"  "pread keeps pos"
il_assert_grep "$LOG" "SELFTEST PASS: pwrite at offset 1"      "pwrite"
il_assert_grep "$LOG" "SELFTEST PASS: pwrite did not move pos" "pwrite keeps pos"
il_assert_grep "$LOG" "SELFTEST PASS: pwrite landed at offset 1" "pwrite landed"
il_assert_grep "$LOG" "SELFTEST PASS: dup shares offset"       "fork shared pos: OK"
il_assert_grep "$LOG" "SELFTEST PASS: lseek on pipe -> ESPIPE" "pipe lseek: ESPIPE"
il_assert_grep "$LOG" "SELFTEST PASS: writev 2+3 bytes"        "writev"
il_assert_grep "$LOG" "SELFTEST PASS: readv 2+3 bytes"         "readv"

# No regressions / faults.
il_assert_grep    "$LOG" "SELFTEST ALL PASS"      "all selftests passed"
il_assert_no_grep "$LOG" "SELFTEST FAIL"          "no selftest failures"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"    "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"                  "no panic"

il_summary
