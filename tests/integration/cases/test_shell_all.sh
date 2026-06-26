#!/usr/bin/env bash
# test_shell_all.sh — comprehensive shell command surface test covering
# all built-in commands and multiple userspace programs.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Shell — all built-in commands"

LOG="$IL_LOGDIR/shell_all.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

MARK="SHELLALL_$$_$(date +%s)"

il_send_delay 5

# Core commands
il_send "help"
il_send_delay 1
il_send "uname"
il_send_delay 1
il_send "pwd"
il_send_delay 1

# File operations
il_send "ls /"
il_send_delay 1
il_send "cat /hello"
il_send_delay 1
il_send "echo $MARK"
il_send_delay 1
il_send "write /tmp/shell_test hello_shell"
il_send_delay 1
il_send "cat /tmp/shell_test"
il_send_delay 1

# Memory and processes
il_send "free"
il_send_delay 1
il_send "ps"
il_send_delay 1

# Userspace programs
il_send "run /hello"
il_send_delay 2
il_send "run /sysinfo"
il_send_delay 2
il_send "run /calc"
il_send "2+3*4"
il_send_delay 1
il_send "exit"
il_send_delay 1

il_send "run /clock"
il_send_delay 2
il_send "exit"
il_send_delay 1

# Directory operations
il_send "mkdir /tmp/testdir"
il_send_delay 1
il_send "ls /tmp"
il_send_delay 1
il_send "rmdir /tmp/testdir"
il_send_delay 1

# stat
il_send "stat /hello"
il_send_delay 1

# touch
il_send "touch /tmp/newfile"
il_send_delay 1
il_send "ls /tmp"
il_send_delay 1

il_send "exit"

il_run_qemu "$LOG" 50

il_assert_grep "$LOG" "(AuraLite|x86_64)"           "uname output"
il_assert_grep "$LOG" "^/$|/"                         "pwd output"
il_assert_grep "$LOG" "/init"                         "ls shows /init"
il_assert_grep "$LOG" "/hello"                        "ls shows /hello"
il_assert_grep "$LOG" "$MARK"                         "echo round-trip"
il_assert_grep "$LOG" "hello_shell"                   "tmp file content"
il_assert_grep "$LOG" "(free|usable|MiB)"             "free output"
il_assert_grep "$LOG" "(hello|Hello)"                 "/hello ran"
il_assert_grep "$LOG" "(sysinfo|System|cpu|CPU)"       "/sysinfo ran"
il_assert_grep "$LOG" "14"                            "calc: 2+3*4=14"
il_assert_grep "$LOG" "mkdir: created /tmp/testdir"   "mkdir succeeded"
il_assert_grep "$LOG" "testdir"                       "ls shows testdir"
il_assert_grep "$LOG" "rmdir: removed /tmp/testdir"   "rmdir succeeded"
il_assert_grep "$LOG" "(stat|Type|Size|regular)"      "stat output"
il_assert_grep "$LOG" "touch: /tmp/newfile"           "touch succeeded"
il_assert_grep "$LOG" "newfile"                       "ls shows newfile"

il_summary
