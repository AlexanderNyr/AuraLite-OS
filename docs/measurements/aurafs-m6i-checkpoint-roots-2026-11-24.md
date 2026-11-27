# M6i — conservative since-mount checkpoint candidate history

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked unchanged at
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Local incremental patch on M6h;
no commit/push. M6 remains IN PROGRESS; no benchmark or speed claim.

## Gap addressed

M6h could observe the new superblock through the block cache after a failed
checkpoint flush and lose the older on-device root from its diagnostic root
set. Keeping only "last successful sync" would also be insufficient: queued
superblocks can undergo ordinary cache writeback outside the explicit sync
flush, and a successful write request still lacks device-FLUSH durability.

M6i conservatively retains every distinct checkpoint candidate submitted in
the current mount, not just the cache-visible image or latest successful one.
This closes a specific diagnostic omission. It does NOT establish a complete
durable-root proof or authorize reclamation.

## Implementation and bounds

- On a successful mount, seed the history with the validated observed
  root/allocation-end pair. Every mount attempt clears history first; failed
  mounts leave no active mounted history. A remount seeds a new observation,
  not a claim to recover all previous device states.
- After appended data/metadata flush succeeds and the old superblock can be
  read, remember the current root/end BEFORE attempting to queue its new
  superblock. Thus even enqueue or superblock-flush failure cannot silently
  remove that candidate from the recorded set.
- Deduplicate exact root/end pairs. Identical repeated syncs do not consume
  slots. Same root with a different allocation horizon is a different pair.
- Do not retire candidates after successful sync. There is no FLUSH barrier,
  dual-super protocol or crash-safe proof justifying such retirement yet.
- Fixed 32-entry history (512 static bytes), independent of snapshot handles:
  release/alias lifetime cannot remove checkpoint candidates. No heap
  allocation and no extra block I/O in the bookkeeping path.
- If a 33rd distinct pair would be recorded, set a sticky overflow flag.
  **Writes and sync continue normally**, preserving their existing return
  behavior; only the reachability diagnostic fails closed until a mount
  attempt resets the history. It must not publish an incomplete root union.
- Audit includes history pairs not already represented by live/checkpoint/
  snapshot root inputs. Traversal still validates each root's own horizon.
  At most 66 root inputs (2 + 32 snapshots + 32 history), 1 GiB address span,
  <564 KiB heap workspace and the existing 4194304-step cap. More candidates
  can hit that work limit sooner; failure is still no partial report.
- Existing caller serialization requirement applies to tracking, audit,
  snapshot operations, mutation and sync. No new locking/lifetime guarantee.

## Actual-driver host evidence

Tests use the real driver/cache/blkdev/SHA implementation with injected
checkpoint device-write failure:

1. Write old content and sync it; capture its root/end. Repeated sync does
   not grow history.
2. Overwrite the same file and fail superblock writeback. Physical RAM-disk
   superblock bytes remain the previous version, while a normal cached super
   read reports the attempted new root. Both roots are retained in history;
   audit succeeds with at least three root inputs.
3. Overwrite again and successfully sync. The old successful and failed
   attempt roots remain recorded, in addition to the new root.
4. Corrupt an old data block now reachable only through checkpoint history.
   Audit rejects it; restore exact bytes and audit succeeds. There are no
   active snapshot pins providing accidental coverage of that old block.
5. Submit more than 32 distinct real write/sync pairs. Sync continues to
   succeed; history stays bounded at 32 and overflow sticks. Audit returns
   failure with an unchanged report; another successful sync does not clear
   overflow. Later existing remount tests verify reset to one seeded pair;
   failed-mount tests verify cleared history.

These are serialized fault tests against a RAM device and real buffer cache,
not device-cache/FLUSH, torn-sector or sudden-power-loss simulations. Audit
reads may cause normal dirty-cache writeback and update verification state.
Existing M6a–h snapshot/CoW/checksum/ENOSPC tests remain in the same suite.

## Validation

Host tests, ASan/UBSan (detect_leaks=0 for existing retained driver/cache
allocations), full unit suite, M5, ISO and QEMU regression outcomes are listed
in CHANGELOG after completion. Existing optional toolchain skips remain;
no rv64/a64 execution, missing TCC or -m32 coverage is inferred.
Guest btrfs/data gates are regressions, not a new guest history/audit syscall.

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_aurafs_m6 && build/test_aurafs_m6
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -I . tests/unit/test_aurafs_m6.c \
  kernel/fs/buffer_cache.c kernel/fs/blkdev.c kernel/lib/sha256.c \
  -Wl,--gc-sections -o build/test_aurafs_m6_sanitize
ASAN_OPTIONS=detect_leaks=0 build/test_aurafs_m6_sanitize
make test-unit -j2
bash tests/btrfs/test_m5_host.sh
make iso -j4
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_data_volume.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

## Stop line

History tracks complete candidate root/end pairs submitted during this mount.
It cannot reconstruct a torn or reordered physical superblock, prove device
persistence, preserve previous mount history or cover unknown out-of-band
writes. Keeping all candidates is conservative, not a crash-consistency
protocol. Overflow and other audit failures are never permission to reuse.

Next requirements remain: durable checkpoint publication/retirement protocol,
complete root lifetime coverage (including open-unlinked files), stronger
structural/semantic validation where GC requires it, and injected-failure /
remount proofs for actual reuse. The allocator remains append-only, failed
append allocations are not reclaimed, mount receipt stays
`reclaim=none durability=WEAK`, and no on-disk format change is introduced.
