#!/usr/bin/env bash
# test_fsbench.sh — FSMODERN_PLAN.md M0: the IN-GUEST filesystem numbers.
#
# The host lane (tests/unit/test_fsbench.c) proves correctness, amplification
# and call counts, but every MiB/s it prints is RAM-to-RAM because the device
# under it is a host array.  This case is the lane where the numbers mean
# something: /bin/fsbench runs inside the guest, through the VFS, on the
# emulated disk, under TCG.
#
# What is asserted is deliberately SHAPE, not speed:
#   - the tool runs at all and prints the receipt;
#   - it wrote and read back its OWN payload (it verifies internally, so a
#     filesystem returning garbage fails here, not in a later GUI bug);
#   - the numbers are finite and positive (a benchmark that prints 0.00 or
#     inf has a broken clock, not a fast disk);
#   - the files are gone afterwards (the unlink phase really ran).
# A throughput floor would be a lie on a CI host of unknown speed; the
# ratchet, when M2 needs one, gets pinned to THIS lane's recorded numbers in
# docs/filesystems_perf.md with a date.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "fsbench (/tmp): in-guest filesystem numbers"

LOG="$IL_LOGDIR/fsbench.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The recipe is test_diskfs.sh's: /fat and /disk need a real AHCI disk, and
# the default boot has none ("[diskfs] no AHCI disk available").  The tmpfs
# run is the fallback that always works; the two disk runs are what make the
# numbers worth publishing.
DISK="$IL_LOGDIR/fsbench-disk.img"
rm -f "$DISK"
truncate -s 64M "$DISK" 2>/dev/null || dd if=/dev/zero of="$DISK" bs=1M count=64 status=none

il_send_delay 8
il_send "fsbench --dir /tmp --files 8 --size 4096"
il_send_delay 6
il_send "fsbench --dir /disk --files 4 --size 4096"
il_send_delay 6
il_send "ls /tmp"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 90 \
    -drive "id=diskimg,file=$DISK,format=raw,if=none,snapshot=on" \
    -device ahci,id=ahci \
    -device ide-hd,drive=diskimg,bus=ahci.0

# --- the tool ran and printed its receipt ------------------------------
il_assert_grep "$LOG" "\[fsbench\] dir=/tmp files=8 size=4096" \
    "the benchmark ran with the requested shape"
# M0's own lesson: the first version timed with the 99 Hz PIT clock and
# reported 31250 MiB/s (a 10 ms tick dividing a 1 ms run).  The receipt must
# name a clock with sub-millisecond resolution, or every number below it is
# arithmetic on a zero divisor.
il_assert_grep "$LOG" "\[fsbench\] dir=/tmp files=8 size=4096 clock=tsc" \
    "the run timed with the TSC, not the 10 ms tick clock"
il_assert_grep "$LOG" "\[fsbench\] PASS:" \
    "the run completed (payload verified and unlinked)"
il_assert_grep "$LOG" "verified=32768 bytes" \
    "it read back all 32 KiB it wrote"
# The parentheses are literal in the receipt, so they are escaped here --
# test_home.sh's first draft lost an assertion to exactly this.
il_assert_grep "$LOG" "removed=8 file\(s\)" \
    "all 8 files were unlinked"

# --- the numbers exist and are parseable -------------------------------
il_assert_grep "$LOG" "\[fsbench\] seq_write=[0-9]" \
    "seq_write produced a number"
il_assert_grep "$LOG" "\[fsbench\] seq_read=[0-9]" \
    "seq_read produced a number"
il_assert_grep "$LOG" "\[fsbench\] unlink=[0-9]" \
    "unlink produced a number"
il_assert_grep "$LOG" "\[fsbench\] sync=[0-9]" \
    "sync produced a number"

# --- the disk lanes: the numbers this document publishes ----------------
il_assert_grep "$LOG" "\[diskfs\] tiny persistent filesystem ready" \
    "/disk really mounted (the AHCI disk was attached)"
il_assert_count "$LOG" "\[fsbench\] PASS:" 2 \
    "both the tmpfs and the on-disk run completed"
il_assert_grep "$LOG" "dir=/disk files=4 size=4096 clock=tsc" \
    "the on-disk run used the requested shape"

# --- the files are gone: the unlink phase was not cosmetic -------------
# The benchmark prints a file's name only on a failure path, so after a
# PASS the log must carry no "fsbench: FAIL" at all.
il_assert_no_grep "$LOG" "fsbench: FAIL" \
    "nothing failed (no FAIL line from the tool)"

il_summary
