# sh6a_exit.sh -- SELFHOST SH6a `exit` gate script.
#
# init IS PID 1, and the shell's `exit` command halts the machine.  Inside a
# script that would mean `exit 3` powering off the system instead of failing
# one build step.  The host asserts the prompt comes back afterwards, which is
# the only observable difference between the two behaviours.
echo [selfhost] sh6a: exit probe
exit 3
echo [selfhost] sh6a: UNREACHABLE-AFTER-EXIT
