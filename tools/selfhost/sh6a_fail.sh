# sh6a_fail.sh -- SELFHOST SH6a failure-path gate script.
#
# Run in-guest as `sh /tests/sh6a_fail.sh`.  Line 8 names a command that does
# not exist, so the script MUST stop there and report that line number; line 9
# must never execute.  A build log that says "failed" without saying where is
# the failure mode this proves cannot happen.
echo [selfhost] sh6a: fail probe start
this_command_does_not_exist_xyz
echo [selfhost] sh6a: UNREACHABLE-AFTER-FAILURE
