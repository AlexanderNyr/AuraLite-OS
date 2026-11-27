# AuraLite OS — Filesystem Modernization Plan (`FSMODERN_PLAN.md`)

## Status: M0 ✅ DONE (the instrument and the first numbers) · M1 ✅ DONE (/data survives a power-off) · M2 ✅ DONE (4 KiB blocks: /fat read 7.4×) · M3 ✅ DONE (fsync means something: /fat write+fsync survives SIGKILL) · M4 ✅ DONE (ext2 mount check CLEAN + conformant multi-group format; fat32 fsync ordered) · M5 ✅ DONE (validated) · M6 🟨 IN PROGRESS (M6a–M6t, root audit/history + FD gate + barriers + opt-in dual checkpoints) · M7 ⬜

> **Baseline:** `bcc248f` + `patches/0001-HOME-H0-home-directory-and-the-user-database.patch`
> (HOME_PLAN H0, 2026-11-22). Prerequisite plan: `HOME_PLAN.md` (H4 depends on
> this one; M0–M3 are what unblock it).
>
> **Where this plan comes from.** The user asked, after `/home` landed, for a
> modernization plan for the filesystems — with one sentence that sets the
> bar: *"сделать свою файловую систему очень быструю и сделать её фишкой ОС"*
> (make our own filesystem very fast and make it the OS's flagship). That
> sentence is the north star of M6; everything before it is the groundwork
> that makes such a claim *measurable* rather than rhetorical.

---

## 0. The honest starting position (measured on the baseline)

This tree has **nine disk-backed filesystems and four in-memory ones**, all
written from scratch, and the breadth is genuinely unusual for a hobby
kernel. The problem this plan addresses is not "too few filesystems" — it is
that **not one of them has a number attached to it.**

| Measured fact | Evidence |
|---|---|
| 9 disk filesystems: fat32, ext2, ext4, f2fs, btrfs, exfat, ntfs, diskfs, usbfs | `kernel/fs/*.c`, 20 012 lines total |
| The buffer cache is **512 B × 1024 = 512 KiB**, one block size, no readahead | `kernel/fs/buffer_cache.h`: `BC_BLOCK_SIZE 512`, `BC_MAX_BUFFERS 1024` |
| Writeback exists (`deferred`/`flushed`/`coalesced` counters) but no **latency** measurement anywhere | `bc_writeback_receipt()`, and `grep -rn ticks kernel/fs/buffer_cache.c` → nothing |
| `SYS_FSYNC` (74) had **no case arm** at all until recently | the arm's own comment in `syscall.c` says so |
| ext2 has no journal (`grep -c journal` → **0**) — the FSFULL plan fixed correctness, not crash-safety | `kernel/fs/ext2.c` |
| Every experimental FS needs its **own dedicated disk**, so the default QEMU run mounts fat32 + diskfs + tmpfs and nothing else | `kernel/kernel.c` mount ladder (`blkdev_count() > 3`, `> 4`, `> 5` …) |
| btrfs, f2fs, ext4 are **not binary-interoperable** with host tools; their headers say so | `btrfs.c` header: "NOT binary-interoperable with host mkfs.btrfs" |
| No filesystem benchmark exists; `test_perf_smoke.sh` measures boot-to-shell, not I/O | `tests/unit/`, `tests/integration/cases/` |

**The consequence, stated plainly:** "быстрая" is currently an *unfalsifiable*
claim. Any number can be asserted and nothing can contradict it. M0 is
therefore not a warm-up — it is the precondition for the rest of the plan
existing at all. A flagship that cannot be measured is a slogan.

---

## 1. The four problems worth solving, in order of how much they cost

### Problem 1 — Nothing is measured (blocks everything else)

A filesystem claim has exactly four numbers: **throughput** (MiB/s),
**latency** (µs/op), **amplification** (bytes written per user byte), and
**mount time** (ms). None is collected today, so no change to any filesystem
can be called an improvement or a regression.

### Problem 2 — The block layer is 1990s-shaped

`512 B` blocks with `1024` buffers is **512 KiB** of cache in a machine with
512 MiB of RAM — a 0.1 % cache. Every operation that misses issues one
512-byte DMA. `blkdev_ops.read` supports multi-sector transfers, so the *seam*
is already modern; the cache above it is not. There is no readahead, no
write-coalescing across buffers beyond the adjacent-sector case, and no
notion of a "large block".

### Problem 3 — Durability is per-filesystem folklore

btrfs has CoW + SHA-256 trailers and an `fsync` that persists a generation.
f2fs has checkpoints. ext2 has neither — a crash mid-write leaves it
inconsistent and there is no fsck that could say so. `SYS_SYNC` flushes the
whole cache and `SYS_FSYNC` exists, but there is no single contract that says
what a filesystem must guarantee on `fsync` vs on power loss. HOME_PLAN H4
("a note survives a reboot") cannot be built on folklore.

### Problem 4 — Nine filesystems, one of which is the actual product

Breadth without a flagship is a museum. The default boot mounts fat32 (which
interops with the outside world) and diskfs (365 lines, its own private
store). Meanwhile `/home` — the thing a user cares about — is tmpfs, and
`/opt` is tmpfs. **The tree's best filesystem (btrfs's CoW+checksums) is on a
disk nobody boots with, and the tree's most important directory (`/home`) is
in RAM.** That inversion is what M6 fixes.

