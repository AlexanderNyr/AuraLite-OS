# M6h — bounded read-only reachability diagnostic

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked unchanged at
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Incremental local patch on M6g;
no commit/push, no benchmark or speed claim. M6 remains IN PROGRESS.

## What is implemented

`aurafs_reachability_audit()` walks the union of the current live tree,
the checkpoint image observed through the filesystem block cache, and all
registered volatile snapshots. Each root retains its own allocation-end
bound. Duplicate roots/handles count as separate root inputs but blocks are
counted once across the entire union. Every stored extent record is followed,
including extents not reachable by pathname or beyond an inode's current EOF;
there is no inode/pathname-based pruning or reclaim decision.

Success reports allocated append blocks, unique metadata blocks, unique data
blocks, their unreferenced difference, and the number of root inputs. The
reserved prefix/superblock is excluded from allocated and reachable counts.
No bitmap or reusable-block list is exposed. Failure returns -1 and leaves
the caller's report unchanged; partial results are never published.

The diagnostic makes no logical filesystem writes and does not change the
allocator, live root, namespace, snapshots or checkpoint. Reads use the normal
block cache and verification path, updating verification counters/cache state.
A cache miss may evict/write back an unrelated dirty cache entry: this is NOT
a promise of zero device writeback, zero side effects on caches, or unchanged
performance counters. The host no-device-write assertion uses a resident
working set and is not generalized to arbitrary cache pressure.

## Validation and resource bounds

- Byte-address alignment and complete-block bounds checked before each read;
  pointers must be at/above 69632 and below that root's allocation end.
- Metadata/data SHA verification uses the actual `blk_read()` path; verified
  metadata may reuse M6d's exact-byte verification memoization, data always
  hashes. Block self-address and allowed type are checked. Extent data owner
  must match the extent key's inode number. Metadata/data role conflict fails.
- Internal pointer counts, nonempty internal nodes and strictly increasing
  separator keys checked. Leaves check count, strictly increasing keys,
  descriptor/data boundaries, payload overlap, known record types and sizes,
  directory name length, extent payload length and aligned logical offset.
  Existing zero-length deleted directory hash slots are accepted as tombstones.
- Iterative depth-first traversal, no recursive kernel-stack buffers. Active
  ancestor cycle detection and depth limit of eight internal edges. Each
  root's traversal state is reset independently: a previously visited root
  cannot bypass a stricter snapshot/checkpoint allocation bound.
- At most 262144 addressable blocks (1 GiB append-address span), 4194304 DFS
  loop steps total, and 34 root inputs. Exceeding any bound aborts, never yields
  a partial map. Repeated data extents may be re-read for owner/hash validation.
- Two byte-per-block arrays plus nine heap traversal frames, data scratch
  and root tokens use less than 564 KiB per call at the maximum span. One
  bounded heap allocation, freed on all success/error exits. Allocation
  failure returns -1 with unchanged output. No new global walk scratch.
- Calls must be serialized with ALL normal FS, checkpoint and snapshot
  operations. No new reentrancy, concurrency or open-vnode lifetime guarantee.

## Host evidence

The dedicated tests include the actual driver, cache, block device and SHA
implementation. An existing real multi-leaf filesystem is audited first.
A separately constructed valid three-block tree (internal root, extent leaf,
data block) proves unique union accounting: after switching live root away,
a registered snapshot retains all three; release reduces the reachable
union by exactly three. Live and cache-visible checkpoint roots are both
included. Existing snapshot old/new payload/corruption tests remain in the
same suite, but are not a claim of executing this API in the guest.

Injected cases reject: OOM, oversized address span, corrupt checkpoint SHA,
self-cycle, child outside allocation horizon, unaligned child, oversized
internal pointer count, leaf payload overlapping descriptor table, overflowing
record size, metadata address used as data extent, data checksum failure,
wrong data owner, re-sealed wrong self-address, and excessive tree depth.
Valid bytes are restored and a final audit succeeds with unchanged cursor
and no device writes in that resident working set. Tests verify unchanged
output on OOM/checkpoint error. Existing M6a–g host tests still pass.

ASan/UBSan pass with `detect_leaks=0` for existing globally retained cache/
driver allocations. Full unit/M5/ISO/QEMU regression results are recorded
in CHANGELOG when complete. Host TCC, -m32 TLS, rv64/a64 execution remain
optional toolchain skips, not executed coverage. No new guest shell command,
syscall or mounted snapshot interface is introduced.

## Stop line — this does NOT authorize block reuse

The word **unreferenced** means absent from this diagnostic's observed roots,
NOT safe to free. The checkpoint read goes through the cache and can be a
newly dirtied image after an unsuccessful checkpoint, while an older root
may still exist on the physical device. There is no device FLUSH guarantee,
old/new durable-root tracker, dual-superblock protocol or torn-super recovery.
Checkpoint total geometry is checked against the mounted volume, not treated
as proof of power-loss persistence.

This is also NOT a full filesystem checker: parent separator/subtree range
agreement, uniform leaf depth, directory/inode consistency, open-unlinked
file semantics and every semantic corruption are not proven. It follows
all stored references conservatively but must not replace future GC safety
proofs. Corruption/bounds/resource failures abort without reclaiming anything.
The DFS step-cap branch is implemented but not forced by the small host disk.

The allocator remains append-only; failed append allocations remain allocated;
mount receipt remains `reclaim=none durability=WEAK`. Before enabling reuse,
implement/prove durable checkpoint-root retention and ordering, complete root
lifetime coverage and appropriate structural/semantic checks, then fault-test
ENOSPC recovery, snapshot pins, open files, interrupted writes/checkpoints,
and remount. M6h deliberately does not claim these properties.

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
