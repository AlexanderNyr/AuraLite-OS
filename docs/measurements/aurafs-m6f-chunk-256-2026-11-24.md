# AuraFS M6 — in-guest measurements and verification profile

Measurement date: 2026-11-24.
QEMU: QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1).
TCG, qemu64, 4 vCPUs, 512 MiB RAM, 64 MiB AHCI /data; fast boot selftests.
Three consecutive runs in one boot, 6 files x 4096 bytes each.
Warm state is allowed; CoW append space is not reset between runs.

| Run | seq_write MiB/s | seq_read MiB/s | create+write ops/s | unlink ops/s | sync us |
|---|---:|---:|---:|---:|---:|
| 1 | 0.1528 | 1.6534 | 39.1000 | 377.6000 | 9584.0000 |
| 2 | 0.0633 | 0.6049 | 16.2000 | 188.7000 | 64237.0000 |
| 3 | 0.1850 | 1.2828 | 47.4000 | 319.5000 | 9878.0000 |
| Median | 0.1528 | 1.2828 | 39.1000 | 319.5000 | 9878.0000 |

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

Serial log SHA-256: `463ae34991e451ccd021f726cd716605e72398eaecb50de7ef3ea8c8d45f0e39`.

## M6f build identity

`SYSCALL_FILE_WRITE_CHUNK=256`; metadata batching ON, verification slots 16.

```text
f00bd4d67e0310ebcda435c3cad079f6564d079de626dd473a6270b5067e0623  build/kernel/arch/x86_64/syscall.o
8f56de31883ec92feb8a43ea63817b941ea34f4366393c1d9e7386843ae753fe  build/kernel.elf
7b8b1c9edb2e4532443832166e00c225d25bab89e0c4f283108c148b3d75c048  build/auralite-dual.iso
```