---

## 2. Decisions

### D1. Measure first, then optimize — and publish the number even when it embarrasses

M0 lands a benchmark that runs **in-guest** and prints a stable machine-readable
receipt, plus a host test that parses it and pins a floor. A number that only
appears when someone remembers to look is not a measurement.

### D2. The flagship is **AuraFS**: a log-structured, checksummed, CoW filesystem built by *evolving* btrfs.c

Not a tenth filesystem written from scratch. btrfs.c already has the two
hardest pieces right — every write is copy-on-write, and every block carries
a SHA-256 trailer verified on read. What it lacks is what a log-structured
design gives for free: **sequential writes**, which is exactly what makes a
flash/tmpfs-hybrid device fast.

M6 is therefore a *promotion and an acceleration*, not a rewrite:

- btrfs.c's tree + trailer stay as the durability substrate.
- A **log-structured allocation policy** replaces the free-block search:
  writes append to a cursor, so the disk sees sequential LBAs (and so does
  the buffer cache, which can then coalesce with zero effort).
- The result is the same on-disk checksum guarantee, with the write path of a
  log filesystem — which is the honest meaning of "very fast" on the hardware
  this tree runs on (QEMU virtio/ahci, later real SATA).

**Naming decision:** the filesystem keeps the file name `btrfs.c` (it is
referenced by parity claims and the self-host source pin) and gains a
user-facing name in its mount receipt. Renaming 1 342 lines of driver to
satisfy a brand is churn; the brand goes in the receipt.

### D3. One block layer, and it must be *bigger*

The 512 KiB cache becomes a **multi-block, size-classed cache** with:

- 4 KiB cache blocks (the natural page size; the block layer already handles
  512 B sectors, so a 4 KiB cache block is 8 sectors — one `blkdev_read`
  call, which the seam already supports);
- readahead on sequential access (the pattern every file read has);
- write-coalescing that merges adjacent dirty cache blocks into one
  multi-sector write.

This is M2 and it benefits **every** filesystem at once, which is why it
comes before the flagship.

### D4. Crash-safety becomes a contract, not a per-driver habit

M3 defines one interface — `fs_sync(vnode, mode)` with `SYNC_DATA` vs
`SYNC_META` — and a **test that kills the VM mid-write** and checks what
survives. A filesystem that cannot meet the contract may declare itself
`FS_DURABILITY_WEAK` in its ops table, and the mount receipt says so. Honest
degradation, named.

### D5. Interop stays exactly as it is

FSFULL's D7 ("the format is the contract") is not renegotiated here. fat32
must keep mounting a host-made image; ext4 must keep refusing a foreign one
by name. The flagship is *additional* — it does not replace the interop
filesystems, and a user who needs to move a file to a real machine still uses
`/fat`.

### D6. `/home` and `/opt` move onto whatever wins, and the receipt stops apologizing

This is HOME_PLAN H4. Its dependency is M1 (a writable disk filesystem the
boot actually mounts) + M3 (a durability contract). When H4 lands, the boot
receipt changes from `storage=tmpfs (volatile: …)` to `storage=aurafS` — and
that one string change is the whole reason the filesystem work matters to a
user.

---

## 3. Phases

### M0 — The benchmark and the baseline numbers ✅ DONE

**Nothing else in this plan can be judged without M0.**

Deliverables:

1. `tests/unit/test_fsbench.c` — a host-compiled benchmark harness that
   drives the **shipped** VFS against a RAM-backed `blkdev_ops` (no QEMU, so
   it runs in `make test-unit` in seconds). Measures: sequential create,
   sequential read, random 4 KiB read, small-file latency, `fsync` latency.
2. `tests/integration/cases/test_fsbench.sh` — the same workload in-guest on
   `/fat`, `/disk` and (when provisioned) `/btrfs`, printing one receipt line
   per filesystem:
   `[fsbench] fat32 seq_write=… MiB/s seq_read=… random_read=… ops/s fsync=… µs`
3. The numbers land in `docs/filesystems_perf.md` — a **table with a date**,
   regenerated by the gate, not written by hand.

**Exit gate:** the receipt exists for at least fat32 and diskfs, the host
harness runs in `make test-unit`, and every later phase links to the row it
moved.

