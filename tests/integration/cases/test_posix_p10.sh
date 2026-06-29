#!/usr/bin/env bash
# test_posix_p10.sh — P10 (POSIX.1-2017) libc surface, end-to-end in QEMU.
#
# Runs the standalone /p10test binary (env vars, strtod/strtol, extended math,
# fnmatch, POSIX regex, POSIX semaphores, inet_pton/ntop, getcwd, dirent) and
# asserts each stable "P10TEST PASS: …" marker. Unlike /selftest, /p10test is
# short and self-contained, so the run reaches its final verdict reliably.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "P10 POSIX libc surface (/p10test)"

LOG="$IL_LOGDIR/posix_p10.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run /p10test"
il_send_delay 14

il_run_qemu "$LOG" 40

# ---- environment variables ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: setenv/getenv round-trip"   "setenv/getenv"
il_assert_grep_fixed "$LOG" "P10TEST PASS: setenv overwrite=1 replaces" "setenv overwrite"
il_assert_grep_fixed "$LOG" "P10TEST PASS: setenv overwrite=0 keeps"   "setenv no-overwrite"
il_assert_grep_fixed "$LOG" "P10TEST PASS: unsetenv removes"           "unsetenv"

# ---- strtod / strtol / strtoul ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: strtod parses 3.5"          "strtod"
il_assert_grep_fixed "$LOG" "P10TEST PASS: strtol parses -42"          "strtol"
il_assert_grep_fixed "$LOG" "P10TEST PASS: strtoul hex ff==255"        "strtoul"

# ---- extended math (these regressed previously to infinite self-loops) ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: asin(1)==pi/2"              "asin"
il_assert_grep_fixed "$LOG" "P10TEST PASS: atan2(1,1)==pi/4"           "atan2"
il_assert_grep_fixed "$LOG" "P10TEST PASS: fmod(7,3)==1"               "fmod"

# ---- fnmatch ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: fnmatch *.c matches foo.c"  "fnmatch match"
il_assert_grep_fixed "$LOG" "P10TEST PASS: fnmatch *.c rejects foo.h"  "fnmatch reject"
il_assert_grep_fixed "$LOG" "P10TEST PASS: fnmatch PATHNAME star stops at slash" "fnmatch PATHNAME"

# ---- POSIX regex ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: regexec finds 'ell' in 'hello'" "regexec match"
il_assert_grep_fixed "$LOG" "P10TEST PASS: regexec no match returns nonzero" "regexec no-match"

# ---- POSIX semaphores (futex-backed) ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: sem_wait on count=1 succeeds" "sem_wait"
il_assert_grep_fixed "$LOG" "P10TEST PASS: sem_trywait on count=0 fails"  "sem_trywait"
il_assert_grep_fixed "$LOG" "P10TEST PASS: sem_post returns 0"            "sem_post"

# ---- inet_pton / inet_ntop ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: inet_pton 127.0.0.1"           "inet_pton"
il_assert_grep_fixed "$LOG" "P10TEST PASS: inet_ntop 192.168.0.5"         "inet_ntop"
il_assert_grep_fixed "$LOG" "P10TEST PASS: inet_pton rejects 256.0.0.1"   "inet_pton reject"

# ---- getcwd + dirent ----
il_assert_grep_fixed "$LOG" "P10TEST PASS: getcwd returns a path"         "getcwd"
il_assert_grep_fixed "$LOG" "P10TEST PASS: opendir/readdir / yields entries" "opendir/readdir"

# ---- overall verdict + no crash ----
il_assert_grep "$LOG" "P10TEST ALL PASS"     "p10test reached ALL PASS"
il_assert_no_grep "$LOG" "P10TEST FAIL"      "no p10test assertion failed"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"             "no panic"

il_summary
