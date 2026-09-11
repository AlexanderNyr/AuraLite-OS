#!/linux/bin/sh
# lx/tests/dyn/sh_cmd.sh — LX_COMPAT_PLAN.md L4 ladder script.
#
# The binfmt_script path for a DYNAMIC shell: execve'ing this script
# re-execs /linux/bin/sh (dash, dynamically linked against glibc)
# through the #! line — the same kernel re-exec the L3 ash_script.sh
# used, but the interpreter here is itself a PIE glibc binary, so this
# receipt proves the loader survived a second dynamic exec from inside
# a dynamic process.
#
# `echo` is a dash builtin: no child exec, so the receipt is the
# shell's own write(2).

echo LX4-SH-OK
exit 0