**What M0 will probably discover (predicted, so the plan can be wrong in
public):** diskfs, at 365 lines with a linear directory, will beat fat32 badly
on small files and lose on throughput; and every filesystem will show a
`fsync` latency dominated by whole-cache flushes, because `SYS_SYNC` is what
`fsync` currently falls back to.

### M1 — A filesystem on the boot disk, by default ✅ DONE

Today the default boot mounts fat32 + diskfs and the flagship candidates
(`/btrfs`, `/ext4`, `/f2fs`) need 4th/5th/6th disks that nobody attaches. The
tree's best work is unreachable in the default run.

- Provision one extra QEMU disk in the standard boot recipe and mount the
  CoW filesystem there as `/data`.
- `make run` and the integration harness both use the same recipe, so the
  default tree has a **persistent** filesystem mounted — the precondition for
  HOME_PLAN H4.

**Exit gate:** `[boot] /data mounted (btrfs-style, CoW+SHA-256)` in the default
boot log; `test_data_volume.sh` writes a file, reboots, and reads it back.
**That reboot test is the first durability proof in the tree.**

### M2 — The block layer: 4 KiB blocks, readahead, write-coalescing ✅ DONE

Bounded by D3. The work is inside `buffer_cache.c` and must not change any
filesystem's on-disk format (the cache block size is internal; filesystems
address 512-byte sectors through the existing API).

Acceptance, all measured by M0's harness:
- sequential read throughput at least **2×** the M0 baseline on `/fat`
  (readahead + multi-sector DMA);
- write coalescing visible in `bc_writeback_receipt` (`coalesced` > 0 for a
  sequential 1 MiB write, from a standing start of ~0);
- cache footprint stated in the boot receipt (`[bc] 4 KiB × N blocks = X MiB`,
  and N derived from RAM size rather than a hard 1024).

### M3 — The durability contract ✅ DONE

- `fs_sync(vnode, mode)` added to `vfs_ops`; each filesystem either implements
  it honestly or declares `FS_DURABILITY_WEAK`.
- **The crash test:** `test_fs_crash.sh` starts a write loop, sends QEMU a
  power-off at a random point, reboots, and asserts (a) the file is either
  fully there or absent, never half-made, and (b) the filesystem mounts
  without repair on the next boot.
- ext2 is allowed to fail (b) and to say so — that is the point of the
  declaration. fat32 must pass (a) for a single-sector write, which its
  FAT-update ordering already implies.

**Exit gate (met 2026-11-22, CHANGELOG `[FSMODERN M3]`):**
`tests/integration/cases/test_fs_crash.sh` kills a random 0–1200 ms after an
in-guest write+fsync and reboots twice — 15/15. Measured: a `/fat` write that
returned success was LOST when the machine died ≤ 0.5 s later (the 1 Hz
writeback was the only thing keeping it alive); with `fsync()` the file came
back **whole and byte-correct after an immediate SIGKILL**. The classes are
declared in each driver's ops table: `/fat`,`/disk` **EXACT**, `/data`,`/ext2`
**WEAK** (both print it at mount), the rest **VOLATILE**.

### M4 — ext2 and fat32 get their missing pieces ✅ DONE

The two filesystems a real user actually touches, brought to the same bar as
the fancy ones:

- ext2: a **bitmap-consistency checker** at mount (`fscheck.c` already exists
  and already detected an inode/bitmap disagreement in the unit run — promote
  it from a test fixture to a boot-time check with a receipt), and a
  documented answer to "what happens on a crash" (M3's declaration).
- fat32: LFN write support gaps if any remain, plus `fsync` reaching the FAT
  sectors before the data sectors (ordering, not speed).

**Exit gate (met 2026-11-24, CHANGELOG `[FSMODERN M4]`):** promoting the
checker to a boot-time check is what found the real bug: the formatter wrote
`blocks_per_group = total_blocks`, so any volume larger than one bitmap block
(8 MiB at 1 KiB) got ONE group whose bitmap could reach 8192 blocks while the
superblock claimed 65536 and the free counters claimed 65468 free of 8171
reachable — and the allocator read past its bitmap buffer. Measured with ASan
on the shipped walker: `heap-buffer-overflow READ of size 1, 0 bytes after
1024-byte region`. The formatter now lays out `min(8*block_size, volume)`
blocks per group with backup superblock+GDT copies in each; a fresh 64 MiB
volume gives `[ext2] formatted: 65536 blocks of 1024 B in 8 group(s) of 8192
blocks, 1024 inodes, 65374 free blocks` and `fsck.ext2 -fn` finds **nothing to
fix**. The mount receipt is `[ext2] mount check: CLEAN (bitmaps + inode table
agree)`. fat32 got `.fsync` (data → all FAT copies → dirent) and its rename
order reversed: the destination is created before the source is removed, so a
power cut leaves the file reachable under two names instead of under none.

### M5 — Directory scaling ✅ DONE (validated 2026-11-24)

Every filesystem here has a linear directory scan. At the sizes a user reaches
(`/home` with a few hundred files, `/opt` with a package tree), that is fine;
at 10 000 files it is quadratic. M5 adds hashed directories **to the flagship
only** (M6's substrate), because that is where the claim will be made — and
measures the crossover point rather than assuming one.

**Exit evidence (2026-11-24, local continuation of `2ecdb6e`):**
`test_dir_scale.sh`: 9/9 assertions, 64/256/1024 names, 2747.9/2948.4/2484.8
stat operations/s under QEMU TCG (2 vCPUs, 512 MiB, 256 MiB AHCI data disk).
16x the names costs 1.11x lookup time. These are metadata-only, empty-file
lookups, not 512-byte payload throughput (the historical `--size 512` receipt
is not evidence of writing payload in scale mode).

The test exposed a real 16-bit hash collision at `f_594.dat`: insertion had
silently replaced a different name. Directory keys now retain the legacy
16-bit hash and use the upper bits for collision-chain slots; tombstones
preserve reachability when a chain head is deleted. Host regression covers
an actual collision and head deletion, 2500-key depth-3 root growth, CoW,
and byte-exact lookup. Chain length is bounded at 4096; tombstones are not
yet reclaimed. Existing non-colliding keys keep their original offsets.

Regression gates: `tests/btrfs/test_m5_host.sh` PASS;
`tests/btrfs/test_btrfs.sh` 19/19; `test_data_volume.sh` 9/9.
Benchmark harness repairs: parse `--scale`, allow up to 4096 names in that
mode, use the calibrated microsecond clock, avoid a per-name stack array,
initialize integration helpers, attach disk 2, explicitly allow formatting
of the fresh test images. Details and limits: `docs/filesystems_perf.md`.

### M6 — AuraFS: the flagship 🟨 IN PROGRESS

The user's sentence: *"сделать свою файловую систему очень быструю и сделать
её фишкой ОС."*

**M6a receipt (2026-11-24, based on upstream `fbab5ed`):** existing M5
allocation already appends to a cursor; there is no free-block search to
replace. The first slice therefore validates the append cursor/superblock
and orders checkpoint writeback with error propagation. Host tests link the
actual cache and blkdev, inject device failures, drop cache and remount, and
cover bounds, corrupt geometry, pinned buffers and retry. M5 host regression
and ASan/UBSan pass. See the M6a CHANGELOG entry for exact commands and limits.
No new QEMU or speed measurements were made in this slice. The existing
single-superblock format and WEAK durability are unchanged; reclaim remains
unimplemented. The volatile snapshot primitive is supplied by M6b below. Do not close M6 or publish a speed
claim until the remaining gates below are measured.

**M6b receipt (2026-11-24):** `aurafs_snapshot_create/read/release` supplies
an in-memory read-only snapshot primitive. Root-parameterized path, inode and
extent reads preserve the old namespace/content after overwrite, truncate,
rename, unlink and path recreation. Host tests cover two 9000-byte versions,
checksum rejection, count overflow, release, and successful/failed remount
expiry; M6a/cache/M5 regressions and ASan/UBSan pass. No data copies or block
allocations are made at capture. Not persistent, not a mounted subvolume,
not a syscall. Kernel callers must serialize with existing FS operations;
future reclamation must first register/pin all snapshot roots. No new QEMU
or speed measurement: M6 is still IN PROGRESS.

**M6c receipt (2026-11-24):** toolchain restored; ISO and full unit target
pass (optional host/toolchain skips documented in CHANGELOG). New registered
QEMU benchmark passes 6/6, btrfs 19/19, `/data` power-off/remount 9/9.
Generated measurement artifact: `docs/measurements/aurafs-m6c-2026-11-24.md`.
Latest medians 0.07 MiB/s write / 0.29 MiB/s read; preliminary median write
was 0.03 MiB/s. These noisy runs are NOT evidence for a speedup against the
historical baseline. Writer now rejects ENOSPC before writing block zero;
host fault test and ASan/UBSan pass. Next: profile write/metadata costs;
reclamation still requires pinning all snapshot roots. M6 is NOT closed.

**M6d receipt (2026-11-24):** redundant metadata SHA recomputation is now
memoized ONLY when all 4096 freshly fetched bytes exactly match a previously
verified copy. Data/superblocks always hash. 16 slots (~64 KiB + tags), no
format change or reuse; serialized-caller contract unchanged. Final same-host
A/B on a corrected clock: reads 5456 in both modes, read SHA 5456 -> 1931;
median read 0.4880 -> 0.8161 MiB/s (1.67x in this pair). Unlink regressed
522.8 -> 293.1 ops/s: no universal speedup is claimed. ISO ON/OFF, full unit
target, host corruption/snapshot/ENOSPC tests, ON sanitizers, QEMU btrfs 19/19
and `/data` power-off 9/9 pass with documented optional skips. See
`docs/measurements/aurafs-m6d-ab-2026-11-24.md`. Next: write amplification and
reclaim design with snapshot pinning. **M6 remains IN PROGRESS.**

**M6e receipt (2026-11-24):** bounded same-leaf metadata batches reduce
write seal attempts 1663 -> 1049 (36.9%) on the same guest workload; other
cases use the existing split-capable inserter. Writes publish private roots
only on final success; orphan allocations on error are not reclaimed.
ON/OFF host tests, ON sanitizers, multi-group 141137-byte write/snapshot,
forced cross-leaf fallback, metadata-ENOSPC/retry, full unit target, both ISO
variants and QEMU btrfs/persistence gates pass (optional skips documented).
Observed median write 0.0259 -> 0.0704 MiB/s (2.72x in one noisy pair).
See `docs/measurements/aurafs-m6e-ab-2026-11-24.md`. Next: the 256-byte syscall
bounce chunks that still cause 16 driver writes per 4096-byte user write;
reclaim still needs registered snapshot roots. **M6 is not closed.**

**M6f receipt (2026-11-24):** ordinary-file SYS_WRITE uses a bounded 4096-byte
heap bounce buffer, retaining 256-byte stack/OOM fallback. Actual helper host
tests prove 16 -> 1 VFS calls per 4096-byte payload; pipe/device/console paths
retain their transfer policy. Later usercopy faults report prior regular-file
progress; pwrite/read/writev remain unchanged. No new FD lifetime protection.
Same-host 256/4096 A/B (batch ON/cache16): seal attempts 1049 -> 239 (77.2%
fewer), median write 0.1528 -> 0.7072 MiB/s; unlink regressed 319.5 -> 171.8
ops/s. No universal speed claim. Host fault/OOM/cleanup tests, sanitizers,
full unit target, both ISO/bench gates, final guest uaccess/pipes/btrfs/data
persistence gates PASS (existing optional skips). See
`docs/measurements/aurafs-m6f-ab-2026-11-24.md`. Next: safe reclaim with
registered snapshot pinning and /home integration. **M6 remains open.**

**M6g receipt (2026-11-24):** volatile snapshots now occupy a bounded
32-entry root registry with non-reused registration IDs. Exact active-token
validation, alias-wide release invalidation, create-over-active rejection,
capacity failure and stale-slot protection are tested. All-or-error root
enumeration supplies one prerequisite for GC, not a complete root set:
live/checkpoint roots and complete reachability still need protection.
Host actual-driver saturation/expiry/no-I/O tests, sanitizers, M5, full unit,
ISO, QEMU btrfs 19/19 and data power-off/remount 9/9 PASS (optional toolchain
skips unchanged). No benchmark; allocator still append-only/reclaim=none.
See `docs/measurements/aurafs-m6g-snapshot-registry-2026-11-24.md` for alias
semantics and fail-closed GC requirements. **M6 remains IN PROGRESS.**

**M6h receipt (2026-11-24):** bounded read-only iterative reachability
diagnostic covers live/cache-visible-checkpoint/registered-snapshot roots,
with union metadata/data counts, per-root horizons and corruption/cycle/
resource-limit rejection. Snapshot-only test tree contributes exactly three
blocks until release. This is NOT a reusable-block map, full fsck or proof
of the physical device checkpoint; reads can cause ordinary cache writeback.
Actual-driver host/sanitizer tests, full unit, M5, ISO, QEMU btrfs 19/19 and
data power-off/remount 9/9 PASS (optional toolchain skips unchanged). No
benchmark or allocator change. See
`docs/measurements/aurafs-m6h-reachability-2026-11-24.md`. Before reclaim:
prove durable checkpoint-root retention/ordering, complete root lifetimes
including open files, and fault/remount-safe reuse. **M6 remains open.**

**M6i receipt (2026-11-24):** since-mount history retains up to 32 distinct
mounted/submitted checkpoint root/end pairs, including failed flush attempts;
a successful sync does not discard older candidates. Audit includes these
history-only roots and fails closed on sticky overflow, while writes/sync
continue normally. Fault test proves physical-old/cache-new super divergence
and catches corruption in old data reachable only through history. Host,
sanitizers, full unit, M5, ISO, QEMU btrfs 19/19 and data persistence 9/9 PASS
(optional toolchain skips unchanged). No format/reclaim change; no durable
root or torn-super recovery proof. See
`docs/measurements/aurafs-m6i-checkpoint-roots-2026-11-24.md`. Next: checkpoint
publication/retirement safety and complete root lifetimes including open
files. **M6 remains IN PROGRESS.**

**M6j receipt (2026-11-24):** fix referenced vnode identity loss on
unlink/recreate: intern by inode, keep pinned slots intact on name invalidation,
and refuse free-slot reuse with nonzero vfs_refs. Hard links share a vnode.
Driver-level old/new payload/nlink/rename/link tests, cache-pressure/all-pinned/
final-reference reuse tests and pre-fix negative control pass. The driver
helper also runs in QEMU (btrfs gate now 20/20); host/sanitizer/full unit/M5/
ISO/data persistence 9/9 pass with optional toolchain skips unchanged. Test
pins model OFD refs, not an end-to-end concurrent VFS lifecycle proof. See
`docs/measurements/aurafs-m6j-open-vnodes-2026-11-24.md`. No reclaim/format
change; checkpoint publication/retirement and full FD root lifetimes remain
unresolved. **M6 remains IN PROGRESS.**

**M6k receipt (2026-11-24):** actual userspace `/tests/aurafsfd` exercises
open/dup/fork/wait/close, shared OFD offset, open-unlinked old/new payloads,
nlink, 96 real open-close pressure cycles, rename/hardlinks and fsync. New
registered fs-group QEMU gate 12/12 includes fresh-boot completion-marker
verification with format off. Pre-M6j driver negative control rejects the
same test; fixed driver/ISO restored. Host/sanitizer/full unit/M5/ISO, QEMU
btrfs 20/20 and data persistence 9/9 PASS (existing optional skips). No runtime
kernel changes, race/GC/durability claims. See
`docs/measurements/aurafs-m6k-userspace-fd-2026-11-24.md`. Next: checkpoint
publication/retirement and complete concurrency/root lifetime proof before
reuse. **M6 remains IN PROGRESS.**

**M6l receipt (2026-11-24):** sync directly reads back the superblock via
the backend seam after writeback and requires exact 4096-byte equality before
generation advancement/success. Warm-cache false success on dropped/partial/
valid-but-different writes and backend read errors is rejected; retained
history and retry covered. Pre-readback negative control fails as required.
Host/sanitizer/full unit/M5/ISO, QEMU btrfs 21/21, userspace FD/remount 12/12
and data persistence 9/9 PASS (optional toolchain skips unchanged). Adds a
4 KiB backend read, not a speed optimization. See
`docs/measurements/aurafs-m6l-checkpoint-readback-2026-11-24.md`. No device
FLUSH/atomicity/durable-root proof, appended-data verification or retirement;
reclaim remains off pending barrier/publication/recovery protocol.
**M6 remains IN PROGRESS.**

**M6m receipt (2026-11-24):** optional explicit blkdev FLUSH with error/
unsupported semantics; AHCI uses advertised IDENTIFY capability and locked
non-data FLUSH CACHE[ EXT]. Sync now orders data writeback/barrier, super
writeback/barrier, exact backend readback, then generation advancement.
Unsupported backends stay explicitly weak; supported failures propagate.
Host seam/selector/order/failure/retry tests, negative control, sanitizers,
full unit/M5/ISO, QEMU btrfs 21/21 with device-FLUSH receipt, userspace FD
12/12 and data persistence 9/9 PASS (optional toolchain skips unchanged).
See `docs/measurements/aurafs-m6m-device-flush-2026-11-24.md`. No benchmark,
super-format/recovery or history retirement/reclaim change: single in-place
super can tear; durability stays WEAK. Next: robust super publication and
recovery selection before retiring roots/reusing blocks. **M6 remains open.**

**M6n receipt (2026-11-24):** user-selected opt-in empty-volume dual-v2
format; no legacy migration. Independent default-OFF knob plus generic
format gate, full zero-device proof and required backend barriers. Two sealed
self-addressed slots, newest-valid selection, ambiguity refusal, inactive-slot
sync with barriers/readback. No automatic repair; both valid roots retained.
Host cut sweep/cache-drop, negative selector and QEMU fresh-boot torn-image
recovery 12/12 PASS, including real FD workload after fallback. Full unit/M5,
ISO, sanitizer and legacy btrfs/FD gates PASS; data gate recorded in changelog.
See `docs/measurements/aurafs-m6n-dual-checkpoint-2026-11-24.md`.
This does not prove hardware crash durability or arbitrary tree-corruption
recovery. Failed sync may recover a fully written NEW copy; legacy remains
single-super. No root retirement/reuse; WEAK declaration and serialized-caller
contract remain. Next: broader recovery/lifetime bounds and safe retirement
protocol, not immediate reclamation. **M6 remains IN PROGRESS.**

**M6o receipt (2026-11-24):** 17 initial dual-format fault cases, actual
RAM-backed driver/cache/seam: reject each write, cut either slot at four
prefixes, fail each barrier/readback. Failed init never exposes a mounted
filesystem. Cache-drop remount is read-only, recovers surviving copies and
supports write/sync/remount/read; no-copy state refuses. Explicit format retry
preserves nonzero remnants; only all-zero media can be re-provisioned. Negative
zero-scan control, focused host/M5/sanitizer tests PASS. No runtime change or
new guest proof; see `docs/measurements/aurafs-m6o-format-interruptions-2026-11-24.md`.
A failed format can still leave valid copies, including after bounded cache
write retries. No rollback/EXACT/reclaim claim. **M6 remains open.**

**M6p receipt (2026-11-24):** actual-driver volatile device-cache host model,
418 power-loss/failing-FLUSH cases. Read-visible writes are not stable until
FLUSH; sector-by-sector persistence tested in both LBA orders, initial format
and sync to either slot. Success must recover byte-exact NEW; failed sync may
recover OLD/NEW as bounded by persisted barriers. Selection is read-only;
surviving trees accept further writes and survive another modeled power loss.
Pre/post-barrier negative controls expose unreadable NEW / lost acknowledged
NEW respectively. New UNIT_TESTS target, focused host/M5/sanitizer PASS; no new
guest/full-suite execution claim. See
`docs/measurements/aurafs-m6p-volatile-cache-2026-11-24.md`.
Model assumes honest successful FLUSH and atomic sectors; no hardware power-cut,
complete lifetime/concurrency or retirement proof. No runtime change, migration,
repair or reuse. Durability WEAK; **M6 remains open** pending broader recovery
and root-retirement protocol, not immediate reclamation.

**M6q receipt (2026-11-24):** add 512 second-crash cases to M6p's 418.
Four reachable torn-publication seeds retain one valid checkpoint plus orphan
appends. Recovery does no I/O writes/FLUSH, resumes the old cursor, retries
with a different payload and suffers another cut/failed barrier. Both survivor
slots and first/second persistence orders tested; sole survivor unchanged,
byte-exact OLD/NEW, never ABORT, continued ALIVE + original-file checks PASS.
Partial failed replay may complete earlier torn bytes and recover NEW; no
failure-means-rollback assumption. Focused host/sanitizer/M5 and negative
control PASS; no production change or new guest/full-unit proof. See
`docs/measurements/aurafs-m6q-repeated-crash-2026-11-24.md`. This covers two
bounded publication failures, not arbitrary fault sequences, live-FD remount
or retirement. WEAK, no reclaim/migration/repair; **M6 remains open.**

**M6r receipt (2026-11-24):** bounded fail-closed selected-root guard for
v2 mount. Direct backend 4 KiB read, SHA/self/type and local leaf/internal
layout/child-pointer bounds; OOM/read/validation error refuses before mounted
state and clears tentative history. No older-tree rollback or write/repair
when valid selected super references a bad root. Invalid-super fallback and
legacy behaviour unchanged; no format migration. Host/sanitizer + 930 crash
cases, M5, full unit, ISO and QEMU dual 18/18 + legacy btrfs 21/21,
FD 12/12 + persistence 9/9 PASS. Entire refused
data image unchanged; live shell. See
`docs/measurements/aurafs-m6r-selected-root-2026-11-24.md` for full regression
receipts. Not recursive fsck: descendant corruption may still mount/fail later;
no authentication, concurrency/lifetime or retirement proof. WEAK, reclaim OFF,
**M6 remains open**. Next requires explicit broader tree recovery policy,
not silently enabling rollback or block reuse.

**M6s receipt (2026-11-24):** user chose mandatory fail-closed metadata
walk before v2 mount. Direct backend DFS validates all selected reachable
metadata and extent addresses/roles; cycles/duplicate metadata/OOM/read/limit
failure refuse, no older-tree fallback or writes. Prefix <=1 GiB, depth <=8
edges, <=4194304 steps, about 293 KiB maximum heap. This is an explicit
availability bound: larger committed prefix refuses even if valid; no new
write-allocation cap or bypass. Not file-data verification or global namespace
fsck. Host structural/budget/alias tests and negative root-only control PASS;
ISO and QEMU dual 18/18 PASS with an exclusive new descendant corrupt while
both roots/supers and older tree stay intact; entire data image unchanged.
Full unit, both ASan/UBSan suites including 930 crash cases, M5 and legacy
QEMU btrfs 21/21 + FD 12/12 + persistence 9/9 PASS. Receipts and limits:
`docs/measurements/aurafs-m6s-mandatory-metadata-2026-11-24.md`.
No legacy migration, repair or root retirement/reuse; WEAK, reclaim OFF,
**M6 remains open**. Broader lifetime/retirement protocol still required.

**M6t receipt (2026-11-24):** align writable v2 prefix with M6s's 1 GiB
mount ceiling (small devices remain smaller, full geometry retained). Same
mandatory metadata/depth/work guard now runs after append pre-FLUSH and
BEFORE queuing a sync target. Candidate guard/cursor failures do not queue a
new super or advance active/generation; live appends are not rolled back.
Legacy ceiling/sync unchanged. Host no-queue/boundary/retry and negative
control PASS; full unit, both sanitizers + 930 modeled crashes, M5, ISO,
QEMU dual 19/19 + legacy btrfs 21/21 + FD 12/12 + persistence 9/9 PASS.
Full receipts:
`docs/measurements/aurafs-m6t-publication-envelope-2026-11-24.md`.
Extra metadata walk on every v2 sync, no speed claim. Not a guarantee against
later OOM/read/device faults or corrupt file payloads. Existing over-cap v2
still refuses; no conversion/repair/retirement/reuse. WEAK, reclaim OFF,
**M6 remains open**; lifecycle/publication/failure proof is still incomplete.

Built by evolving btrfs.c per D2:

1. **Log-structured allocation** — retain and validate M5's append cursor;
   every write appends. Measured by M0's harness against the M1 baseline.
2. **Hashed directory** (M5) for `/home`-shaped directories.
3. **The flagship receipt**: on mount, the filesystem prints what makes it
   what it is, in one line a user can read:

```
[aurafS] mounted at /home: log-structured, CoW, SHA-256 per block,
         hashed dirs, seq_write=NNN MiB/s (M0 baseline: NNN)
```

4. **The claim is bounded and true**: AuraFS is fast *for the patterns this
   OS produces* (many small files, sequential appends, a user's home), and
   that claim is backed by M0's table with dates. It is **not** claimed to be
   faster than ext4 on a real SSD at large sequential I/O — that would be a
   lie the benchmark would immediately expose.

### M7 — Where the number goes on the website/README ⬜

The plan's final phase is not code: it is the sentence a stranger reads first.
Today the README says what the OS *has*; after M6 it can say what the OS
*measures*. This phase writes that paragraph and links `docs/filesystems_perf.md`.

---

## 4. Dependency graph

```
M0 (measure)
 ├─> M2 (block layer)  ──┐
 ├─> M3 (durability)   ──┤
 └─> M1 (persistent /data)
        │                │
        └──> HOME_PLAN H4 ┘   (/home becomes durable)
                 │
                 └─> M4 (ext2/fat32 completeness)  ... independent lane
                          │
                          └─> M5 (hashed dirs) ─> M6 (AuraFS) ─> M7 (the publication)
```

M0 gates everything. M1 is what makes the plan matter to a user tomorrow.
M2 and M3 are parallel. M4 runs alongside and can be dropped without
affecting M6. **HOME_PLAN H4 must not start before M1 + M3 are done** — a
durable `/home` on a filesystem without a durability contract is the same
folklore problem in a new place.

---

## 5. Risks and the ones this plan will not take

| Risk | Answer |
|---|---|
| "A tenth filesystem is scope creep" | It isn't a tenth: M6 evolves btrfs.c. If M0's numbers say btrfs.c's tree is the bottleneck rather than its allocation policy, M6 is re-scoped *by data*, not by opinion. |
| "512 KiB cache is fine at this scale" | It may be. M0 will say. If the benchmark shows the cache is not the bottleneck, M2 drops to the end of the queue. |
| "Crash tests are flaky in CI" | The crash test kills QEMU at a controlled point and asserts an invariant that holds for both outcomes (present-and-complete, or absent). It asserts a *property*, not a timing. |
| Data loss during development | Every phase mounts its candidate as a **new** path (`/data`, `/aurafS`), never over `/fat`, `/disk` or a user's existing files. |
| The flagship becomes unmaintainable | The hard parts (CoW, checksums) are already written and tested (`test_btrfs`, 10 assertions + a kernel self-test). M6 changes the allocator and the directory index, not the integrity geometry. |

---

## 6. What this plan deliberately does NOT do

- **No network filesystem** (NFS/SMB). That is INTERNET_PLAN's ground and its
  own series.
- **No encryption / per-file keys** — HARDENING_PLAN owns that, and a
  checksummed CoW store is the right substrate for it *later*.
- **No snapshots UI.** AuraFS's CoW makes snapshots cheap, and M6 will expose
  the primitive (`fs_snapshot()`); the user-facing story is a later plan, on
  purpose — a half-built snapshot UI that silently loses data is worse than
  none.
- **No claim about hardware we do not have.** The benchmark runs under QEMU's
  emulated devices; the plan's numbers are labelled with that hardware and
  never presented as SSD numbers.
- **No re-litigating FSFULL.** That plan closed the *correctness* of the five
  experimental filesystems. This one is about speed, durability contracts and
  one flagship.

---

## 7. What "done" looks like

1. `docs/filesystems_perf.md` exists, carries dates, and is **generated** by
   the gate — a number that is not in the table did not happen.
2. `[boot] /data mounted` in the default boot, and a file survives a reboot.
3. `/home` says `storage=aurafS`, and `HOME_PLAN`'s H4 flips DONE.
4. The README's first paragraph about storage contains one measured number and
   one honest boundary.
