# M6s — mandatory bounded v2 metadata-tree verification

Date: 2026-11-24, Europe/Moscow. GitHub HEAD rechecked unchanged:
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Incremental local patch after
M6r; no commit/push. User explicitly selected **mandatory before mount,
fail closed on corruption/read/OOM/limits**, rather than an optional checker.
No on-disk version change or legacy migration.

## Policy and compatibility consequence

After normal dual-v2 checkpoint selection, walk ALL metadata reachable from
that selected root BEFORE setting mounted state. Direct backend reads bypass
the filesystem and verified-block caches. Failure refuses the whole mount,
clears tentative checkpoint history and prints:

```text
[aurafs] selected checkpoint metadata guard failed; mount refused, no older-root rollback or repair
```

No fallback to an older TREE when the selected super itself is valid. M6n's
existing selection among valid/invalid super copies remains unchanged. No
repair, checkpoint rewrite, format, FLUSH or other write is issued by the
checker. Legacy mount/sync remains unchanged. Existing v2 volumes are checked
without enabling the creation knob. There is no bypass switch in this slice.

**New v2 mount support bound:** the COMMITTED APPEND PREFIX must be at most
1 GiB (262144 × 4096 bytes), not merely the size of reachable metadata. A
larger device with a small prefix is allowed. An otherwise valid v2 volume
whose committed cursor exceeds this cap now refuses mount. There is no
migration/resize/GC tool or implicit bypass. Normal writes/sync do not add a
matching allocation cap: operators must keep workloads inside this tested
mount envelope; a successful sync beyond it does not imply supported remount.
Work-budget exhaustion can likewise reject a structurally valid large tree.
This availability tradeoff is the user's chosen fail-closed policy, NOT a
claim of corruption or a new supported full-capacity filesystem envelope.

## Algorithm and hard resource bounds

An iterative DFS holds at most nine metadata frames (root plus eight edges),
reusing the existing fixed frame format. Allocate one work object containing
those frames and one state byte per block below the committed cursor. Maximum
heap is about 293 KiB; no recursive C calls or 4 KiB stack buffers. Free on
every exit. OOM refuses mount.

Constants: prefix <= 262144 blocks, depth <= BTRFS_MAX_LEVELS (8 edges),
frame-loop steps <= 4194304. Both production constants remain hard ceilings;
the internal limited helper permits SMALLER test budgets, not bypassing caps.
Each metadata block is read at most once; repeated metadata references fail,
so direct metadata read volume is at most 1 GiB within the prefix ceiling.
The step bound can stop the walk earlier. Leaf overlap checking remains
quadratic in a fixed <=144 slot bound, not unbounded file/volume size.

For each visited metadata block:

- Aligned whole block within the selected checkpoint's committed prefix.
- Direct backend read, SHA-256, header magic, physical self and metadata type.
- Leaves: bounded item count, strict local key ordering, table/payload bounds,
  nonoverlap, known inode/dir/extent record shapes, via existing audit helper.
- Internal nodes: nonzero bounded pointer count and strict local key ordering.
  Every child is traversed under the same checks and depth budget.
- State distinguishes unseen, active metadata, completed metadata and extent
  data addresses. Cycles, metadata re-entry/duplicate child references and
  metadata/data role conflicts fail in EITHER traversal order.
- Extent record addresses must be aligned whole blocks below the cursor.
  Shared data addresses are not rejected. No file-data block is read here.

Tentative history still conservatively retains the superblock candidates,
but only the SELECTED tree is validated. An invalid unselected older tree
is not an implicit veto and may still make later union audit fail. No
retirement/reuse permission is derived from passing this walk.

## Not a full fsck or authentication boundary

This verifies reachable metadata encoding and graph structure, NOT global
namespace correctness. It does not establish internal separator/child range
consistency, all cross-leaf global key invariants, inode-link/directory
referential integrity, tree ancestry/owner/generation proofs, valid file data,
all extent ownership or malicious re-sealed image authenticity. It does not
inspect unreferenced/orphan blocks or every retained candidate tree.

File payload SHA corruption can still mount and then fail the ordinary
SHA-checked file read: a host test explicitly pins this scope. One serialized
mount is assumed; active-FD remount, background-writeback races, arbitrary
hardware faults and persistence beyond backend FLUSH honesty remain unproven.

## Host evidence added

- Previously passing M6r bad-child case now refuses before mount, despite
  intact selected root and older candidate. Child read failure also refuses.
