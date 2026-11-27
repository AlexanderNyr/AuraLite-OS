# M6f — bounded regular-file SYS_WRITE buffers

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked, unchanged:
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Local cumulative M6a–M6f;
no commit or push. This patch is incremental on the delivered M6e patch.

## Scope and safety

Only the x86_64 SYS_WRITE VFS helper changes. Non-devfs VFS_TYPE_FILE writes
larger than 256 bytes allocate min(length, 4096) per call. Smaller writes
use the existing 256-byte stack buffer. Allocation failure falls back to
256-byte transfers without failing the request. No shared buffer, larger
stack allocation, disk format, GC, snapshot or durability changes.

Pipes/FIFOs, character devices and even file-shaped devfs nodes remain on
256-byte transfers; their historical copy-fault result is retained. The
hard-wired console branch is untouched. Redirected stdout/stderr pointing to
ordinary files use the new helper. Reads, pwrite and writev are unchanged.
This is not a pipe atomicity fix, FD lifetime fix or syscall-wide transaction.
The borrowed vnode/type query follows existing VFS lifetime assumptions;
concurrent close/reuse is not newly protected or proven safe.

The complete user range is still validated before allocation or I/O; every
chunk is fully copied by fault-recovering copy_from_user before VFS receives
it. A mid-copy fault never submits a partially initialized buffer. For regular
files only, a later copy fault now returns prior committed progress instead
of hiding it behind -EFAULT; a first-copy fault returns -EFAULT. Initial range
failure still returns -EFAULT without writing anything. Lengths above
INT64_MAX are rejected with -EINVAL before range checking. VFS errors return
prior progress if any; short/zero writes stop the loop. Impossible driver
counts greater than the submitted length are rejected without overflowing
accounting. All allocated buffers are freed on every exit.

`kernel/proc/write_bounce.h` is the actual implementation included by syscall.c
and the host test, not a transcription. Macro SYSCALL_FILE_WRITE_CHUNK
(default 4096, bounded 256..16384) permits a controlled 256-byte control build.

## Controlled A/B

Both binaries contain M6e batching ON, 16 verification-cache slots and the
same corrected TSC clock math. Same host, QEMU 10.0.13 TCG/qemu64, 4 vCPUs,
512 MiB RAM; fresh 64 MiB AHCI /data for each mode, three consecutive
6-file x 4096-byte runs per boot. Modes executed sequentially, no parallel
benchmark or regression load. Each run verifies 24576 bytes and removes
six files; explicit fsync /data after each run. Warm state is permitted.

| Metric | 256-byte control | 4096-byte default |
|---|---:|---:|
| VFS calls per full 4096-byte write (exact helper host test) | 16 | 1 |
| Median seq_write MiB/s | 0.1528 | 0.7072 |
| Median seq_read MiB/s | 1.2828 | 1.9139 |
| Median create+write+close ops/s | 39.1 | 181.1 |
| Median unlink ops/s | 319.5 | 171.8 |
| Median whole-system sync us | 9878 | 8226 |
| Cumulative block-read calls | 5149 | 2449 |
| Cumulative read SHA computations | 1317 | 507 |
| Cumulative write SHA / block seal attempts | 1049 | 239 |
| Exact-byte verification-cache hits | 3832 | 1942 |
| Same-leaf batches / fallbacks | 289 / 0 | 19 / 0 |

Write seal attempts fall **77.2%**; these are driver work counters including
mount/probe/checkpoint activity, NOT physical I/O or allocation measurements.
One 4096-byte write now generates one driver call / final inode update
instead of sixteen. Host tests prove this call count without timer noise.
The observed write ratio is **4.63x in this pair**, NOT a universal speedup.
Unlink regressed **319.5 -> 171.8 ops/s** despite its path being unchanged:
three TCG runs, cache/tree state and host scheduling do not isolate every
timing effect. Do not attribute the read/sync changes to a read-path patch,
claim hardware performance, or compare against previous sessions' timings.
No crash-atomicity, power-loss-durability or syscall-atomicity claim is made;
AuraFS durability remains WEAK and failed append allocations are unreclaimed.

## Validation

- Exact helper host tests at 256 and 4096; default ASan + UBSan (leak checking
  left enabled), PASS. Payload and offsets, 257-byte allocation, small writes,
  multiple chunks/tail, hostile range/oversize/zero length, first/later copy
  fault (mock modifies half buffer before failing), first/later VFS error,
  short/zero/impossible count, OOM fallback, all allocated-buffer exits,
  FIFO/character/file-shaped-devfs policy, retained pipe/device fault results,
  redirected stdout, PASS. This is mocked fault/OOM injection, not real guest
  concurrent unmapping or heap exhaustion.
- Full `make test-unit -j2`: PASS, exit 0. Existing optional skips: host TCC,
  TCC kernel build, -m32 TLS lane, rv64/a64 execution. Two existing stale plan
  opener diagnostics remain; no new blanket cross-architecture claim.
- Both ISO builds and benchmark gates 6/6, PASS; final ISO default4096.
- Final default ISO: hostile-pointer uaccess 4/4 (all battery cases and live
  shell), shell pipes/command lists 16/16, btrfs structural/mutation 19/19,
  /data power-off/remount 9/9, PASS.
- Registry check: all 247 integration cases registered; diff whitespace check.

The first full unit run correctly failed the existing uint64_t-cast ratchet;
redundant count casts were removed, not the ratchet raised. Final rerun PASS.

## Reproduce

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_write_bounce && build/test_write_bounce
cc -DSYSCALL_FILE_WRITE_CHUNK=256 -std=c11 -Wall -Wextra -Werror -O1 -g -I . \
  tests/unit/test_write_bounce.c -o build/test_write_bounce_256
build/test_write_bounce_256
cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I . \
  tests/unit/test_write_bounce.c -o build/test_write_bounce_sanitize
build/test_write_bounce_sanitize
make test-unit -j2
rm -f build/kernel/arch/x86_64/syscall.o
make build/kernel/arch/x86_64/syscall.o CC='clang -DSYSCALL_FILE_WRITE_CHUNK=256'
make iso -j4
bash tests/integration/cases/test_aurafs_bench.sh
cp build/aurafs_m6_report.md /tmp/chunk-256.md
rm -f build/kernel/arch/x86_64/syscall.o
make iso -j4
bash tests/integration/cases/test_aurafs_bench.sh
cp build/aurafs_m6_report.md /tmp/chunk-4096.md
bash tests/integration/cases/test_uaccess.sh
bash tests/integration/cases/test_selfhost_pipe.sh
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_data_volume.sh
```

Remove the syscall object when changing compiler flags: Make does not track
flag changes. The object-only override avoids the ISO tool-name dependency
check. Header dependency is explicit; header edits rebuild the real helper.
Default4096 ISO restored. Per-run reports include serial-log SHA-256 and
syscall object/kernel/ISO hashes:

- [256-byte control](aurafs-m6f-chunk-256-2026-11-24.md)
- [4096-byte default](aurafs-m6f-chunk-4096-2026-11-24.md)

M6 remains IN PROGRESS: safe reclamation with registered snapshot roots,
/home integration and wider workloads remain. pwrite/read/writev need their
own scope and evidence; this patch does not silently expand them.
