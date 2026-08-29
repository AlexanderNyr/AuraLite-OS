# sh6a_probe.sh -- SELFHOST SH6a happy-path gate script.
#
# Staged into the initrd at /tests/sh6a_probe.sh and run in-guest as
#     sh /tests/sh6a_probe.sh kernel
# by tests/integration/cases/test_selfhost_script.sh.
#
# What it has to prove, line by line:
#   - whole-line comments and blank lines are skipped;
#   - $0 is the script name, $1 the first argument, $# the argument count;
#   - $? carries the previous line's exit status;
#   - $$ yields a literal dollar and an unknown name ($PATH) is passed
#     through untouched, so SH6b can define named variables later;
#   - a nested `sh` call works and returns to this script.
#
# The final line is the receipt the host greps for.  Its count is the number
# of command lines in this file, comments and blank lines excluded, and
# includes the receipt line itself: seven.

echo [selfhost] sh6a: script=$0 target=$1 args=$#
pwd
echo [selfhost] sh6a: pwd-status=$?
echo [selfhost] sh6a: dollar=$$ env=$PATH
sh /tests/sh6a_nested.sh $1
echo [selfhost] sh6a: nested-status=$?
echo [selfhost] script PASS: 7 lines ran in-guest
