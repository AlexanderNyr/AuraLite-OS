#!/usr/bin/env bash
# test_lx_busybox.sh -- LX_COMPAT_PLAN.md L2: the second rung of the
# Linux application ladder.
#
# An UNMODIFIED upstream busybox (static musl 1.35.0, fetched by the
# Makefile behind a pinned SHA-256) runs the three file utilities the
# phase is named for, one process each:
#   echo  -- the write path through the translated number;
#   cat   -- open/read/write on a staged file: busybox's sendfile fast
#            path receives our honest -ENOSYS and falls back to its own
#            read/write loop (the fallback is verified in
#            busybox-1.35.0 libbb/copyfd.c, not assumed) -- into an
#            mmap_anon copy buffer, which is what found the lazy-VMA
#            validate bug (read into a fresh anonymous mmap page);
#   ls    -- openat(O_DIRECTORY), stat/lstat marshalled into Linux's
#            144-byte struct stat, and getdents64 records packed from
#            the VFS readdir snapshot, on two directories.
#
# Assertion mechanics, learned on the first run:
#   - the console answers TIOCGWINSZ, so busybox believes stdout is a
#     terminal and COLORISES ls output -- every ls runs --color=never;
#   - patterns anchor at ^ only (grep -E here does not parse \r, and
#     the applet lines end with a CR from the tty), never at $;
#   - "^hello" would false-positive against the boot selftest's own
#     /bin/hello line, so /linux/tests is not asserted by name -- the
#     per-run "exited (code=0)" receipts carry the clean-exit proof
#     instead (echo/cat/ls x2 = 4).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "LX L2: unmodified static busybox (ls/cat/echo)"

LOG="$IL_LOGDIR/lx_busybox.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "lxrun /linux/bin/busybox echo LXBEE-7f3a"
il_send_delay 6
il_send "lxrun /linux/bin/busybox cat /linux/etc/motd"
il_send_delay 6
il_send "lxrun /linux/bin/busybox ls --color=never -1 /"
il_send_delay 6
il_send "lxrun /linux/bin/busybox ls --color=never -1 /linux/etc"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 150

il_assert_grep    "$LOG" "^LXBEE-7f3a" \
                       "echo: the applet's own write(1) reached the console"
il_assert_grep    "$LOG" "^LX-BUSYBOX-CAT-OK" \
                       "cat: the staged file's first line came back"
il_assert_grep    "$LOG" "^linux" \
                       "ls /: the root listing names /linux (getdents64)"
il_assert_grep    "$LOG" "^motd" \
                       "ls /linux/etc: motd is listed (getdents64)"
il_assert_grep    "$LOG" "^zz-ls-probe" \
                       "ls /linux/etc: the second entry too (record packing)"
il_assert_count   "$LOG" "/bin/lxrun.*exited .code=0." 4 \
                       "all four busybox runs exited 0 (echo+cat+ls x2)"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"   "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                 "no kernel panic"
il_assert_no_grep "$LOG" "ELF load failed"       "the 1.1 MB ELF actually mapped"

il_summary
