#!/usr/bin/env bash
# test_lx_dynamic.sh -- LX_COMPAT_PLAN.md L4: the fourth rung of the Linux
# application ladder — an UNMODIFIED dynamic glibc binary runs, and a
# dynamic glibc shell answers `sh -c 'echo ok'`.
#
# Two receipts, one per dynamic image:
#   1. /linux/tests/dyn/hello  a PIE, glibc-linked hello (Debian gcc
#      default).  Its whole startup is the phase: PT_INTERP read, ld.so
#      mapped at AT_BASE, libc mapped with MAP_FIXED mmap + mprotect
#      RELRO, futex on the loader lock, TLS via arch_prctl(ARCH_SET_FS),
#      set_robust_list, rseq answered -ENOSYS (glibc's fallback).
#   2. /linux/bin/sh -c 'echo ok'  dash — itself a PIE glibc binary — runs
#      the echo builtin and prints "ok" (the literal plan receipt).
#   3. /linux/tests/dyn/sh_cmd.sh  the binfmt_script re-exec: the kernel
#      re-execs /linux/bin/sh through the #! line, a second dynamic exec
#      from inside a dynamic process (the L3 ash pattern, now dynamic).
#
# Assertion mechanics (inherited from test_lx_shell.sh):
#   - patterns anchor at ^ only (the serial console CR-terminates lines and
#     the host grep does not parse \r);
#   - exit-status propagation is asserted through the kernel's
#     "[thread] '/bin/lxrun' (tid N) exited (code=0)" receipt — every
#     lxrun invocation here exits 0, so the count is the receipt.
#
# The placeholders: if the build had no host gcc (no dynamic hello) or no
# host dash/ld-linux/libc, the payloads are zero-byte and this case skips
# exactly like test_lx_busybox — a vacuous gate is worse than a skip.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "LX L4: dynamic glibc hello + sh -c 'echo ok'"

LOG="$IL_LOGDIR/lx_dynamic.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "lxrun /linux/tests/dyn/hello"
il_send_delay 6
il_send "lxrun /linux/bin/sh -c 'echo ok'"
il_send_delay 6
il_send "lxrun /linux/tests/dyn/sh_cmd.sh"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 150

il_assert_grep    "$LOG" "^LX4-HELLO-OK" \
                       "dynamic hello printed its receipt (ld.so mapped it + libc)"
il_assert_grep    "$LOG" "^ok" \
                       "sh -c 'echo ok' printed the plan's receipt (dash echo builtin)"
il_assert_grep    "$LOG" "^LX4-SH-OK" \
                       "binfmt_script re-exec: dash ran the staged script"
il_assert_count   "$LOG" "/bin/lxrun.*exited .code=0." 3 \
                       "all three dynamic invocations exited 0"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"

il_summary
