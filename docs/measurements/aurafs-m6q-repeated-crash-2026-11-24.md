# M6q — second crash after single-copy recovery

Date: 2026-11-24, Europe/Moscow. GitHub HEAD rechecked unchanged:
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Incremental local test/evidence
patch after M6p. No commit/push, runtime driver change or format change.
User's explicit empty-volume-only v2 creation/no-migration policy is unchanged.

## Gap addressed

M6p tests one interrupted operation and then successful continued operation.
M6q adds a SECOND interrupted publication after recovery from a reachable
one-valid-checkpoint state, with durable but uncommitted appended blocks.
It does not claim arbitrary-length crash sequence exploration. The unchanged
M6p model assumptions apply: honest successful FLUSH, volatile reads, 512-byte
atomic sector persistence, two deterministic LBA orders, serialized host stubs,
fork isolation and abrupt setjmp/longjmp cuts. This is not a real SSD power cut,
restart of an actual kernel, live-FD remount safety or concurrency validation.

## Four reachable degraded seeds

For each of two active-slot orientations:

1. Format explicit fresh v2, create and durably sync a 9000-byte OLD file.
   An optional extra sync selects the other active slot. Save this healthy
   stable image, active checkpoint bytes, generation and append cursor.
2. Overwrite with a distinct 9000-byte ABORT pattern. Fail the SECOND sync
   barrier after persisting half the pending checkpoint sectors. The first
   barrier durably retains all appended data/metadata of this abandoned root.
3. Perform modeled power loss: discard volatile bytes and filesystem cache.
4. Verify exactly ONE stored checkpoint is SHA/geometry-valid. Mount with
   both creation permissions OFF. Neither write count nor FLUSH count changes.
   Active slot/generation and cursor match the original OLD checkpoint; its
   full 4096 bytes remain unchanged. OLD reads byte-exact, never ABORT.
5. Verify the abandoned append range is nonempty and differs from the healthy
   stable image, then snapshot the resulting stable degraded image as a seed.

Repeat with ascending and descending persistence order in the first failed
publication: 2 survivor orientations × 2 first-crash orders = **4 seeds**.
These are generated through real driver/backend operations, not constructed
by directly editing a checkpoint signature. The saved snapshot is only a host
fixture for starting the second-crash cases reproducibly.

The driver resumes the old committed cursor. Writing into its uncommitted
suffix is existing allocator behaviour, NOT new committed-root reclamation
or permission to reuse blocks reachable from retained valid checkpoints.
No allocator or retirement policy is changed by this slice.

## Second publication matrix: 512 cases PASS

Each seed is recovered without formatting/repair, then overwritten with NEW,
which differs from BOTH OLD and ABORT across the 9000-byte patterned file.
The successful replay trace has **57 events** for all four seeds.

| Second operation | Cases |
|---|---:|
| Cut each replay event: 57 × 4 seeds × 2 second-crash sector orders | 456 |
| Complete replay then lose power: 4 seeds × 2 orders | 8 |
| Fail either replay FLUSH, persist none/half/all: 2 × 3 × 4 seeds × 2 orders | 48 |
| **New total** | **512** |

The existing M6p 418 cases also run: **930 cases overall**, not 930 independent
hardware faults. Both first- and second-crash orders and both checkpoint
orientations are covered; not every possible storage-sector permutation.

At every second recovery, BEFORE any follow-up extension:

- The entire original sole-survivor checkpoint is byte-identical to its saved
  healthy image. The second attempt must never overwrite that checkpoint.
- Cache/volatile state is lost, creation gates are OFF and mount writes nothing.
  There must still be at least one valid copy and a mounted v2 filesystem.
- Generation is exactly OLD or its next NEW generation. The full 9000-byte
  file must equal the corresponding OLD/NEW pattern, never ABORT or a mixture.
- Successful replay must recover NEW. Failed first barrier must recover OLD;
  failed second with all persistence recovers NEW, none recovers OLD. Partial
  failed second publication is ambiguous and may recover either complete one.
