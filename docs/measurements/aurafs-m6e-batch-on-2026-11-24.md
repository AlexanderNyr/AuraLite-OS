# AuraFS M6 — in-guest measurements and verification profile

Measurement date: 2026-11-24.
QEMU: QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1).
TCG, qemu64, 4 vCPUs, 512 MiB RAM, 64 MiB AHCI /data; fast boot selftests.
Three consecutive runs in one boot, 6 files x 4096 bytes each.
Warm state is allowed; CoW append space is not reset between runs.

| Run | seq_write MiB/s | seq_read MiB/s | create+write ops/s | unlink ops/s | sync us |
|---|---:|---:|---:|---:|---:|
| 1 | 0.0601 | 0.7558 | 15.4000 | 249.0000 | 54534.0000 |
| 2 | 0.0929 | 1.5040 | 23.8000 | 271.8000 | 8494.0000 |
| 3 | 0.0704 | 0.7294 | 18.0000 | 267.4000 | 35289.0000 |
| Median | 0.0704 | 0.7558 | 18.0000 | 267.4000 | 35289.0000 |

## Verification profile (cumulative since mount)

Cache slots: 16; block reads: 5149; SHA reads: 1317; SHA writes: 1049; exact-byte hits: 3832.
Batching: enabled=1; same-leaf commits=289; sequential fallbacks=0.
Counters include mount/probe/checkpoint walks and are not physical disk I/O counts.
All block reads still fetch bytes through the block cache; only identical, previously verified metadata avoids SHA recomputation.

Every run verified 24576 bytes and removed 6 files.
`create` is create+write+close throughput, NOT isolated inode creation.
`sync` is the existing whole-system sync() interval, NOT a power-loss proof.
No real SSD, cold-cache, speedup, or exact durability claim is made.
M0/M1 historical numbers are not a same-host A/B baseline.

Serial log SHA-256: `c73a8d7c1e784a35ece4b6d6bac63a364c8ba6d976289e9247d3272d2483d2af`.
