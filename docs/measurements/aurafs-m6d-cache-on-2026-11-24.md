# AuraFS M6 — in-guest measurements and verification profile

Measurement date: 2026-11-24.
QEMU: QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1).
TCG, qemu64, 4 vCPUs, 512 MiB RAM, 64 MiB AHCI /data; fast boot selftests.
Three consecutive runs in one boot, 6 files x 4096 bytes each.
Warm state is allowed; CoW append space is not reset between runs.

| Run | seq_write MiB/s | seq_read MiB/s | create+write ops/s | unlink ops/s | sync us |
|---|---:|---:|---:|---:|---:|
| 1 | 0.0582 | 0.8161 | 14.9000 | 209.4000 | 32313.0000 |
| 2 | 0.0249 | 0.3377 | 6.4000 | 319.7000 | 89860.0000 |
| 3 | 0.0876 | 1.2256 | 22.4000 | 293.1000 | 15195.0000 |
| Median | 0.0582 | 0.8161 | 14.9000 | 293.1000 | 32313.0000 |

## Verification profile (cumulative since mount)

Cache slots: 16; block reads: 5456; SHA reads: 1931; SHA writes: 1663; exact-byte hits: 3525.
Counters include mount/probe/checkpoint walks and are not physical disk I/O counts.
All block reads still fetch bytes through the block cache; only identical, previously verified metadata avoids SHA recomputation.

Every run verified 24576 bytes and removed 6 files.
`create` is create+write+close throughput, NOT isolated inode creation.
`sync` is the existing whole-system sync() interval, NOT a power-loss proof.
No real SSD, cold-cache, speedup, or exact durability claim is made.
M0/M1 historical numbers are not a same-host A/B baseline.

Serial log SHA-256: `ba7f637db84dd4f336b967c4e01f9395cc86747b66572a5fd4f2c5b133decd94`.
