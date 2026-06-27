#!/usr/bin/env bash
# test_termios.sh — P5 gate: TTY / termios / ioctl / isatty / FILE*.
#
# Runs /selftest, whose P5 block opens /dev/tty0 and exercises isatty,
# tcgetattr/tcsetattr/cfmakeraw, TIOCGWINSZ, and FILE* (fopen/fprintf/fgets).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "TTY / termios / FILE* (P5)"

LOG="$IL_LOGDIR/termios.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "SELFTEST PASS: open /dev/tty0"                  "open /dev/tty0"
il_assert_grep "$LOG" "SELFTEST PASS: isatty(/dev/tty0)"              "isatty TTY"
il_assert_grep "$LOG" "SELFTEST PASS: tcgetattr OK"                  "tcgetattr: OK"
il_assert_grep "$LOG" "SELFTEST PASS: cfmakeraw clears ICANON/ECHO/ISIG" "cfmakeraw: OK"
il_assert_grep "$LOG" "SELFTEST PASS: tcsetattr raw OK"              "tcsetattr raw"
il_assert_grep "$LOG" "SELFTEST PASS: raw mode round-trips"          "raw round-trip"
il_assert_grep "$LOG" "SELFTEST PASS: TIOCGWINSZ"                    "TIOCGWINSZ rows/cols"
il_assert_grep "$LOG" "SELFTEST PASS: isatty(regular file) == 0 + ENOTTY" "isatty non-tty"
il_assert_grep "$LOG" "SELFTEST PASS: fopen w"                       "fopen write"
il_assert_grep "$LOG" "SELFTEST PASS: fgets line 1"                  "fgets line 1"
il_assert_grep "$LOG" "SELFTEST PASS: fgets line 2"                  "fgets line 2"
il_assert_grep "$LOG" "SELFTEST PASS: fgets EOF"                     "fgets EOF"

# No regressions / faults.
il_assert_grep    "$LOG" "SELFTEST ALL PASS"      "all selftests passed"
il_assert_no_grep "$LOG" "SELFTEST FAIL"          "no selftest failures"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"    "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"                  "no panic"

il_summary
