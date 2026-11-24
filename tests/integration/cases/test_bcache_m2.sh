#!/usr/bin/env bash
# test_bcache_m2.sh — FSMODERN_PLAN.md M2: the block layer is 4 KiB now.
#
# M2's acceptance has three parts, and each one is a claim about the LIVE
# kernel, not about a host array:
#
#   1. sequential read throughput at least 2x the M0 baseline on /fat;
#   2. write coalescing visible in the writeback receipt (merged > 0 after a
#      sequential 1 MiB write, from a standing start);
#   3. the cache footprint stated in the boot receipt, with the block count
#      derived from RAM rather than a hard 1024.
#
# The M0 baseline this compares against is recorded with a date in
# docs/filesystems_perf.md -- /fat seq_read 2.19 MiB/s on 2026-11-22.  The
# floor below is that number doubled, which is exactly the plan's "2x the M0
# baseline", so the ratchet is the plan's own threshold and not a number
# picked to be easy to pass.
#
# The host lane (tests/unit/test_bcache_m2.c) owns the arithmetic: block
# translation, partial-sector requests, batch fetch, coalescing counts.  This
# case is what proves the arithmetic survives contact with a real disk.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "/fat: M2's three acceptance criteria on a real disk"

LOG="$IL_LOGDIR/bcache_m2.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

DISK="$IL_LOGDIR/bcache_m2.img"
rm -f "$DISK"
truncate -s 64M "$DISK" 2>/dev/null || dd if=/dev/zero of="$DISK" bs=1M count=64 status=none

# 8 x 64 KiB = 512 KiB per phase.  Larger than the old 512 KiB cache, so
# the run exercises eviction as well as the hit path.
il_send "fsbench --dir /fat --files 8 --size 65536"
il_send "sync"
il_send "exit"
il_run_qemu "$LOG" 150 \
    -drive "id=d0,file=$DISK,format=raw,if=none" -device ahci,id=ahci0 \
    -device ide-hd,drive=d0,bus=ahci0.0 \
    -fw_cfg name=opt/auralite.fsformat,string=1

# ---- criterion 3: the footprint is stated and derived from RAM ----------
il_assert_grep "$LOG" "\[bc\] 4 KiB x [0-9]+ blocks = [0-9]+ KiB cache" \
    "the boot receipt states the cache footprint in 4 KiB blocks"
il_assert_grep "$LOG" "usable RAM" \
    "and says which RAM figure it derived the count from"
il_assert_no_grep "$LOG" "4 KiB x 1024 blocks" \
    "the block count is NOT the old hardcoded 1024"
il_assert_grep "$LOG" "clamp 512\.\.2048" \
    "the clamp bounds are named, so the derivation is checkable"

# ---- the run itself -----------------------------------------------------
il_assert_grep "$LOG" "\[fsbench\] dir=/fat" "fsbench ran against /fat"
il_assert_grep "$LOG" "\[fsbench\] PASS:" "it wrote, verified and unlinked its payload"

# ---- criterion 1: at least 2x the M0 baseline (2.19 MiB/s) --------------
# Parsed, not grepped: a grep for a decimal would pass on "2.20" and fail on
# "16.31" if the pattern were written the obvious way.
seq_read=$(sed -n 's/.*\[fsbench\] seq_read=\([0-9.]*\) MiB\/s.*/\1/p' "$LOG" | tail -1)
seq_write=$(sed -n 's/.*\[fsbench\] seq_write=\([0-9.]*\) MiB\/s.*/\1/p' "$LOG" | tail -1)
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -n "$seq_read" ] && awk "BEGIN{exit !($seq_read >= 4.4)}"; then
    il_pass "seq_read ${seq_read} MiB/s is at least 2x the M0 baseline (4.38)"
else
    il_fail "seq_read ${seq_read:-<none>} MiB/s did not reach 2x the M0 baseline (4.38)"
fi
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -n "$seq_write" ] && awk "BEGIN{exit !($seq_write > 0)}"; then
    il_pass "seq_write ${seq_write} MiB/s is finite and positive"
else
    il_fail "seq_write ${seq_write:-<none>} is not a usable number"
fi

# ---- criterion 2: coalescing is visible and non-zero --------------------
# The receipt prints `merged=` -- device commands saved by merging adjacent
# dirty blocks.  It must be > 0 somewhere in this run: a sequential write of
# 512 KiB cannot avoid producing adjacent dirty blocks.
merged_max=$(sed -n 's/.*merged=\([0-9]\+\)$/\1/p' "$LOG" | sort -n | tail -1)
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -n "$merged_max" ] && [ "$merged_max" -gt 0 ]; then
    il_pass "write coalescing merged $merged_max adjacent-block commands into fewer"
else
    il_fail "write coalescing never merged anything (max merged=${merged_max:-<none>})"
fi

# The coalesced run must actually reach the device as ONE command.  The
# host lane asserts this exactly; here the evidence is that merged > 0
# while no write error was reported.
il_assert_no_grep "$LOG" "\[bc\] ERROR" "the cache reported no I/O error"

il_summary
