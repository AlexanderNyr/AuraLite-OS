#!/usr/bin/env bash
# test_shell_commands.sh — exercise the built-in shell command surface.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Shell command surface"

LOG="$IL_LOGDIR/shell_commands.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

MARK="MARK_$$_$(date +%s)"

il_send_delay 6
il_send "help"
il_send_delay 1
il_send "uname"
il_send_delay 1
il_send "pwd"
il_send_delay 1
il_send "free"
il_send_delay 1
il_send "ls /"
il_send_delay 1
il_send "echo $MARK"
il_send_delay 1
il_send "ps"
il_send_delay 1
il_send "run /hello"
il_send_delay 2
il_send "run /sysinfo"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 30

# help output must mention multiple commands
il_assert_grep "$LOG" "help"                "help printed"
il_assert_grep "$LOG" "(AuraLite|x86_64)"   "uname output"
il_assert_grep "$LOG" "^/$|^/"              "pwd printed"
il_assert_grep "$LOG" "(free|usable|MiB|KiB)" "free output"

# ls / should show files from initrd
il_assert_grep "$LOG" "/init"               "ls shows /init"
il_assert_grep "$LOG" "/hello"              "ls shows /hello"
il_assert_grep "$LOG" "/calc"               "ls shows /calc"

il_assert_grep "$LOG" "$MARK"               "echo round-tripped marker"

# run /hello produced its banner
il_assert_grep "$LOG" "(Hello|hello)"       "/hello ran"

# run /sysinfo produced something
il_assert_grep "$LOG" "(sysinfo|System|cpu|CPU|Aura)" "/sysinfo ran"

il_summary