- Create/sync ALIVE, lose volatile/cache state again, mount without writes,
  then read BOTH ALIVE and the previously recovered OLD/NEW file byte-exact.

The original M6p path also gains the final original-file recheck after ALIVE.
No new Makefile target is needed: the registered `test_aurafs_power` target
already tracks this source. The historical M6p receipt remains separately
printed, followed by the new M6q receipt.

## Important observed nuance: torn-image completion

The first abandoned attempt and the retry can produce the same checkpoint
body: same next generation, root block addresses and cursor. The referenced
appended blocks have DIFFERENT file content but are rewritten at those same
uncommitted addresses after rollback to the committed cursor. A partial second
FLUSH can combine complementary stored portions with the previous torn target
and yield a valid complete checkpoint, even while returning an error.

This is allowed only as complete OLD/NEW recovery here. The pre-publication
barrier has already made the NEW tree/data durable before target publication,
so a valid newly completed target must serve NEW, not ABORT. Requiring OLD for
every partial failed replay would be an incorrect rollback guarantee. The
new tests explicitly permit OLD/NEW while checking generation/content together.
They do not authenticate against malicious re-sealed metadata or replace a
recursive tree-validation/fsck policy.

## Negative control

A temporary harness copy drops appended replay writes at/after the recovered
cursor while acknowledging success, ONLY for the successful second operation
in repeated mode. Seed construction and other cases remain unchanged; runtime
source is not sabotaged or delivered with hooks. The checkpoint can reuse the
same addresses and have a valid SHA despite the stale ABORT tree/data below it.

The control fails the new byte-exact payload assertion after recovery; an
extra diagnostic confirms `negative replay payload matches ABORT=1`. It does
not merely fail for a missing checkpoint or an unreachable event. Normal
source passes. This shows why a different third pattern and complete data
read are needed rather than only checking checkpoint SHA/generation or using
identical data in both failed attempts.

## Validation actually performed

- Final actual-driver volatile target: M6p **418 PASS** + M6q **512 PASS**.
- ASan + UBSan on the same combined matrix: PASS;
  `ASAN_OPTIONS=detect_leaks=0` for retained historical cache allocations.
- Existing focused actual-driver M6a–M6o and fsformat API: PASS.
- M5 host regression: PASS (layout, splits, lookup, CoW, delete/reinsert,
  oversize, root growth, hash collisions).
- Negative dropped-replay seam control: expected ABORT-pattern assertion
  failure; normal sources pass.
- Registry: 249 integration cases, all registered; no new integration case.
- Width sweep PASS: 448/449 casts, 69/69 x64 includes, zero cross-arch hits,
  27/27 asm files. Diff whitespace and HEAD+a..q chain/equality checks PASS.
- No new full unit suite, ISO or QEMU run in this host-only test slice.
  Existing UNIT_TESTS registration is not a full-suite execution receipt.
  Host Makefile discovery reports missing rustc; explicit C targets build/run.
  No new Rust, physical disk or guest crash validation is inferred.

Reproduce on POSIX host from HEAD + patches a through q:

```sh
make build/test_aurafs_power build/test_aurafs_m6 build/test_fsformat
build/test_aurafs_power
build/test_aurafs_m6
build/test_fsformat
bash tests/btrfs/test_m5_host.sh
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -I . tests/unit/test_aurafs_power.c \
  kernel/fs/buffer_cache.c kernel/fs/blkdev.c kernel/lib/sha256.c \
  -Wl,--gc-sections -o build/test_aurafs_power_sanitize
ASAN_OPTIONS=detect_leaks=0 build/test_aurafs_power_sanitize
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

## Scope boundary

No checkpoint-format/runtime change, legacy migration, mount-time repair,
root retirement, committed-block reuse, orphan collection or speed claim.
This bounds two publication failures for one serialized overwrite shape;
other workloads, faults, lifetime races and arbitrary-length histories remain
unproven. Durability WEAK, reclaim OFF, M6 IN PROGRESS. More test cases are
not by themselves permission to enable reclamation or declare EXACT.
