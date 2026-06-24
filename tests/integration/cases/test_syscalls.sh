#!/usr/bin/env bash
# test_syscalls.sh — syscall surface: write/read/open/close/listdir/getpid.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Syscall surface"

LOG="$IL_LOGDIR/syscalls.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# Boot, then use the shell, which itself relies on read/write/open/listdir.
il_send_delay 6
il_send "ls /"            # listdir + write to stdout
il_send_delay 1
il_send "cat /hello"      # open + read + write
il_send_delay 1
il_send "ps"              # exercises getpid via shell internals
il_send_delay 1
il_send "exit"            # exit(0)

il_run_qemu "$LOG" 20

il_assert_grep "$LOG" "/init"                       "listdir syscall"
il_assert_grep "$LOG" "\x7fELF|ELF"                 "open+read returned ELF magic from /hello"
il_assert_grep "$LOG" "auralite#"                   "shell read serial input ok"

# No SIGSEGV-equivalent (kernel kills user thread on user-mode #PF / #GP).
il_assert_no_grep "$LOG" "user thread killed"       "no user thread killed unexpectedly"

il_summary
