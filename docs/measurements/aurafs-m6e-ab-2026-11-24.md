# AuraFS M6e — batched-write A/B (2026-11-24)

Base: GitHub `fbab5ed` + local M6a through M6e; no upstream push.
Same host, sequential measurements, QEMU 10.0.13 TCG, qemu64, 4 vCPUs,
512 MiB RAM, fresh 64 MiB AHCI `/data` per boot. Three consecutive runs per
configuration; each verifies 6 files x 4096 bytes, unlinks and then fsyncs.
Both binaries include M6d's corrected clock and default 16-entry verification
memoization. Only `AURAFS_BATCH_WRITES` differs (0 versus default 1).

| Median | Sequential fallback (0) | Batching (1) |
|---|---:|---:|
| Write MiB/s | 0.0259 | 0.0704 |
| Read MiB/s | 0.2103 | 0.7558 |
| Create+write+close ops/s | 6.6000 | 18.0000 |
| Unlink ops/s | 166.2000 | 267.4000 |
| Whole-system sync us | 108200.0000 | 35289.0000 |

Observed median write ratio in this pair: **2.72x**. The workload is tiny
and noisy; this is not a statistical/hardware-wide performance guarantee.
The more deterministic result is less metadata work:

| Cumulative counter after final checkpoint | Off | On |
|---|---:|---:|
| `blk_read` calls | 5456 | 5149 |
| Read SHA computations | 1931 | 1317 |
| Write SHA computations / block seal attempts | 1663 | 1049 |
| Same-leaf commits | 0 | 289 |
| Sequential fallbacks | 289 | 0 |

**36.9% fewer block seal attempts** on this successful workload. This is NOT
physical device write amplification: counters include probe/checkpoint work,
and the buffer cache can coalesce device commands. No claim of reclaimed
space, crash atomicity, real SSD speed or tail-latency improvement.

## Implementation boundaries

Up to 32 edits (at most 64 bytes per item) are staged in bounded driver
scratch. If every key routes to one existing leaf and the result fits that
leaf, rebuild the leaf and each ancestor once. No fast-path tree splits.
If keys span leaves or do not fit, use the existing split-capable inserter
with private intermediate roots. Larger writes flush bounded groups, with
one reserved slot for the final inode record. Driver scratch remains shared;
callers still MUST serialize, as with the rest of this driver.

Only the successful final inode+extent update publishes the new live root.
On failure, the old live root/inode size remain, but newly allocated orphan
blocks are NOT reclaimed. This is a property of ONE DRIVER CALL under the
serialized-caller contract, not of an arbitrary userspace write syscall or
a power cut. The syscall layer currently splits writes into **256-byte**
driver calls (`SYSCALL_IO_CHUNK`), which remains a measured-path limitation
for the next slice. No syscall buffering behavior is changed in M6e.

The disabled baseline uses the same private-root publication scheme, with
sequential inserts instead of the fast path, isolating the batching effect.
Persistent format, checksum coverage and snapshot roots are unchanged.

## Verification

Host tests ON/OFF, ON ASan/UBSan, M5 regression, full `make test-unit -j2`
(exit 0, documented optional skips), both ISO builds; QEMU btrfs 19/19,
`/data` power-off/remount 9/9, both benchmark gates 6/6. Host coverage includes
141137-byte writes (over one staging group), old snapshot contents after
replacement, forced cross-leaf fallback, and metadata ENOSPC after one data
block was accepted: old live contents/root remain byte-exact and retry works.

## Reproduce

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
rm -f build/kernel/fs/btrfs.o
make build/kernel/fs/btrfs.o CC='clang -DAURAFS_BATCH_WRITES=0'
make iso -j4
bash tests/integration/cases/test_aurafs_bench.sh
cp build/aurafs_m6_report.md /tmp/batch-off.md
rm -f build/kernel/fs/btrfs.o
make iso -j4
bash tests/integration/cases/test_aurafs_bench.sh
cp build/aurafs_m6_report.md /tmp/batch-on.md
```

Removing the object when switching flags is REQUIRED: this Makefile does not
track command-line compiler-flag changes. Direct `CC='clang -D...' make iso`
also trips the tool-name dependency check; the object-only command is deliberate.
Default-on kernel and ISO are restored after this experiment.

Generated per-run reports, configuration and serial-log SHA-256:
- [batch off](aurafs-m6e-batch-off-2026-11-24.md)
- [batch on](aurafs-m6e-batch-on-2026-11-24.md)
