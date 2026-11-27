# AuraFS M6 — in-guest measurements and verification profile

Measurement date: 2026-11-24.
QEMU: QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1).
TCG, qemu64, 4 vCPUs, 512 MiB RAM, 64 MiB AHCI /data; fast boot selftests.
Three consecutive runs in one boot, 6 files x 4096 bytes each.
Warm state is allowed; CoW append space is not reset between runs.

| Run | seq_write MiB/s | seq_read MiB/s | create+write ops/s | unlink ops/s | sync us |
|---|---:|---:|---:|---:|---:|
| 1 | 0.0166 | 0.0529 | 4.2000 | 46.4000 | 254582.0000 |
| 2 | 0.0677 | 0.6208 | 17.3000 | 769.6000 | 48094.0000 |
| 3 | 0.0310 | 0.4880 | 7.9000 | 522.8000 | 32735.0000 |
| Median | 0.0310 | 0.4880 | 7.9000 | 522.8000 | 48094.0000 |

## Verification profile (cumulative since mount)

Cache slots: 0; block reads: 5456; SHA reads: 5456; SHA writes: 1663; exact-byte hits: 0.
Counters include mount/probe/checkpoint walks and are not physical disk I/O counts.
All block reads still fetch bytes through the block cache; only identical, previously verified metadata avoids SHA recomputation.

Every run verified 24576 bytes and removed 6 files.
`create` is create+write+close throughput, NOT isolated inode creation.
`sync` is the existing whole-system sync() interval, NOT a power-loss proof.
No real SSD, cold-cache, speedup, or exact durability claim is made.
M0/M1 historical numbers are not a same-host A/B baseline.

Serial log SHA-256: `4836739a480ccbc24a3cebdbe2a99a89751467de08917115ade8eb8faaa3213c`.
