# M6j — preserve referenced vnode identity across pathname changes

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked unchanged at
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Local incremental patch on M6i;
no commit/push. M6 remains IN PROGRESS. No benchmark or speed claim.

## Concrete lifetime defect

VFS already increments vnode.vfs_refs once per newly allocated OFD and drops
it on final OFD destruction; dup/fork FD slots share the OFD. Normal AuraFS
cache eviction checked this count, but two other paths bypassed identity:

1. `bv_evict(path)` marked the cache entry inactive after unlink/rename even
   while referenced. The next free-slot allocation could memset and retarget
   that vnode underneath an open file.
2. `bv_intern()` matched reusable path strings, then changed objectid. After
   unlink/recreate (or replacing a name), an old referenced vnode could become
   a different inode. Hard-link paths also had separate vnode instances,
   defeating shared visible size and per-vnode append-lock identity.

## Change

- Intern by filesystem inode/objectid, not pathname. A live cache hit never
  retargets its objectid. Hard-link aliases share the same vnode, size and
  existing per-vnode append-lock object within this mount/cache lifetime.
- Unlink/rename cache invalidation clears the name of a referenced entry,
  leaving its in_use identity and inode intact. An unreferenced entry may
  still be made inactive normally. Path and vnode.name are diagnostic cache
  labels, not authoritative inode identity or open-file pathnames.
- Even the inactive/free-slot path checks vfs_refs == 0. Existing fallback
  cache replacement already requires zero refs. All-pinned exhaustion returns
  NULL instead of recycling a referenced slot.
- Final OFD release uses the existing VFS refcount decrement; no new close
  callback, VFS ABI, refcount implementation or disk format is introduced.
  Zero-ref entries are eligible for the existing bounded cache replacement.

Unlinked inode/extent records already remain in the live tree; reads/writes
by the retained objectid therefore continue to find their data and nlink=0.
M6h's conservative audit follows those stored records, not just names. This
patch does NOT reclaim zero-link records, build open-file root snapshots,
or prove that future inode deletion/GC would preserve them automatically.

## Evidence

A driver-level helper is exercised by the host test and the existing in-guest
btrfs self-test. It pins vnode references according to the existing VFS
contract, writes OLD, unlinks, recreates the same name and writes NEW; checks
that vnode/inode identities differ, the old handle still reads OLD, writing
GONE through it leaves the new file unchanged, and old stat reports nlink=0.
Rename and hard-link lookups must return the same pinned new vnode; reads
survive both link removals. All helper exits drop their own test pins.

Host pressure tests perform 128 cache interning operations while an unlinked
vnode is pinned; its inode and payload survive. Pinning all 64 slots rejects
an unrelated lookup without changing any cached identity. Making only the
old slot zero-ref proves that replacement can reuse it after the simulated
last-OFD release. A following reachability audit succeeds. Pressure intern
records are cache-only synthetic identities, not new on-disk inode records.

A negative control compiled the current tests/helper with the PRE-M6j vnode
intern/evict implementation: it aborted at the new identity helper assertion
as required. This proves that the test distinguishes the old defect from the
fix, not merely that two new implementations agree.

`tests/btrfs/test_btrfs.sh` adds a mandatory receipt for the new driver-level
helper. Both host and guest helper model OFD pins directly through vfs_refs;
this is NOT an end-to-end userspace open/dup/fork/close or concurrent-FD test.
Existing VFS code was inspected but not changed. No new syscall is added.

Validation outcomes are recorded in CHANGELOG after completion. ASan/UBSan
uses detect_leaks=0 for existing globally retained driver/cache allocations.
Optional host TCC, -m32 TLS, rv64/a64 execution skips remain explicit; they
are not new coverage. No performance measurement is made in this slice.

## Limits and next steps

Driver scratch and vnode cache still require serialized callers. Atomic VFS
reference increments do NOT make pathname lookup vs OFD acquisition or
concurrent unlink/close/reuse safe; no lock/lifetime protocol is added here.
Borrowed lookup pointers with zero VFS refs remain evictable and are not
promised stable. Mount attempts still reset the vnode pool; active OFDs across
remount are not supported/proven safe. Rename replacement semantics and path
metadata correctness are not generalized by these tests. Resource failure
may still leave namespace mutations if create allocates an inode before
failing to intern a vnode; transaction rollback is not implemented.

The allocator is append-only, checkpoint history is conservative rather than
a durable-root proof, reclaim stays none and durability WEAK. Before reuse,
prove checkpoint publication/retirement and complete root/lifetime handling,
including actual VFS open-unlinked descriptors and failure/remount cases.
This patch removes a concrete serialized identity bug, not all GC blockers.

## Reproduce

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
