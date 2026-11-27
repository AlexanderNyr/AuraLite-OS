# M6p — volatile device-cache and abrupt power-loss host model

Date: 2026-11-24, Europe/Moscow. GitHub HEAD rechecked unchanged:
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Incremental local patch on
M6o; no commit/push. No production driver or format change. User's explicit
new-empty-volume v2 opt-in / no legacy migration decision is unchanged.

## Why a different model

M6o's RAM backend immediately retained accepted writes. That detects many
publication errors but cannot distinguish direct backend readback from true
persistence. M6p adds a separate POSIX host target, `test_aurafs_power`, using
the actual driver/cache/blkdev and historical host stubs. The historical main
is renamed, not executed by this target; the old suite is also run separately.
The target is registered in Makefile UNIT_TESTS with explicit source/header
dependencies. No kernel test hook or production configuration knob is added.

## Explicit device model

- Capacity 8 MiB; volatile visible array, separate stable array, one dirty bit
  per 512-byte sector. Writes update only the visible array + dirty bits.
- Backend reads, including direct checkpoint readback, see volatile bytes.
- A successful FLUSH copies EVERY dirty sector to stable media before success.
- A failed FLUSH is configured to commit none, half (floor) or all of its
  pending dirty sectors, then returns `-EIO`. Error does not mean rollback.
- Each sweep is run in ascending AND descending LBA persistence order.
  Ascending can persist the checkpoint before higher-addressed data if the
  pre-publication barrier is removed. Descending can persist a SHA trailer
  before the header. Both expose incomplete multi-sector checkpoint images.
- Power cuts occur at numbered backend events: before/after a write,
  before/after FLUSH effects, after EVERY persisted sector inside FLUSH, and
  after backend reads of either checkpoint slot. These are finite observed
  event boundaries, not every instruction or every possible storage ordering.
- On a cut, `longjmp` abandons the operation; no remaining driver instructions
  execute. Visible bytes are reset from stable bytes, all dirty sectors are
  dropped, filesystem cache is discarded with `bc_init`, and mount starts
  with BOTH creation permissions OFF. No clean unmount/extra FLUSH occurs.
- Each cut/fault case runs in its own forked child; this isolates abandoned
  stacks/global locks and retained host cache allocations. The stubs have
  no-op spinlocks: this is a serialized operation model, NOT lock/concurrency
  validation or execution of a real restarted kernel process.

Model self-tests show an unflushed write is readable but disappears on power
loss, whereas a successfully flushed write survives. Stable copies exist
only in the harness, not in the production filesystem. No FUA, spontaneous
background eviction, controller scheduling or volatile-cache size is modelled.
512-byte sector persistence is atomic here; intra-sector tearing, dishonest
successful FLUSH, arbitrary corruption and multiple independent media faults
are NOT covered. The two LBA orders are examples, not all sector permutations.

## Workloads and 418-case receipt

Initial formatting uses zero media with both explicit creation gates enabled.
Normal sync starts from a fully persisted OLD file of 9000 patterned bytes,
overwrites it with a distinct 9000-byte NEW pattern and syncs. Two seed states
make NEW target primary A and mirror B respectively. Appended metadata/data
therefore span multiple sectors/blocks; validation is not only a tiny string.

The successful initial-format trace has **68** observed events; each normal
sync trace has **57**. Traces include checkpoint selection reads where they
occur inside the armed operation. Cuts are injected at every trace event.
The baseline event counts are measured, not universal protocol constants.
Each requested event must be reached; a silently skipped cut is an assertion
failure. Baseline sync traces for both target slots must have equal counts.

| Scenario | Cases |
|---|---:|
| Initial-format cuts, 68 events × 2 sector orders | 136 |
| Sync cuts, 57 events × 2 target slots × 2 orders | 228 |
| Completed operations then power loss: format + two sync targets, both orders | 6 |
| Failed format FLUSH: 4 barriers × 3 commit policies × 2 orders | 24 |
| Failed sync FLUSH: 2 barriers × 3 policies × 2 targets × 2 orders | 24 |
| **Total** | **418** |

