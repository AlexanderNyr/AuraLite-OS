#!/usr/bin/env bash
#
# test_mmap_file.sh — TESTAUDIT_PLAN.md A6.
#
# File-backed MAP_SHARED used to return -ENOSYS: MATURITY_PLAN.md M4 said it
# was "pending page cache writeback from M9".  The page cache and the fault
# path were both already there; what was missing was anything that set the
# dirty bit, so writeback had nothing to write.
#
# The gate runs /tests/mmapfile, which stores through a shared file mapping,
# msync()s it, unmaps, reopens the file and reads the bytes back -- with a
# MAP_PRIVATE mapping as the control that must NOT write through.
#
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/lib.sh
. "$SCRIPT_DIR/../lib/lib.sh"

il_init "mmap_file"
il_section "file-backed MAP_SHARED: store, msync, munmap, reopen"

LOG="$IL_LOGDIR/mmap_file.log"

# The prompt appears late in the boot log; sending earlier looks exactly
# like a broken feature.
il_send_delay 9
il_send "run mmapfile"
il_send_delay 12
il_send "exit"

il_run_qemu "$LOG" 75 \
    -drive file="$IL_BUILD/auralite.iso",format=raw,if=ide,snapshot=on \
    -boot order=c -m 512M -smp 2 -display none -no-reboot -cpu qemu64

il_assert_grep_fixed "$LOG" "mmapfile: file-backed MAP_SHARED round trip" \
    "mmapfile ran"

# The mapping must be granted at all -- this is the -ENOSYS that A6 removed.
il_assert_grep_fixed "$LOG" "ok   mmap(MAP_SHARED, file) is not -ENOSYS" \
    "file-backed MAP_SHARED is no longer ENOSYS"

# Writeback actually happened: the dirty bit was set, and flushed.
il_assert_grep_fixed "$LOG" "ok   msync() reports success" \
    "msync() succeeded"
il_assert_grep_fixed "$LOG" \
    "ok   after msync(), read() on a second fd sees the store" \
    "msync() made the store visible to read()"
il_assert_grep_fixed "$LOG" "ok   bytes survive munmap + reopen" \
    "the bytes reached the filesystem"

# The control: if this passes while MAP_PRIVATE also wrote through, the
# implementation made everything shared.
il_assert_grep_fixed "$LOG" "ok   MAP_PRIVATE did NOT write through (control)" \
    "MAP_PRIVATE stayed copy-on-write"

# The whole battery, so a silently shrinking denominator cannot pass.
il_assert_grep_fixed "$LOG" "ok   fsync() returns 0, not -ENOSYS" \
    "M9: fsync() is implemented"
il_assert_grep_fixed "$LOG" "ok   fsync() wrote the mapping back" \
    "M9: fsync() actually flushes the page cache"
il_assert_grep_fixed "$LOG" "== 8/8 passed ==" "mmapfile 8/8"

# The kernel must survive it.
il_assert_no_grep "$LOG" "PANIC|page fault in kernel" "no kernel fault"

il_summary
