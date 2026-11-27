# AuraFS M6 — in-guest measurements and verification profile

Measurement date: 2026-11-24.
QEMU: QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1).
TCG, qemu64, 4 vCPUs, 512 MiB RAM, 64 MiB AHCI /data; fast boot selftests.
Three consecutive runs in one boot, 6 files x 4096 bytes each.
Warm state is allowed; CoW append space is not reset between runs.

| Run | seq_write MiB/s | seq_read MiB/s | create+write ops/s | unlink ops/s | sync us |
|---|---:|---:|---:|---:|---:|
| 1 | 0.0147 | 0.2103 | 3.8000 | 34.4000 | 108200.0000 |
| 2 | 0.1502 | 2.0448 | 38.4000 | 856.0000 | 10605.0000 |
| 3 | 0.0259 | 0.1312 | 6.6000 | 166.2000 | 196017.0000 |
| Median | 0.0259 | 0.2103 | 6.6000 | 166.2000 | 108200.0000 |

## Verification profile (cumulative since mount)

Cache slots: 16; block reads: 5456; SHA reads: 1931; SHA writes: 1663; exact-byte hits: 3525.
Batching: enabled=0; same-leaf commits=0; sequential fallbacks=289.
Counters include mount/probe/checkpoint walks and are not physical disk I/O counts.
All block reads still fetch bytes through the block cache; only identical, previously verified metadata avoids SHA recomputation.

Every run verified 24576 bytes and removed 6 files.
`create` is create+write+close throughput, NOT isolated inode creation.
`sync` is the existing whole-system sync() interval, NOT a power-loss proof.
No real SSD, cold-cache, speedup, or exact durability claim is made.
M0/M1 historical numbers are not a same-host A/B baseline.

Serial log SHA-256: `11ae7e9877464464500c44e9a10b71ecc6f099f13b0b690471f9640dc977d603`.
