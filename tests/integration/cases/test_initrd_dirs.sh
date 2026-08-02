#!/usr/bin/env bash
# test_initrd_dirs.sh — subdirectories in the initrd (FSLAYOUT_PLAN phase F0).
#
# The host unit test (tests/unit/test_initrd_dirs.c) covers the parser and the
# derived directory view exhaustively.  This case covers what the host test
# cannot: that the packaging script actually ships a subdirectory, that the
# kernel mounts an image containing one, and that the shell's ls/cat see it.
#
# It also asserts the *negative* that motivated the phase: "ls /" must not
# print a path with a slash in it.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "initrd subdirectories"

LOG="$IL_LOGDIR/initrd_dirs.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 6
il_send "ls /"
il_send_delay 1
il_send "ls /etc"
il_send_delay 1
il_send "cat /etc/motd"
il_send_delay 1
# Existing root-level programs must be untouched by the change.
il_send "ls /"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 25

il_assert_grep "$LOG" "\[initrd\] parsed .* file\(s\) in .* director"  "parser reports directories"
il_assert_grep "$LOG" "etc"                                          "ls / shows the etc directory"
il_assert_grep "$LOG" "motd"                                         "ls /etc shows motd"
il_assert_grep "$LOG" "filesystem layout"                            "cat /etc/motd reads through the subdirectory"

# The regression F0 exists to prevent: full paths leaking into a listing.
il_assert_no_grep "$LOG" "etc/motd  ("                               "ls does not print etc/motd as a root file"

# F0 added the capability; F3 used it and F5 removed the root aliases, so the
# root now holds directories.  The assertion that programs are still reachable
# lives in test_runtime_layout.sh, where it belongs.
il_assert_grep "$LOG" "apps/"                                        "ls / shows the apps directory"
il_assert_grep "$LOG" "tests/"                                       "ls / shows the tests directory"

il_assert_no_grep "$LOG" "PANIC"                                     "no kernel panic"

il_summary
