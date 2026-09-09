#!/usr/bin/env bash
# test_lx_hello.sh -- LX_COMPAT_PLAN.md L1: the first rung of the Linux
# application ladder.
#
# An UNMODIFIED, statically linked Linux glibc binary (built by the host
# gcc, staged under /linux) runs on AuraLite through the lx personality:
# the /linux path prefix selects the Linux number map at execve, glibc's
# static startup survives (set_tid_address, brk, arch_prctl, and every
# probed call we honestly refuse with -ENOSYS), the write(2) lands on
# the console through the translated number, and exit_group terminates
# through the lx-only arm.  lxrun(1) is the front door; the direct
# absolute-path exec exercises the same rule without the wrapper.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "LX L1: unmodified static Linux binary (hello)"

LOG="$IL_LOGDIR/lx_hello.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "lxrun /linux/tests/hello"
il_send_delay 4
il_send "/linux/tests/hello"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 60

il_assert_grep    "$LOG" "lx] hello from an unmodified Linux binary" \
                       "the Linux binary's own write(2) reached the console"
il_assert_count   "$LOG" "lx] hello from an unmodified Linux binary" 2 \
                       "both launches: via lxrun and via the direct path"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"   "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                 "no kernel panic"
il_assert_no_grep "$LOG" "ELF load failed"       "the ELF actually mapped"

il_summary
