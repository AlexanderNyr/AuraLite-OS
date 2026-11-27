# M6l — checkpoint backend read-after-write verification

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked unchanged at
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Local incremental patch on M6k;
no commit/push. M6 remains IN PROGRESS. No speed or durable-reclaim claim.

## Publication check added

Before M6l, AuraFS sync flushed appended blocks, queued the sealed superblock,
flushed that cache range and incremented its generation. A backend that
acknowledged a dropped/partial superblock write could make sync return success;
reading back through `blk_read()` would still see the newly clean cached copy.

Now sync performs one explicit eight-sector `blkdev_read()` of the 4096-byte
superblock AFTER range writeback and BEFORE generation advancement and success
receipts. This uses the backend seam directly, bypassing the filesystem block
cache and AuraFS verification memoization. It compares ALL 4096 bytes against
the exact sealed image just submitted, including header, geometry, label,
reserved bytes and SHA trailer. A valid but different SHA-sealed image also
fails. The expected bytes already contain the write-time SHA; an independent
hash of returned bytes is unnecessary for exact equality.

Transport read error or any byte mismatch returns failure without incrementing
the generation or emitting success receipts. Existing checkpoint history
retains all earlier/attempted pairs, including the failed attempt. No rollback,
cache invalidation or history retirement is attempted. A later serialized
sync rewrites the cached checkpoint and can succeed without allocating another
history entry for the same root/end. Candidate history overflow still affects
only audit, not ordinary sync attempts.

The implementation reuses existing 4096-byte driver data staging scratch;
no new heap allocation or enlarged kernel stack. The existing serialization
contract for all driver/snapshot/audit/sync calls remains mandatory.

## Exact limits

This is **read-after-write verification, NOT device FLUSH**, FUA, a write
barrier, atomic superblock publication, rollback or torn-super recovery.
The backend/device may itself return volatile cached data. A matching read
cannot certify that power loss preserves this checkpoint or appended data.
Only the superblock is read back; silently lost appended data/metadata is
NOT detected by this additional step. A read error after a successful write
is an ambiguous outcome: sync reports failure even if the image was written.

Old candidate roots are deliberately not retired after verified readback.
On-disk format, allocator, append ENOSPC, snapshot persistence and reclaim
remain unchanged; mount receipt stays `reclaim=none durability=WEAK`.
The existing compatibility log line "fsync: persisted tree root" remains,
but a new explicit receipt immediately precedes it:

```text
[aurafs] checkpoint: backend readback verified; no device FLUSH, history retained
```

Do not interpret the historical wording as an EXACT durability upgrade.
Failure emits a separate checkpoint-readback-failed receipt and returns -1.

Each successful or readback-failing attempt reaching this stage adds one
4096-byte backend read. This changes seam/diskstats read traffic and can add
latency; it is NOT a performance optimization. These reads bypass the AuraFS
`blk_read` verification profile counters, so those counters alone no longer
describe all checkpoint-read I/O. Earlier measurement artifacts remain
historical and are not rerun or retroactively compared in this slice.

## Actual-driver host fault evidence

The tests include the actual driver, cache, backend seam and SHA code. The
RAM-device mock injects four failures, each with a fully warmed super cache:

1. Acknowledge the superblock write without storing any bytes. Backend keeps
   the prior superblock; filesystem cached read shows the submitted new root.
2. Acknowledge storing only the first 512-byte sector, leaving a stale SHA
   trailer. Full-image comparison rejects the partial checkpoint.
3. Store a different label byte and recompute a valid SHA. Normal superblock
   checksum/geometry validation accepts it, but exact submitted-image
   comparison rejects it.
4. Return a read error from the backend super read after successful writeback.

For each, sync fails, exactly one direct backend super read is observed,
generation does not advance, and the attempted pair remains in history.
A normal filesystem read still sees the submitted cached root/end. Remove
the injection and retry: one backend read, successful sync, one generation
increment, no extra history slot, exact expected physical RAM-device bytes.

Negative control compiles current tests with only the readback block removed
from a temporary driver copy: the dropped-write case fails its required
`btrfs_sync(NULL) < 0` assertion. The normal delivered source is never replaced
by that control. This verifies the new test would fail on pre-M6l behavior.

The existing QEMU btrfs gate requires the explicit backend-verification
receipt, raising its assertion count from 20 to 21. Guest gates validate the
normal AHCI path; faulty backend responses are host-injected, not emulated
hardware fault/power-loss tests. Existing M6k userspace FD/remount gate and
/data power-off/remount regressions are also rerun.

## Validation and reproduction

Host fault tests and negative control pass. Final full unit/M5/sanitizer/ISO
and QEMU results are recorded in CHANGELOG after completion. ASan/UBSan uses
`detect_leaks=0` for existing retained driver/cache allocations. Optional
TCC/multilib/rv64/a64 execution skips remain explicit; no cross-ISA execution
or device-cache durability coverage is inferred.

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_aurafs_m6 && build/test_aurafs_m6
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -I . tests/unit/test_aurafs_m6.c \
  kernel/fs/buffer_cache.c kernel/fs/blkdev.c kernel/lib/sha256.c \
  -Wl,--gc-sections -o build/test_aurafs_m6_sanitize
ASAN_OPTIONS=detect_leaks=0 build/test_aurafs_m6_sanitize
make test-unit -j4
bash tests/btrfs/test_m5_host.sh
make iso -j6
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_aurafs_fd.sh
bash tests/integration/cases/test_data_volume.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

Before retiring checkpoint candidates or reusing blocks, a separate tested
backend FLUSH/barrier contract, robust super publication/recovery protocol,
and complete root/concurrency lifetime reasoning are still required. M6l
strengthens one publication error check; it does not close M6.