For every recovery attempt:

1. Independently validate the stable checkpoint images with the actual SHA/
   geometry validator. Mount succeeds iff a valid copy exists; write count
   must not change during selection. Initial no-copy state refuses and stays
   unmounted rather than formatting. No automatic repair is requested.
2. Existing-file recovery must select OLD or exactly the next NEW generation
   and return the complete byte-exact corresponding 9000-byte pattern. No
   mixed payload, missing tree or SHA-valid-but-unreadable root is accepted.
3. A sync that returned success MUST recover NEW after volatile loss. Failed
   pre-barrier sync recovers OLD, even when that failed barrier persisted all
   appended data. Failed post-barrier sync with full persistence recovers NEW;
   with none/half persistence it recovers OLD in this sector model. A cut may
   recover either complete generation according to the persisted target.
4. Failed initial barriers have explicit stable-copy count expectations:
   first/second have no checkpoint; third has A only if the full pending slot
   persisted; fourth always retains A, and also B only with full persistence.
5. Every successfully recovered initial/existing tree is extended with an
   ALIVE file, synced, subjected to another volatile/cache loss, remounted
   without writes and read back byte-exact. This traverses/extends metadata,
   not just counts sealed checkpoint receipts.

Initial no-copy interrupted media is not re-provisioned in this matrix. M6o's
separate explicit zero-only retry tests cover that policy. No orphan reclaim
or allocation-reuse proof is inferred from successful continued operation.

## Two negative controls

Both use temporary copied sources; neither sabotage hook is delivered.

- **Omit post-publication FLUSH** only for a completed normal sync (seed setup
  and all other operations remain honest). Replace its barrier result with
  the successful pre-barrier result. The driver returns success and direct
  backend readback sees NEW, but NEW remains volatile. After power loss the
  test fails the required NEW-generation assertion, recovering OLD instead.
- **Omit pre-publication FLUSH** only in cut normal-sync runs, using ascending
  persistence order. The post barrier can persist the lower-addressed target
  checkpoint before the higher appended tree/data. A cut at event 13 exposes
  a valid NEW super with unreadable referenced file; the byte-exact file-read
  assertion fails. The original source passes all cut points.

These demonstrate sensitivity to both barriers, not a test that passes merely
because filesystem-cache reads conceal the omitted device operations.

## Validation actually run

- Final new power target: **418/418 PASS**.
- Final new target under ASan + UBSan: **418/418 PASS**;
  `ASAN_OPTIONS=detect_leaks=0` for retained historical cache allocations.
- Existing actual-driver M6a–M6o suite and fsformat API: PASS.
- M5 host regression: PASS (layout/splits/lookup/CoW/delete/reinsert/oversize/
  root growth/hash collisions).
- Both negative controls fail at their intended NEW-generation / file-read
  assertions; normal sources pass.
- Integration registry 249 cases, all registered; no new integration case.
- Width sweep 448/449 casts, 69/69 x64 includes, cross-arch 0, 27/27 asm PASS.
- Diff whitespace and incremental HEAD+a..p application/file equality checked.
- No full unit-suite, ISO, real QEMU reboot or hardware power-cut rerun in
  this host-test-only slice. UNIT_TESTS membership is not an execution receipt.
  The restored host lacks rustc, so Makefile discovery emits diagnostics while
  these explicit C targets still build and pass. No guest/Rust coverage claim.
  M6n guest receipts remain historical, not new evidence from this model.

Reproduction (POSIX host with C compiler and Python, from HEAD + a through p):

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

## Remaining boundaries

This model strengthens bounded crash evidence, not a proof for real SSDs,
AHCI firmware or arbitrary write reordering. It covers one serialized mutator,
not active FDs across remount, background writeback races or complete root
lifetime accounting. Checkpoint selection still does not recursively fsck
all candidate trees. No automatic migration, repair, on-disk format change,
root retirement, block reuse, garbage collection or performance improvement.
Durability declaration remains WEAK, reclaim stays OFF, M6 IN PROGRESS.