- Re-sealed descendant wrong-self/type/layout faults fail.
- Duplicate metadata children with ordered keys, two-block cycle, extent
  address unaligned/outside prefix or aliasing metadata fail.
- Metadata/data aliases tested with metadata visited BEFORE and AFTER the
  leaf extent reference. Shared metadata is not silently skipped as a DAG.
- Synthetic internal chains pass through eight edges, fail at nine; a cycle
  fails without uncontrolled recursion or repeated I/O.
- Smaller helper prefix/step budgets fail, normal budgets pass. A sparse
  capacity declaration + valid resealed super tests the actual >1 GiB prefix
  mount refusal before large allocation/out-of-fixture I/O.
- Corrupt file data still mounts, but subsequent data read fails as scoped.
- Prior warm-cache root/refusal/OOM/unselected-older/valid leaf/node tests
  remain; every guard_refuses checks no writes, not-mounted and cleared history.

Negative control: temporary copied driver skips descendant reads while still
checking root/local pointer issues, reproducing root-only policy. The new
bad-descendant refusal assertion fails. No bypass is delivered.

## Real QEMU fixture

Extend the existing three-boot dual gate, retaining the creation, torn-super
fallback and real userspace FD workload in its first two boots. After boot 2,
Python traverses/checksums BOTH candidate metadata trees on disposable media,
finds a leaf reachable ONLY from the newest root (not either root block), and
zeros its first sector. Both superblocks, both roots and the entire older
metadata tree remain intact.

Boot 3 with both creation permissions OFF must report metadata-guard refusal,
not mount the older namespace, keep shell alive and not format/panic. Whole
faulted data-image SHA-256 before/after the boot must match. This is an offline
media-corruption fixture, not hardware power cutting or online fsck.

## Validation and reproduction

Completed on the final runtime/test source:

- Full `make test-unit -j4` PASS, including the changed descendant tests and
  M6p 418 + M6q 512 crash cases. No older receipt substituted for a rerun.
- Both actual-driver suites ASan/UBSan PASS (leaks off); M5 host PASS.
- Root-only negative control fails the new descendant refusal as intended.
- ISO PASS, Rust 1.98.0 + clang/lld, default creation opt-in still OFF.
- QEMU dual/descendant-corruption gate 18/18 PASS; legacy btrfs 21/21,
  userspace FD 12/12 and persistent data 9/9 PASS. QEMU 10.0.13 TCG/qemu64,
  4 vCPUs, 512 MiB, disposable raw AHCI media for the dual gate.
- Registry 249 cases all registered: existing dual case extended, not duplicated.
- Width sweep 448/449 casts, 69/69 x64 includes, cross-arch 0, 27/27 asm PASS;
  plan tables and diff whitespace PASS.
- HEAD+a..s sequential check/apply and delivered-file byte equality PASS.
- Optional TCC/libtcc1, -m32 host ABI and cross-ISA execution skips in the full
  suite remain explicit, not covered hardware/toolchain execution.

Production/test source-header dependencies already track the changed driver
and actual-driver host suite. No hardware power-cut claim.

```sh
export PATH="$HOME/.cargo/bin:/usr/sbin:$PATH"
make build/test_aurafs_m6 build/test_aurafs_power build/test_fsformat
build/test_aurafs_m6 && build/test_aurafs_power && build/test_fsformat
make test-unit -j4
bash tests/btrfs/test_m5_host.sh
make iso -j6
bash tests/integration/cases/test_aurafs_dual.sh
bash tests/btrfs/test_btrfs.sh
bash tests/integration/cases/test_aurafs_fd.sh
bash tests/integration/cases/test_data_volume.sh
python3 tools/check_test_registry.py
python3 tools/check_width_sweep.py --check
python3 tools/check_plan_tables.py
git diff --check
```

ASan/UBSan: build each actual-driver test with buffer_cache.c, blkdev.c,
sha256.c, `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
-ffunction-sections -fdata-sections -I . -Wl,--gc-sections`, then run with
`ASAN_OPTIONS=detect_leaks=0` (historical cache allocations retained).

No root retirement, committed-block reuse, repair/migration, garbage
collection, speed or EXACT durability claim. Durability WEAK, reclaim OFF,
M6 IN PROGRESS. This is stricter mount validation with explicit refusal bounds,
not closure of the crash/lifetime/retirement protocol.
