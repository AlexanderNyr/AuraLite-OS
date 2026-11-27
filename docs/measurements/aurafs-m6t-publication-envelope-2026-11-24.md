# M6t — align v2 allocation/publication with the mount envelope

Date: 2026-11-24, Europe/Moscow. GitHub HEAD rechecked unchanged:
`fbab5edba13498d6843cf443e1df8cf55b72e18c`. Local incremental patch after
M6s, no commit/push. Continues the user's mandatory fail-closed v2 metadata
policy. No on-disk version change, migration, automatic repair or reclaim.

## Gap closed, narrowly

M6s could accept live appends beyond the 1 GiB committed-prefix mount cap,
and could sync a tree exceeding the mount check's depth/work envelope. This
could knowingly publish a state outside the supported remount envelope.
M6t aligns v2 allocation with the byte ceiling and runs the SAME metadata
checker on a sync candidate BEFORE queuing its target checkpoint.

This is NOT a universal remount/durability guarantee: future read/OOM errors,
external corruption, dishonest devices or unsupported concurrency can still
make later mount fail. File payloads remain outside the metadata guard. The
change closes deterministic allocation/validation-policy mismatches under
the existing serialized-caller and honest-backend assumptions.

## Allocation ceiling

`aurafs_append_ceiling` returns min(device geometry, 1 GiB) for v2 and the
unchanged full geometry for legacy. Use it when setting max_block on format
and mount, and defensively inside blk_alloc even if an in-memory max_block
were widened. No block outside this v2 prefix can be allocated by the shared
append allocator. Existing subtraction-based ENOSPC/overflow checks remain.

- The final whole block ending exactly at the cap can be allocated.
- The next allocation returns zero without advancing the cursor or wrapping.
- Small devices retain their smaller physical cap.
- The superblock's total_bytes and physical capacity remain unchanged. No
  partitioning, shrinking, data relocation or format conversion is performed.
- Existing v2 volumes already beyond M6s's mount cap remain refused; this does
  not provide a recovery/conversion tool or enlarge the supported prefix.
- Legacy keeps its old geometry/append ceiling and weak barrier path.

This bounds raw append allocation, not user payload capacity: CoW metadata,
headers, retained history and orphan suffix consume the same limited prefix.
No free-space reuse or garbage collection is enabled at ENOSPC.

## Sync publication ordering

For v2 only:

1. Early refuse invalid node size, cursor alignment/minimum or cursor beyond
   min(actual total geometry, mount cap), before writeback/barrier requests.
2. Flush pending appends and require pre-publication device FLUSH as before.
3. Read active super into private shared scratch; form the candidate generation,
   root, cursor and next-inode fields, without queuing a target super yet.
4. Run the existing mandatory selected-tree checker on those candidate fields:
   prefix <=1 GiB, <=8 edges, <=4194304 frame steps, direct metadata reads,
   SHA/self/types/layout/role/bounds checks and bounded allocation. OOM/read/
   structural/budget failure returns sync error here.
5. Only on success, print `checkpoint candidate metadata verified before
   publication`, remember the candidate, seal/write the inactive slot, perform
   post-FLUSH/exact direct readback, then switch active slot/generation.

Failure at step 4 prints `checkpoint metadata guard failed; publication
refused, active checkpoint unchanged`. It does not enqueue a NEW target
checkpoint or advance in-memory generation/active slot. It is not a rollback
of live mutations or preceding I/O: appends may already be durable, allocator
cursor/live root remain advanced, and the old checkpoint still describes OLD.

Older failed-sync dirty checkpoints/cache eviction and device effects are
not comprehensively modelled here. A failed sync in general remains ambiguous;
this change does not turn every failure into proof that both stored supers
are unchanged. The new no-queue tests start from clean checkpoint-cache state
and isolate failure of this attempt's pre-publication validation. Serialized
callers and retained history remain required; no root retirement is inferred.

After correcting a transient/test fault, sync may be retried. No automatic
repair of a corrupt root or rollback of the live tree is supplied. Initial
format publication is unchanged except for the allocator ceiling; its own
failed-format recovery semantics from M6n/M6o still apply.

