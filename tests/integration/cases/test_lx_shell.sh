#!/usr/bin/env bash
# test_lx_shell.sh -- LX_COMPAT_PLAN.md L3: the third rung of the Linux
# application ladder — an UNMODIFIED static busybox ash runs a script.
#
# The script (lx/tests/ash_script.sh, staged at /linux/tests/) exercises the
# process plumbing the phase is named for, one receipt per line:
#   echo | cat          fork + pipe + wait4 + SIGCHLD (the pipeline path)
#   ls / >/dev/null     getdents64 + stat marshal + redirection
#   sleep 0 & ; wait    background job: SIGCHLD -> waitpid(-1, WNOHANG)
#   trap INT; kill -$$  the whole signal round-trip on the SHELL ITSELF:
#                       rt_sigaction marshal (trap installs a handler),
#                       kill(2), signal delivery to an lx handler through
#                       the Linux rt_sigframe, rt_sigreturn parse, and exit-
#                       status propagation (the trap exits 7).
#
# Assertion mechanics (learned on the L2 run, plus two L3 facts):
#   - patterns anchor at ^ only (the serial console CR-terminates lines and
#     the host grep does not parse \r);
#   - the trap's `exit 7` makes the SCRIPT exit 7, so the "exited (code=7)"
#     receipt is BOTH the signal-round-trip proof AND the exit-status proof;
#   - `kill -INT` of a *background* child is NOT a usable receipt: a
#     non-interactive shell sets SIGINT to SIG_IGN in background children
#     (measured on the host: wait returns 0), which is why the trap test
#     targets the shell's own pid instead.
#
# If busybox is absent from the build (no curl/wget at build time) this case
# skips exactly like test_lx_busybox does — a vacuous gate is worse than a
# skip.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "LX L3: unmodified static busybox ash runs a script"

LOG="$IL_LOGDIR/lx_shell.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "lxrun /linux/bin/busybox ash /linux/tests/ash_script.sh"
il_send_delay 8
# The binfmt_script path: execve the script itself; the kernel re-execs
# /linux/bin/busybox through the #! line and the persona survives because
# the interpreter path stays under /linux.
il_send "lxrun /linux/tests/ash_script.sh"
il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 150

il_assert_grep    "$LOG" "^LX3-ECHO-OK" \
                       "pipeline: echo's line survived the pipe (fork+pipe+wait4)"
il_assert_grep    "$LOG" "^LX3-LS-OK" \
                       "ls / succeeded with output redirected (getdents64+stat)"
il_assert_grep    "$LOG" "^LX3-BGJOB-OK" \
                       "background job: wait4 reaped the child after SIGCHLD"
il_assert_grep    "$LOG" "^LX3-TRAP-OK" \
                       "signal round-trip: the INT trap ran (sigaction+kill+frame+sigreturn)"
il_assert_no_grep "$LOG" "^LX3-NOTREACHED" \
                       "the trap's exit short-circuited the script"
il_assert_count   "$LOG" "/bin/lxrun.*exited .code=7." 1 \
                       "the script exited 7 (trap exit-status propagation)"
il_assert_count   "$LOG" "^LX3-ECHO-OK" 2 \
                       "both invocations ran (direct ash + binfmt_script re-exec)"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"   "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                 "no kernel panic"

il_summary
