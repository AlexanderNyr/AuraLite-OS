#!/linux/bin/busybox ash
# lx/tests/ash_script.sh — LX_COMPAT_PLAN.md L3 ladder script.
#
# Run by the gate as `lxrun /linux/bin/busybox ash /linux/tests/ash_script.sh`
# (and, for the binfmt_script path, via `lxrun /linux/tests/ash_script.sh`,
# which re-execs /linux/bin/busybox through the #! line above — the persona
# survives the re-exec because the interpreter path stays under /linux).
#
# Each line prints a greppable receipt; the assertions in test_lx_shell.sh
# anchor at ^ so the serial echo of the typed command cannot satisfy them.
#
#   echo | cat          pipeline: fork + pipe + wait4 + SIGCHLD
#   ls / >/dev/null     getdents64 + stat marshal + redirection
#   sleep 0 & ; wait    background job + wait4 (SIGCHLD -> waitpid(WNOHANG))
#   trap INT; kill -$$  sigaction marshal + kill + signal delivery to an lx
#                       handler + rt_sigreturn + exit-status propagation (7)
#
# cat/ls/sleep resolve through PATH to /linux/bin/NAME (hard links to the
# busybox binary, whose argv[0] basename dispatches the applet): this
# busybox build has SH_STANDALONE off, so the shell must execve them.

PATH=/linux/bin
export PATH

echo LX3-ECHO-OK | cat
ls / >/dev/null && echo LX3-LS-OK
sleep 0 &
wait
echo LX3-BGJOB-OK
trap 'echo LX3-TRAP-OK; exit 7' INT
kill -INT $$
echo LX3-NOTREACHED
exit 0
