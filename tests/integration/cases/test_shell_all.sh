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
# NOT `cat /bin/hello`: that dumps a 96 KB ELF into the serial log, which
# makes the log a binary file and every later grep unreliable.  /etc/motd is
# a text file that exists on every boot.
il_send "cat /etc/motd"
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
il_send "run hello"
il_send_delay 2
il_send "run sysinfo"
il_send_delay 2
# calc leaves on "quit" or "q" -- NOT on "exit".  This said "exit", so calc
# stayed at its prompt and swallowed every command that followed; three
# assertions below failed for that reason rather than for anything they test.
il_send "run calc"
il_send "2+3*4"
il_send_delay 1
il_send "quit"
il_send_delay 1

# `clock` exits by itself, so it needs NO "exit" of its own.  There used to be
# one here, and it went to the SHELL -- closing it, so every command below ran
# into a dead terminal and three assertions failed for a reason that had
# nothing to do with what they were testing.  Only interactive programs
# (calc, editor) get an exit.
il_send "run clock"
il_send_delay 2

# Directory operations.
#
# M9: tmpfs DOES implement mkdir -- tmpfs_ops gained .mkdir in Q12, and all
# three tmpfs op tables (/tmp, /opt, /dev/shm) carry it.
#
# This comment used to say the opposite, and the assertion below was
# inverted to match: it required `mkdir: failed /tmp/testdir` and called
# that correct.  Verified against the running system instead of the
# comment -- the guest prints "mkdir: created /tmp/testdir" and `ls /tmp`
# lists `testdir/`.  The test was pinning a limitation that had been fixed,
# which is worse than no test: it would have gone red if someone fixed
# nothing and green only while the feature stayed broken.
il_send "mkdir /tmp/testdir"
il_send_delay 1
il_send "ls /tmp"
il_send_delay 1
il_send "rmdir /tmp/testdir"
il_send_delay 1

# stat
il_send "stat /bin/hello"
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
il_assert_grep "$LOG" "bin/"                          "ls / shows the bin directory"
il_assert_grep "$LOG" "apps/"                         "ls / shows the apps directory"
il_assert_grep "$LOG" "filesystem layout"             "cat /etc/motd read a text file"
il_assert_grep "$LOG" "$MARK"                         "echo round-trip"
il_assert_grep "$LOG" "hello_shell"                   "tmp file content"
il_assert_grep "$LOG" "(free|usable|MiB)"             "free output"
il_assert_grep "$LOG" "(hello|Hello)"                 "/hello ran"
il_assert_grep "$LOG" "(sysinfo|System|cpu|CPU)"       "/sysinfo ran"
il_assert_grep "$LOG" "14"                            "calc: 2+3*4=14"
il_assert_grep_fixed "$LOG" "mkdir: created /tmp/testdir" \
    "mkdir works on tmpfs (M9: tmpfs_ops has .mkdir)"
# And the directory must actually appear -- "created" alone would pass even
# if nothing were inserted into the volume.
il_assert_grep "$LOG" "testdir/" \
    "the new tmpfs directory is listed by ls"
il_assert_grep "$LOG" "(stat|Type|Size|regular)"      "stat output"
il_assert_grep "$LOG" "touch: /tmp/newfile"           "touch succeeded"
il_assert_grep "$LOG" "newfile"                       "ls shows newfile"

il_summary
