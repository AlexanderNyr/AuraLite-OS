# AuraLite OS — Filesystem Modernization Plan (`FSMODERN_PLAN.md`)

## Status: M0 ✅ DONE (the instrument and the first numbers) · M1 ✅ DONE (/data survives a power-off) · M2 ✅ DONE (4 KiB blocks: /fat read 7.4×) · M3–M7 ⬜

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

### M3 — The durability contract ⬜

- `fs_sync(vnode, mode)` added to `vfs_ops`; each filesystem either implements
  it honestly or declares `FS_DURABILITY_WEAK`.
- **The crash test:** `test_fs_crash.sh` starts a write loop, sends QEMU a
  power-off at a random point, reboots, and asserts (a) the file is either
  fully there or absent, never half-made, and (b) the filesystem mounts
  without repair on the next boot.
- ext2 is allowed to fail (b) and to say so — that is the point of the
  declaration. fat32 must pass (a) for a single-sector write, which its
  FAT-update ordering already implies.

### M4 — ext2 and fat32 get their missing pieces ⬜

The two filesystems a real user actually touches, brought to the same bar as
the fancy ones:

- ext2: a **bitmap-consistency checker** at mount (`fscheck.c` already exists
  and already detected an inode/bitmap disagreement in the unit run — promote
  it from a test fixture to a boot-time check with a receipt), and a
  documented answer to "what happens on a crash" (M3's declaration).
- fat32: LFN write support gaps if any remain, plus `fsync` reaching the FAT
  sectors before the data sectors (ordering, not speed).

### M5 — Directory scaling ⬜

Every filesystem here has a linear directory scan. At the sizes a user reaches
(`/home` with a few hundred files, `/opt` with a package tree), that is fine;
at 10 000 files it is quadratic. M5 adds hashed directories **to the flagship
only** (M6's substrate), because that is where the claim will be made — and
measures the crossover point rather than assuming one.

### M6 — AuraFS: the flagship ⬜

The user's sentence: *"сделать свою файловую систему очень быструю и сделать
её фишкой ОС."*

Built by evolving btrfs.c per D2:

1. **Log-structured allocation** — a write cursor replaces free-block search;
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
