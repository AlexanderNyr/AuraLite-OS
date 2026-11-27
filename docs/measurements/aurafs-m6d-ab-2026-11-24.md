# AuraFS M6d — same-host verification-cache A/B (2026-11-24)

Base: `fbab5ed` + local M6a/M6b/M6c/M6d, not a pushed upstream commit.
Both binaries include the corrected TSC calibration and four-decimal rate
receipts; they differ in `AURAFS_VERIFY_CACHE_SLOTS` (0 versus default 16).
QEMU 10.0.13 TCG, qemu64, 4 vCPUs, 512 MiB RAM, 64 MiB AHCI `/data`;
fast boot selftests. Each configuration starts a fresh disk and boot and
runs 3 consecutive 6-file x 4096-byte workloads. Same host; not parallel.

| Median metric | Cache off | Cache on (16) |
|---|---:|---:|
| Write MiB/s | 0.0310 | 0.0582 |
| Read MiB/s | 0.4880 | 0.8161 |
| Create+write+close ops/s | 7.9000 | 14.9000 |
| Unlink ops/s | 522.8000 | 293.1000 |
| Whole-system sync us | 48094.0000 | 32313.0000 |

Observed median read ratio: **1.67x** in this pair. This is a tiny,
noisy warm-workload comparison, not a universal speedup. In particular,
unlink regressed in this pair: 522.8 -> 293.1 ops/s.
The earlier probes varied widely; no hard speed floor or hardware claim is
inferred. The stable result is the reduction in verification work:

| Cumulative `blk_read`/`blk_write` counters at last checkpoint | Off | On |
|---|---:|---:|
| `blk_read` calls | 5456 | 5456 |
| SHA-256 computations in `blk_read` | 5456 | 1931 |
| Exact-byte verified-metadata hits | 0 | 3525 |
| SHA-256 computations in `blk_write` | 1663 | 1663 |

Read-side SHA recomputations decreased **64.6%**; writes did not decrease.
Counters include mount/probe/checkpoint tree walks, not just timed payload
loops. They exclude direct superblock-validation hashing and are NOT
physical device I/O counters or a measurement of CPU-time percentages.

## Integrity contract

Every `blk_read` still fetches the current bytes via `fs_read_block`.
Only leaf/root metadata can reuse an earlier verification, and only after
an exact comparison of ALL 4096 bytes, including header and digest trailer.
Changed bytes run SHA-256 and must match the trailer. Data and superblocks
always rehash. A successful unchanged metadata hit does not bypass the
underlying block-cache read or serve a separate stale block copy.
Like before, changes below the underlying block cache become visible only
when that cache refreshes. This is not a new physical bit-rot detector.

The direct-mapped 16-entry table uses 64 KiB of byte copies plus tags;
no heap allocation, format change, persistent snapshot or reclamation.
Mount attempts reset it. Existing serialized-driver call requirements apply.
Tests modify already-warmed header, payload and digest bytes via direct
`fs_write_block` (no FS helper invalidation) and require checksum rejection.

## Reproduce

```bash
# Run from a cleanly built tree containing M6a through M6d.
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
rm -f build/kernel/fs/btrfs.o
make build/kernel/fs/btrfs.o CC='clang -DAURAFS_VERIFY_CACHE_SLOTS=0'
make iso -j4
bash tests/integration/cases/test_aurafs_bench.sh
cp build/aurafs_m6_report.md /tmp/cache-off.md
# Restore default-on object explicitly: make does not track compiler flags.
rm -f build/kernel/fs/btrfs.o
make iso -j4
bash tests/integration/cases/test_aurafs_bench.sh
cp build/aurafs_m6_report.md /tmp/cache-on.md
```

`CC='clang -D...' make iso` directly is NOT supported by the repository's
deps-check, which splits tool names; the object-only build above is deliberate.
Do not compare binaries built with different benchmark clock helpers.

Raw per-run tables and log SHA-256 identifiers:
- [cache off](aurafs-m6d-cache-off-2026-11-24.md)
- [cache on](aurafs-m6d-cache-on-2026-11-24.md)

An exploratory disabled-cache run was rejected because its two-decimal write
receipt rounded to 0.00, despite payload checks passing. That failed report
is NOT included as a successful timing sample. Four-decimal receipts fix
this precision problem. Separately, inspection found the unsigned-nanosecond
calibration bug at a second boundary; the clock helper now has a unit test.
Earlier M6c timings are retained as historical observations, not trustworthy
A/B baselines across the clock correction.
