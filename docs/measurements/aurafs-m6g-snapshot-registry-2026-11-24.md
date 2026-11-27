# M6g — registered volatile snapshot roots

Date: 2026-11-24 (Europe/Moscow). GitHub HEAD rechecked unchanged at
`fbab5edba13498d6843cf443e1df8cf55b72e18c`; local incremental patch on M6f.
No commit or push, no benchmark or speed claim. M6 remains IN PROGRESS.

## Why this slice precedes reclamation

The old token contained an arbitrary old root, with no registry and no way
for a future collector to discover active snapshots. Clearing one value copy
also left other copies readable. This was safe only because no blocks were
reused. M6g registers every successful capture and makes reads depend on an
exact, still-active registration. It does NOT enable block reuse.

## Contract

- Fixed 32-entry static registry: 1280 bytes on the current x86_64 ABI (a
  32-byte token and owner pointer per entry), no heap/stack-sized table and no
  allocation, data copies, checkpoint or device I/O when creating/releasing.
- Each capture has a monotonically increasing, nonzero 64-bit registration
  ID. IDs are never reused across mount attempts. Exhaustion fails closed
  until kernel restart instead of wrapping. Same-root captures remain
  independent handles, including when root and allocation end are identical.
- Capture need not read initialized output memory. It reserves the output
  address until release and rejects creating over an active owner address;
  capacity/argument/ID exhaustion failures leave output unchanged. Caller
  must keep the address reserved, not let a live owner go out of scope.
- The driver copies the captured root, allocation end, mount epoch and ID
  into its registry. Read/release require all four to match an entry.
  An altered token cannot accidentally unregister the original entry.
- Token copies and enumeration results are aliases, NOT independent
  references. Releasing any exact alias invalidates all copies of that
  handle and frees its slot; releasing a stale alias cannot unregister a
  replacement handle occupying the same slot. Releasing NULL or an already
  released handle is harmless. This deliberately strengthens the old copied
  token lifetime semantics, not a persistent snapshot ABI compatibility claim.
- `aurafs_snapshot_roots(NULL, 0)` queries the active count. Otherwise output
  capacity must hold the entire set: insufficient capacity gives -1 before
  any output writes. Enumeration includes each active registration, even
  duplicate roots; it neither captures nor pins additional references.
  Unmounted enumeration fails with -1. Order is an implementation detail.
- Every mount attempt clears the registry, including failed mounts. Mount
  epoch validation remains; persistent IDs prevent ABA even if epochs wrap.
- API is for trusted kernel callers, not an authorization boundary. All
  calls MUST be serialized with ordinary FS operations; no new locking,
  lifetime reference scheme, callback/reentry or concurrency claim is added.

## Host evidence and regression gates

`tests/unit/test_aurafs_m6.c` includes the actual driver, buffer cache,
blkdev and SHA implementation. New tests cover all 32 entries occupied,
identical roots with distinct IDs, count/full enumeration, insufficient
capacity with unchanged output, NULL/capacity mismatch, failed capture with
unchanged output, active-owner overwrite rejection, altered root/end/epoch/ID,
release through enumeration alias, read after alias release, slot reuse with
new ID, stale release not freeing replacement, double/NULL release, simulated
ID exhaustion, and unchanged driver cursor/root/device-write count.

Existing multi-block old/new snapshot payload tests, overwrite/truncate/
rename/unlink/recreation and corruption detection still pass. Remount tests
check an actual active token expires, registry is empty, fresh IDs increase,
and releasing an expired token does not remove a new registration. Failed
mount clears all slots and rejects enumeration/capture/read.

Validation commands:

```bash
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_aurafs_m6 && build/test_aurafs_m6
cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -I . tests/unit/test_aurafs_m6.c \
  kernel/fs/buffer_cache.c kernel/fs/blkdev.c kernel/lib/sha256.c \
  -Wl,--gc-sections -o build/test_aurafs_m6_sanitize
ASAN_OPTIONS=detect_leaks=0 build/test_aurafs_m6_sanitize
bash tests/btrfs/test_m5_host.sh
make test-unit -j2
make iso -j4
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_data_volume.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
git diff --check
```

Host and ASan/UBSan PASS; leak detection disabled for the existing globally
retained driver/cache allocations. Full unit target, M5, ISO, QEMU btrfs and
/data persistence outcomes are recorded in CHANGELOG after completion.
Guest regression gates do not exercise this kernel-only snapshot API; its
registry, saturation and fault/expiry evidence is host-side, not a new guest
snapshot command. Existing optional toolchain skips still apply.

## Explicit stop line before GC

Enumeration is one prerequisite, NOT a safe collector. Future design must:

1. Preserve live roots, every registered snapshot root, AND any root still
   reachable from the durable checkpoint. Never assume registry output is
   the complete root set or mark only files reachable by pathname.
2. Walk every metadata child/extent with validated bounds, levels and hashes;
   account for hard links, stale/deleted inode/extent records, partial blocks
   and allocation tails. Treat corruption, capacity limits and incomplete
   traversal as abort-without-reuse, not a free-space hint.
3. Define checkpoint ordering and failure recovery before exposing reused
   blocks. Existing single in-place superblock and absent device FLUSH still
   imply WEAK durability; an in-memory bitmap alone cannot fix this.
4. Serialize capture/release, mutation, checkpoint and mark/reuse or supply
   a separately tested lifetime/locking protocol. Preserve live open vnode
   behavior and handle failed-write orphan allocations conservatively.
5. Prove reuse through ENOSPC recovery, snapshot old/new payloads, release,
   remount, corruption and injected write/checkpoint failures before enabling
   allocator changes. Do not silently change the on-disk format.

M6g deliberately leaves the append-only allocator, ENOSPC policy, mount
receipt `reclaim=none`, snapshot persistence and /home mounting unchanged.
