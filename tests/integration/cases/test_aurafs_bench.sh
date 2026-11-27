#!/usr/bin/env bash
# M6c: three measured /data runs; assert correctness, never a CI speed floor.
set -uo pipefail
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3
IL_SELFTEST=fast
IL_SMP=4
IL_CPU=qemu64
LOG="$IL_LOGDIR/aurafs_bench.log"
REPORT="$IL_BUILD/aurafs_m6_report.md"
# A failed run must not leave a previous measurement looking fresh.
rm -f "$REPORT"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT
il_section "AuraFS /data: three in-guest 6 x 4096-byte runs"
DISKS=(-device ahci,id=aurafs-ahci -fw_cfg name=opt/auralite.fsformat,string=1)
for i in 0 1 2; do
    img="$IL_LOGDIR/aurafs_bench_$i.img"
    rm -f "$img"
    truncate -s 64M "$img"
    DISKS+=(-drive "id=aurafs-d$i,file=$img,format=raw,if=none"
            -device "ide-hd,drive=aurafs-d$i,bus=aurafs-ahci.$i")
done
for repeat in 1 2 3; do
    il_send_prompt "run /bin/fsbench --dir /data --files 6 --size 4096"
    # Benchmark unlinks too: persist the resulting tree before next run.
    il_send_prompt "fsync /data"
done
il_run_qemu_prompt "$LOG" 180 "${DISKS[@]}"
il_assert_grep "$LOG" '\[data\] PASS: .*mounted at /data' '/data really mounted'
il_assert_grep "$LOG" '\[aurafs\] append log: .*reclaim=none durability=WEAK' 'honest AuraFS mount receipt'
il_assert_count "$LOG" '\[fsbench\] PASS:' 3 'all three payloads verified and unlinked'
il_assert_count "$LOG" '^fsync: /data' 3 'all three explicit fsync commands succeeded'
il_assert_no_grep "$LOG" 'fsync: failed|fsync: cannot open|fsbench: FAIL|\[DOUBLE FAULT\]|\[bsod\]|KERNEL EXCEPTION' 'no benchmark failure or kernel exception'
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if python3 - "$LOG" "$REPORT" "${MEASUREMENT_DATE:-$(date +%F)}" \
    "$(qemu-system-x86_64 --version | head -1)" <<'PY'
import hashlib
import math
import re
import statistics
import sys
from pathlib import Path
log, report, date, qemu = sys.argv[1:]
raw = Path(log).read_bytes()
text = raw.decode(errors="replace")
starts = list(re.finditer(r'\[fsbench\] dir=/data files=6 size=4096 clock=tsc', text))
assert len(starts) == 3, 'need exactly three TSC benchmark receipts'
rows = []
for i, start in enumerate(starts):
    chunk = text[start.start(): starts[i+1].start() if i+1 < len(starts) else len(text)]
    row = []
    for metric in ('seq_write', 'seq_read', 'create', 'unlink', 'sync'):
        match = re.search(r'\[fsbench\] ' + metric + r'=([0-9]+(?:\.[0-9]+)?)', chunk)
        assert match, f'missing {metric} in run {i+1}'
        value = float(match[1])
        assert math.isfinite(value) and value > 0, f'invalid {metric}'
        row.append(value)
    assert '[fsbench] verified=24576 bytes, removed=6 file(s)' in chunk
    assert '[fsbench] PASS: 24576 B written, read back, verified, unlinked' in chunk
    rows.append(row)
profile = re.findall(r'\[aurafs\] verify: reads=(\d+) sha_reads=(\d+) sha_writes=(\d+) hits=(\d+) slots=(\d+)', text)
assert len(profile) >= 6, 'missing per-checkpoint verification counters'
assert len({p[4] for p in profile}) == 1, 'cache configuration changed within one boot'
for values in profile:
    reads, sha_reads, sha_writes, hits, slots = map(int, values)
    assert reads > 0 and sha_writes > 0 and sha_reads + hits == reads
    assert (slots > 0 or hits == 0), 'disabled cache reported a hit'
reads, sha_reads, sha_writes, hits, slots = map(int, profile[-1])
batch_profile = re.findall(r'\[aurafs\] batch: fast=(\d+) fallback=(\d+) enabled=([01])', text)
assert len(batch_profile) >= 6, 'missing batch counters'
assert len({p[2] for p in batch_profile}) == 1
batch_fast, batch_fallback, batch_enabled = map(int, batch_profile[-1])
assert batch_fast + batch_fallback > 0
assert batch_enabled or batch_fast == 0
if batch_enabled:
    assert batch_fast > 0, 'this small-file workload should exercise the fast path'
lines = ['# AuraFS M6 — in-guest measurements and verification profile', '',
         f'Measurement date: {date}.', f'QEMU: {qemu}.',
         'TCG, qemu64, 4 vCPUs, 512 MiB RAM, 64 MiB AHCI /data; fast boot selftests.',
         'Three consecutive runs in one boot, 6 files x 4096 bytes each.',
         'Warm state is allowed; CoW append space is not reset between runs.', '',
         '| Run | seq_write MiB/s | seq_read MiB/s | create+write ops/s | unlink ops/s | sync us |',
         '|---|---:|---:|---:|---:|---:|']
for i, row in enumerate(rows, 1):
    lines.append(f'| {i} | ' + ' | '.join(f'{x:.4f}' for x in row) + ' |')
lines.append('| Median | ' + ' | '.join(f'{statistics.median(c):.4f}' for c in zip(*rows)) + ' |')
lines += ['', '## Verification profile (cumulative since mount)', '',
          f'Cache slots: {slots}; block reads: {reads}; SHA reads: {sha_reads}; SHA writes: {sha_writes}; exact-byte hits: {hits}.',
          f'Batching: enabled={batch_enabled}; same-leaf commits={batch_fast}; sequential fallbacks={batch_fallback}.',
          'Counters include mount/probe/checkpoint walks and are not physical disk I/O counts.',
          'All block reads still fetch bytes through the block cache; only identical, previously verified metadata avoids SHA recomputation.',
          '', 'Every run verified 24576 bytes and removed 6 files.',
          '`create` is create+write+close throughput, NOT isolated inode creation.',
          '`sync` is the existing whole-system sync() interval, NOT a power-loss proof.',
          'No real SSD, cold-cache, speedup, or exact durability claim is made.',
          'M0/M1 historical numbers are not a same-host A/B baseline.', '',
          f'Serial log SHA-256: `{hashlib.sha256(raw).hexdigest()}`.', '']
Path(report).write_text('\n'.join(lines))
print(f'Wrote {report}')
PY
then
    il_pass 'all measured values finite/positive; exact payload checks; report generated'
else
    il_fail 'invalid measurement receipts; report not generated'
fi
# Avoid publishing any artifact if another assertion failed.
if [ "$IL_FAIL_COUNT" -ne 0 ]; then rm -f "$REPORT"; fi
il_summary