## Cost and scope

Every v2 sync now pays for a full selected reachable METADATA walk in addition
to the existing writeback/barriers/readback. Mount still checks independently.
Maximum temporary heap about 293 KiB; work/depth/prefix limits are unchanged.
I/O and CPU can be substantial. No speed/latency improvement is claimed or
measured. No file-data scan, namespace/global key-range/inode-link consistency
proof, authentication, concurrency or full fsck claim. Legacy sync does not
perform this new traversal.

## Host tests

New actual-driver test `test_publication_envelope` covers:

- Warm-cache candidate root SHA corruption, direct root read failure, guard
  allocation OOM and re-sealed wrong metadata type refuse sync.
- Too-deep candidate chain (beyond mount depth cap) also refuses publication.
- For each: no active/generation switch, no checkpoint write count increase,
  byte-identical stored supers, and an explicit checkpoint-range flush finds
  no newly queued dirty target. Appended writes/barriers are not claimed absent.
- Misaligned or beyond-device cursor refuses before any additional FLUSH.
- Restoring the candidate fixture permits successful retry and NEW remount.
- Sparse capacity declaration just above 1 GiB mounts existing low-addressed
  metadata with true total geometry but a capped max_block. Last allocation,
  ENOSPC, defensive widened-max_block case, and legacy ceiling helper covered.
- Over-cap sync cursor refuses with no checkpoint/FLUSH; exactly-at-cap cursor
  syncs/remounts with byte-exact NEW and allocator ENOSPC.

The sparse boundary fixture does NOT write/read 1 GiB of physical data. All
metadata/device requests are within the small RAM array; the high append
cursor describes an unreferenced suffix. This tests accounting/policy, not
throughput or full-capacity media operation. The depth fixture is explicit
host media editing, not a new production tree builder.

Negative control: temporary driver disables ONLY the pre-publication guard
invocation (mount verification stays enabled). New publication_refuses fails
because a bad candidate is accepted. No sabotage/bypass hook is delivered.

## Guest and full validation

The existing dual QEMU case gains an assertion for the new candidate-verified
receipt during its real FD/sync workload. Its offline newer-descendant fault
still requires boot-3 mount refusal, no older-tree rollback, live shell and
whole-data-image SHA equality.

Completed final-source validation:

- Full `make test-unit -j4` PASS; new publication-envelope receipt and existing
  M6p 418 + M6q 512 model receipts verified in the log.
- Both actual-driver ASan/UBSan suites PASS (leaks off), M5 host PASS.
- Guard-disabled negative control fails intended publication_refuses assertion.
- ISO PASS, Rust 1.98.0 + clang/lld, default creation opt-in OFF.
- QEMU dual 19/19 PASS including candidate metadata receipt and image-preserving
  descendant refusal; legacy btrfs 21/21, actual userspace FD 12/12 and
  persistent /data 9/9 PASS. QEMU 10.0.13 TCG/qemu64, 4 vCPUs, 512 MiB;
  three disposable raw 64 MiB AHCI disks for the dual gate.
- Registry 249 cases, all registered; width sweep 448/449 casts, 69/69 x64
  includes, cross-arch 0, asm 27/27 PASS; plan/diff checks PASS.
- HEAD+a..t sequential check/apply and delivered-file byte equality PASS.
- Optional TCC/libtcc1, -m32 host ABI and cross-ISA execution skips remain
  explicit, not executed coverage or real-hardware validation.

Reproduce from HEAD + patches a through t:

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

Both actual-driver suites additionally use ASan/UBSan with leaks off for
historical retained cache allocations. The M6p 418 + M6q 512 modeled cases
exercise this new pre-publication walk too; read-only metadata guard reads
do not add mutation/persistence events to that model's cut numbering. This
is bounded host evidence, not new hardware power-cut proof.

M6 IN PROGRESS, durability WEAK, reclaim OFF. Retirement/lifetime proof,
complete failure/concurrency handling and broader supported-volume envelope
remain open; this slice does not declare EXACT or enable committed-block reuse.
