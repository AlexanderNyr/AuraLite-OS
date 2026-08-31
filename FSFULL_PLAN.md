# AuraLite OS — Full Filesystem Support Plan (ext4 / btrfs / f2fs / exFAT / NTFS)

## Status: IN PROGRESS — F1 ✅ DONE; F2 ✅ DONE; F3–F7 planned 📋

> This is a feature plan in the style of `GL_PLAN.md`, `FSLAYOUT_PLAN.md` and
> `INTERNET_PLAN.md`, written against the tree as it stands. It follows the
> same structure: dependency-ordered phases, a definition of done and a test
> gate for every phase, one `.patch` per phase.
>
> It pays off the series opened by **RES-46 (HANDED-OFF@R12)**:
>
> > *FS data paths + modern NICs — the row itself was part-stale: exfat 103 /
> > ntfs 79 lines ARE probe skeletons, but ext4 1429 / btrfs 969 / f2fs 1309
> > lines (ext4 carries delayed allocation and write paths) — what NONE of the
> > five have is a single CI case (zero in run_all.sh)*
>
> The opener fact is quoted here because it is the plan's thesis: **the five
> filesystems are not equal, and the plan is not five identical rewrites.**
> It is a measured gradient: three contain real filesystem code with real
> design debt, two are skeletons, and the glue around all five has gaps that
> decide the order in which anything else is worth doing.

**Baseline:** commit `06be2ab` (SH9 update 4; `make iso`, `make test-unit`,
`make test-integration-fast` green in the working tree).

**Ledger integration:** when a phase's exit gate runs green, its ledger row
flips `DONE@F<n>` in the same commit (`docs/residue_ledger.md`, class W);
the final phase adds the machine-checkable coverage row (F7 exit gate). The
ledger stays the index of truth; this document is the detail.

---

## 1. Where things actually stand

Measured, not assumed — line counts and call sites are from the baseline
tree, not from the headers.

### 1.1 The five filesystems, measured

| FS | File | Lines | vfs_ops surface | What exists | What is missing |
|---|---|---|---:|---|---|
| ext4 | `kernel/fs/ext4.c` | 1483 | lookup, create, read, write, readdir, mkdir, unlink, stat, truncate | on-disk superblock/BG-descriptor/inode/extent parsing, extent maps (read + write), delayed-allocation flag, journal-flag superblock, in-kernel formatter | JBD2 journal is **flag-only**; no 64-bit feature (`s_blocks_count_hi`), no flex_bg/indexed (HTree) dirs, no symlinks, no hard links, no rmdir/rename, no timestamps, no error paths |
| f2fs | `kernel/fs/f2fs.c` | 1357 | lookup, create, read, write, readdir, mkdir, unlink, stat, truncate | superblock/checkpoint/NAT/SIT structs, node walking (inode/direct/indirect), segment allocator, in-kernel formatter | no SSA (segment summary) reconstruction, no cleaning/GC, no NAT/SIT journals, no fsync, no rename/rmdir, no timestamps |
| btrfs | `kernel/fs/btrfs.c` | 1017 | lookup, create, read, write, readdir, mkdir, unlink, stat | superblock (magic `_BHRfS_M`), B-tree node parse, inline + regular extents, CoW allocate-and-write, in-kernel formatter | no root tree/subvolumes, no chunk tree, no checksums (CRC32C), no tree COW on update, no rename/rmdir/truncate, no fsync |
| exFAT | `kernel/fs/exfat.c` | 103 | lookup, read, write | boot-region read, single-cluster dir scan by exact name | **skeleton**: no FAT chain walk, no cluster chaining, no readdir, no create/mkdir/unlink/rename/stat, no 64-bit size, no time fields, wrong struct offsets |
| NTFS | `kernel/fs/ntfs.c` | 79 | lookup, read | boot-sector read, MFT magic check | **skeleton**: lookup returns a fake vnode unconditionally, read returns the MFT buffer, no attributes ($DATA/$FILE_NAME), no runlists, no $MFT/$MFTMirr, no write |

**The two skeletons are worse than absent.** `ntfs_lookup()` succeeds for
every name and `ntfs_read()` returns MFT bytes, so any test against them
today would be green for the wrong reason — the exact failure shape the
suite's `il_check_synthetic` guard exists to prevent (USB_PLAN U0). The
honest interim state is a loud refusal at mount (F5), not a fake vnode.

### 1.2 The claims in the headers are ahead of the code

`ext4.h` advertises "full journaling ext4", "delayed allocation" and
"HTree-ready structure"; `btrfs.h` advertises "checksums (CRC32C per block,
simplified)" and "subvolumes and snapshots". The first two are partially
true; the third and fourth are not implemented. This plan's F6 **rewrites the
headers to match the code**, because the headers are what a reader (and a
checker script) trusts first — the same debt the RESIDUE pass named in
`TODO.md`.

### 1.3 The glue around them has gaps that decide ordering

| Seam | Today | Consequence |
|---|---|---|
| `bc_init()` runs **after** `ahci_init()` | buffer cache is created at `[boot] initialising buffer cache...`, and `blkdev_register_blkdevs` has already happened | exfat/ntfs use `bc_get` on an uninitialised cache; ext4/btrfs/f2fs use direct `blkdev_read/write` and never touch the cache — **two different I/O stacks** |
| `read_block()`/`write_block()` | `blkdev_read(m4.bdev, m4.base_lba + block, 1, buf)` — one 4 KiB block per call, no cache | every 4 KiB read is a fresh 512-byte-sector round-trip through AHCI |
| `ext4_init(prefer_port)` | ignores the port, uses a global `m4.bdev` | the "prefer" contract is a lie; f2fs and btrfs copy it |
| mount in `kernel.c` | `vfs_mount("/ext4", &ext4_ops, NULL)` with **no superblock validation** beyond magic | a non-ext4 disk at blkdev 3 is **auto-formatted** on boot (see §1.4) |
| `vfs_ops` (Q13 era) | has `link`, `settimes`, `chmod`, `chown` slots; fat32/ext2 fill them | ext4/btrfs/f2fs fill none — timestamps are hardcoded `1337` |
| `usbfs` | a second, **independent** FAT32 reader (`kernel/fs/usbfs.c`, `usb_fat_state`) with a 32-file table and no writes | the FAT32 work of `fat32.c` is not reused; exFAT-on-USB would need a third reader |
| `diskfs` | a raw block view (`/disk`) with no partition table | no partitioning support anywhere (`RES-04`: raw mounts silently ignore GPT/MBR) |

### 1.4 Auto-formatting: the most dangerous behaviour in this tree

Every experimental FS `init` path follows the same shape:

```c
/* kernel/fs/ext4.c:1406 */
if (read_block(0, ext4_scratch) != 0) {
    kprintf("[ext4] cannot read superblock, formatting...\n");
    if (format_ext4() != 0) return -1;
    ...
}
if (sb->s_magic != EXT4_MAGIC) {
    kprintf("[ext4] not ext4 magic (0x%04X), formatting...\n", sb->s_magic);
    if (format_ext4() != 0) return -1;
    ...
}
```

So a disk containing user data that is **not** ext4 — a partition table, a
swap volume, another filesystem, random bits — is destroyed the moment it is
attached as blkdev 3, with no confirmation, no read-only fallback, and no
refusal mode. f2fs (`[f2fs] not F2FS magic, formatting...`) and btrfs
(`[btrfs] not btrfs magic, formatting...`) do the same. This is the single
strongest argument in this document for doing something: **three of the five
drivers are currently data-destroyers by default.**

The fix is small and is F1: a `FS_MOUNT_FORMAT` knob (build + fw_cfg), a
per-FS `format_*` name, and the format branch gated behind it.

### 1.5 The reference implementations to copy from

The tree already contains two **complete, CI-gated** filesystem drivers with
the exact surface these five lack:

- **`kernel/fs/fat32.c` (1641 lines)** — cluster-chain walking, LFN, mkdir/
  rmdir/unlink/rename/truncate, timestamps, `fat32_self_test()`, five
  integration cases, `/fat/AURALOG.TXT` persistence.
- **`kernel/fs/ext2.c` (1604 lines)** — block-group layout, indirect blocks,
  mkdir/rmdir/unlink/rename/link/settimes/chmod/chown, in-kernel mkfs,
  `ext2_self_test()`, `test_ext2.sh` against a **real `mkfs.ext2`-formatted
  disk** — the proof that a Linux-tool-made volume mounts.

The plan's default answer to "how should X work" is: **mirror the
corresponding fat32/ext2 function, then add the format-specific twist.** Not
because fat32/ext2 are perfect, but because they are the only two FS drivers
in the tree whose behaviour is pinned by tests a human can run in one
command.

---

## 2. Decisions

### D1. One gradient, not five identical rewrites

The five filesystems are at three different maturity points, and the plan
says so in its phases:

| Maturity | FS | Treatment |
|---|---|---|
| Real code, real debt | ext4, f2fs, btrfs | **complete** the surface (F3/F4/F6) against their own on-disk formats |
| Skeletons | exFAT, NTFS | **replace** with honest minimal implementations (F5) that refuse loudly where they cannot serve, and add the full surface where the format is tractable (exFAT) |
| Glue | all five | shared seams first (F2), so the per-FS work is done once |

### D2. Refuse loudly before the first byte is written

The exFAT/NTFS skeleton behaviour — success on every lookup, MFT bytes as
file data — is the failure shape the suite already refuses to certify
(USB_PLAN U0's synthetic-data guard). F5 replaces fake success with
`-ENOTSUP`/`-EIO` and a named `[fs]` line, *then* implements. A driver that
says "no" is honest; a driver that returns garbage is a lie with a file
descriptor.

### D3. Auto-formatting dies by default (F1)

The current mount-or-format behaviour is a data-loss bug wearing a feature's
clothes. After F1:

- default build and CI: **refuse to mount a foreign volume**, print
  `[ext4] not ext4 magic (0x%04X); format disabled (FS_MOUNT_FORMAT=0)` and
  leave the device untouched;
- `make iso FS_MOUNT_FORMAT=1` (and `-fw_cfg opt/auralite.fsformat=1`):
  explicit opt-in for the dev loop;
- the opt-in is what the integration cases use, so the *tested* path and the
  *default* path are both exercised — the negative control (format off +
  foreign magic → no writes to the device) is an assertion in every case.

This mirrors the `selftest=` knob precedent (OPT O2): a build default, an
fw_cfg override, and the probe line printed in the boot log.

### D4. One block I/O path for all five (F2)

`bc_init()` moves before `ahci_register_blkdevs()`, `bc_get`/`bc_release`
become the only sector access the five use, and `read_block`/`write_block`
become cache-backed 4 KiB helpers on top. This is the phase that makes
"reads a 1 GiB file" cost the same as "reads 32 KiB", and it is a
prerequisite for every per-FS phase's performance gate. It also deletes the
"two I/O stacks" fact from §1.3.

### D5. ext4 first, because it is the reference

ext4 is the largest, the closest to complete, and the one whose external
tooling is cheapest to install (`mkfs.ext4` is in the same `e2fsprogs`
package the CI already installs). The discipline learned on ext4 — the
external-formatter harness, the foreign-volume negative control, the
64-bit/symlink/hardlink gap list — is then applied to f2fs and btrfs with
their own tooling. `mkfs.f2fs` is a one-package install; btrfs tooling is a
heavier install and is the *last* per-FS phase for exactly that reason.

### D6. exFAT before NTFS

exFAT is a FAT-family format: cluster chains, a FAT table, directory entries
with name/size/cluster — the same vocabulary as `fat32.c`, which is already
working in this tree. NTFS is a different universe (MFT, attribute lists,
runlists, $BITMAP, update sequence arrays) with no existing in-tree
vocabulary. Both start honest (D2); exFAT is expected to reach the full
surface in one phase, NTFS is expected to land a read-only core with an
honest capability list.

### D7. Interop is the goal; the format is the contract

"Support" means: **a volume created by the standard Linux tool mounts, and
a volume created by AuraLite's formatter mounts back in Linux** — in both
directions, for the surface each phase claims. Every per-FS phase therefore
has two lanes:

1. *external* lane: host tool (`mkfs.ext4`, `mkfs.f2fs`, `mkfs.btrfs`,
   `mkfs.exfat`, `mkfs.ntfs` via `ntfs-3g`) formats a raw image → attached as
   a blkdev → AuraLite reads, writes, unmounts;
2. *internal* lane: AuraLite's formatter creates a volume → Linux mounts it
   (loop device) and verifies the files.

A phase whose format claims cannot be verified against the real tool is a
phase whose claim is a guess — the plan does not ship guesses.

### D8. Tests live in `tests/integration/cases/` and `tests/unit/`, registered

The five filesystems have **zero** CI cases today (RES-46's opener fact). F7
is not "write tests"; every phase ships its own gate, and F7's job is the
two *derived* artefacts: an `FSFULL` CI shard and the machine-checkable
coverage row in the ledger (RES-46's exit gate). Cases follow the existing
harness exactly — `il_make_disk`/`il_run_qemu`/`il_assert_grep`, disk per
case, `il_check_synthetic` active — and register in
`tests/integration/run_all.sh`'s `ALL_CASES` array in the same commit the
case lands.

### D9. The ledger rows this plan closes

| Row | Class | Status today | This plan |
|---|---|---|---|
| RES-46 | S | HANDED-OFF@R12 | F1–F7 deliver the series; final row flips DONE@F7 with the coverage row as exit gate |
| RES-04 | W | DONE@R1 | re-affirmed: the raw-mount skip is F1's negative control and F2's partition probe names it |
| RES-07 | W | OPEN | buffer_cache joins the shared lists on all four widths in F2 |
| RES-06 | W | OPEN | vfsmount is the seam F4's rmdir/rename work rides on; the fd half is out of scope, re-affirmed |

---

## 3. Phases

### Phase F1 — Mount safety: kill auto-format, add the knob ✅ DONE

**Objective:** no filesystem driver may destroy a foreign volume by default.

#### Tasks

- [x] A build-time + fw_cfg knob `FS_MOUNT_FORMAT` (0 = refuse, 1 = allow
      format), following the `selftest=` precedent (OPT O2): probe line in
      the boot log, `-fw_cfg opt/auralite.fsformat` override, Makefile
      variable for `make iso FS_MOUNT_FORMAT=1`.
- [x] All five `*_init` paths: the `format_*` branch is gated on the knob;
      with the knob off, a foreign/absent magic prints
      `[ext4] not ext4 magic (0x%04X); format disabled (FS_MOUNT_FORMAT=0)`
      and returns an error **without a single sector write**.
- [x] The mount site in `kernel.c` distinguishes "not mounted (no disk)",
      "mounted", and "refused (foreign volume)" — the last is a distinct,
      greppable line so tests can assert it.
- [x] The `-1` (no disk) and `refused` paths stop calling
      `vfs_mount("/ext4", &ext4_ops, NULL)` — today a failed init is still
      mounted, and the first `ls /ext4` reaches into an uninitialised
      `m4`.

#### Test gate

- With the knob off, a disk pre-filled with a non-magic pattern is attached
  at blkdev 3; boot log shows the refusal line; the disk's first 4 KiB are
  byte-identical after boot (hash before/after in the case script).
- With the knob on, the same disk is formatted and mounted (`[ext4] ...
  formatting` + `[vfs] mounted '/ext4'`).
- `make test-unit` and the existing integration suite unchanged.

**Result:** `tests/unit/test_fsformat.c` (gate default + coercion, real
source, ASan+UBSan), `tests/unit/test_exfat_ntfs.c` (real exfat.c/ntfs.c
against stubs — the NTFS slot is blkdev 6, unreachable from a 6-port
AHCI controller, so the host lane is the NTFS refusal test), and
`tests/integration/cases/test_fsformat_knob.sh` (5 foreign volumes →
named refusals + **byte-identical disk hashes** with the knob off; the
same disks format and mount with the knob on, and the mounted /f2fs
serves a `ls` readdir).
`make test-unit` green; the boot log carries `[fsformat] auto-format:
DISABLED (build)` / `ENABLED (fw_cfg)`.

**Open issues discovered while gating F1 (pre-existing, parked for their
phases):** (1) ext4 data path is unstable — even `ls /ext4` trips KCANARY
in `read_inode`; reproduced on pristine `origin/main` 06be2ab, so not an
F1 regression (F3). (2) f2fs `path_resolve` conflates "directory
missing" with "final component missing", so VFS `open(O_CREAT)` always
fails (F4). (3) btrfs reads its first superblock with `block_size == 0`,
so any pre-existing volume is "unreadable" and pre-F1 was always
re-formatted; F1 now refuses it honestly (F4b).

#### Deliverable

`patches/FS_F1_mount_safety.patch` ✅

---

### Phase F2 — One block I/O path: buffer cache under everything ✅ DONE

**Objective:** all five filesystems read and write through `bc_get`/
`bc_release`; the two-stack fact in §1.3 dies.

#### Tasks

- [x] `bc_init()` moves before `ahci_register_blkdevs()` in `kmain` so the
      cache exists before any FS touches a device (`[boot] initialising
      buffer cache...` now precedes the AHCI section).
- [x] `bc_get`/`bc_release` become the only sector access in the five; the
      direct `blkdev_read/write` calls in ext4/btrfs/f2fs go through a
      shared 4 KiB `fs_read_block(dev, lba, buf)` / `fs_write_block`
      helper (in `kernel/fs/buffer_cache.c`), replacing the per-FS
      `read_block`/`write_block` copies.
- [x] exfat/ntfs stop reading the boot region through an uninitialised
      cache; they use the same helpers (`fs_read_block` on sector 0).
- [x] `bc_sync` is exported to the VFS `sync` slot so a filesystem can
      flush on `sync`/umount; the five set `.sync` (`fs_cache_sync`,
      which flushes the cache and prints the `[bc] hits=N misses=M`
      receipt).  The x86_64 `fsync()` path calls the vnode's `.sync`
      after the page-cache flush.
- [x] (RES-07) the buffer cache compiles on all four widths (shared lists),
      so the x86-only pin in RES-07 closes (`kernel/fs/buffer_cache.c`
      joins KERNEL32_SHARED, KERNELRV_SHARED and KERNELA64_SHARED;
      `make kernel32/kernelrv/kernela64` link green).

#### Test gate

- A boot with a formatted ext4 volume: reading a 1 MiB file through the
  cache shows the cache-hit counter climbing and the raw AHCI sector count
  staying bounded (receipt line `[bc] hits=N misses=M` asserted in the
  case).
- The same file is byte-identical pre-/post- a write-through cycle.
- `make test-unit` (existing `test_blkdev`, `test_fat32`) green; the four
  widths build.

#### Deliverable

`patches/FS_F2_bcache_seam.patch` ✅

---

### Phase F3 — ext4: complete the surface, prove interop ✅ planned

**Objective:** ext4 is the reference implementation for the rest of the
plan: real files on real `mkfs.ext4` volumes, full mutation surface, honest
feature list.

#### Tasks

- [ ] **External-formatter harness** (the pattern every later FS copies):
      `mkfs.ext4` formats `$IL_BUILD/ext4.img`; the case attaches it as
      blkdev 3 and drives the shell: `ls /ext4`, `cat`, `write`, `mkdir`,
      `mv`, `rm`, `rmdir`, `stat`, `truncate`, `sync`.
- [ ] **64-bit sizes**: `s_blocks_count_hi`/`s_inodes_count_hi` and
      `i_size_high` parsed and honoured (`m4.blocks_count` becomes 64-bit;
      files > 4 GiB read correctly from a big sparse image).
- [ ] **rmdir + rename** (`ext4_rmdir`, `ext4_rename` — the only mutation
      ops missing from `ext4_ops`), plus `.link`/`.settimes` so the Q13-era
      slots stop being empty.
- [ ] **Symlinks**: fast (in-inode) and slow (block) symlink reading, since
      `readdir` already emits `VFS_TYPE_SYMLINK` for `EXT4_FT_SYMLINK`.
- [ ] **HTree**: parse only (readdir), with `EXT4_FT_DIR_CSUM`-era dx_root
      detection; indexed-dir *writing* stays out of scope (D-notes).
- [ ] **Journal**: mount refuses read-write when
      `EXT4_FEATURE_COMPAT_HAS_JOURNAL` is set and `s_journal_inum` is
      nonzero — JBD2 replay stays out of scope; a `mkfs.ext4 -O ^has_journal`
      volume is the tested lane.
- [ ] **Self-test** extended: multi-block file (> one extent), subdir
      traversal, rename, unlink, truncate up and down, 64-bit size field.

#### Test gate

- `test_ext4.sh` (new): external-lane asserts — every mutation visible from
  the shell, content byte-exact, `[ext4] PASS:` receipt; internal lane —
  `format_ext4()` volume, then `fsck.ext4 -n` (or `e2fsck -fn`) on the image
  from the host.
- Negative control from F1 holds (foreign volume refused, hash unchanged).
- `mkfs.ext4 -O ^has_journal` volume mounts read-write and passes the same
  mutation script.

#### Deliverable

`patches/FS_F3_ext4.patch`

---

### Phase F4 — f2fs: checkpoint/NAT/SIT reality, then btrfs: tree COW reality ✅ planned

**Objective:** the two remaining "real code" filesystems reach the same
honest bar, in dependency order (f2fs first: its tooling is a one-package
install; btrfs last: its tooling is the heaviest).

#### Tasks (f2fs)

- [ ] `mkfs.f2fs` external harness (`test_f2fs.sh`); internal-lane check
      with the host `fsck.f2fs`.
- [ ] **NAT + SIT reconstruction on mount** (node address table and segment
      info table parsed from the superblock offsets — currently read
      never); SSA (segment summary) read for block ownership.
- [ ] **Checkpoint validation**: both CP packs compared, newer-version
      selection, checksum verified; refuse (loud) on torn CP instead of
      half-mounting.
- [ ] **rename + rmdir**, `.link`/`.settimes`; `fsync` as a `sync`-slot
      flush of the current segment.
- [ ] **Multi-segment files**: writes spanning segment boundaries allocate
      and chain correctly; file > 2 MiB round-trips.
- [ ] Cleaning/GC explicitly out of scope (named in the file header and in
      this plan's D-notes) — the log-structured writer *is* the future GC's
      foundation, but shipping a correct sequential writer first is the
      honest milestone.

#### Tasks (btrfs)

- [ ] `mkfs.btrfs` external harness (`test_btrfs.sh`) — the interop lane
      that will surface every layout assumption the current code makes.
- [ ] **Tree COW on update**: node updates allocate a fresh block and
      re-parent (the "copy-on-write" the file header claims but the code
      does not do on metadata).
- [ ] **CRC32C checksums**: verify on read, compute on write, for the
      claimed-but-unimplemented data-integrity story.
- [ ] **rename + rmdir + truncate**; `.link`/`.settimes`.
- [ ] **Subvolumes/snapshots**: explicitly out of scope (the header's
      claim is retracted in F6).
- [ ] Self-test extended: CoW write → read-back → overwrite → verify both
      generations' blocks exist and the tree still resolves.

#### Test gate (both)

- External lane: mutations round-trip from the shell on `mkfs.*` volumes;
  internal lane: host `fsck` passes on AuraLite-formatted images.
- Multi-segment / multi-block files byte-exact after power-loss-free
  remount (umount + re-attach in the same case).
- `[f2fs] PASS:` / `[btrfs] PASS:` receipts pinned.

#### Deliverables

`patches/FS_F4_f2fs.patch`, `patches/FS_F4b_btrfs.patch`

---

### Phase F5 — exFAT and NTFS: replace the skeletons with honesty ✅ planned

**Objective:** the two skeletons stop lying, and exFAT reaches the full
surface because it is a FAT-family format with an in-tree reference
(`fat32.c`).

#### Tasks (exFAT)

- [ ] **Correct on-disk structs**: the current `exfat_boot_region`/
      `exfat_dir_entry` are a single-cluster approximation; port the real
      layout from the spec (`exfat_boot_sector`, `exfat_entry_*` set
      types, 32-bit `fat_length`/`cluster_count` fields the current
      16-bit fields truncate).
- [ ] **Cluster-chain walking** via the FAT (fat32.c's chain walk is the
      reference), multi-cluster file reads, `readdir`, `stat` with real
      sizes.
- [ ] **create / mkdir / unlink / rename / truncate**, entry-set
      allocation (0x85 + 0xC0/0xC1 name entries), timestamps via
      `exfat_settimes`.
- [ ] `mkfs.exfat` external harness (`test_exfat.sh`); internal-lane check
      with `fsck.exfat` (exfatprogs).
- [ ] The format knob (F1) and the cache seam (F2) apply unchanged.

#### Tasks (NTFS)

- [ ] **Stop lying first**: `ntfs_lookup` returns `-ENOENT`/`-ENOTSUP` for
      everything and `ntfs_read` refuses, with a `[ntfs]` line naming the
      limitation — until the read-only core lands in the same phase.
- [ ] **Read-only core**: MFT record parse (`FILE` magic + update sequence
      array fixup), attribute list walk ($FILE_NAME, $DATA, $STANDARD_
      INFORMATION), **runlist decode** (the sparse `(lcn,count)` runs), file
      read through runs, root directory `readdir` via the $I30 index for
      `FILE_NAME` attributes.
- [ ] `mkntfs` (ntfs-3g) external harness (`test_ntfs.sh`): a real NTFS
      volume with a few files is read correctly.
- [ ] Write/mkdir/rename: out of scope, named in the header and D-notes;
      the mount is read-only and says so.

#### Test gate (both)

- No case may pass while the driver returns fake data: each case asserts
  the `[fs]` refusal/read-only lines and the real reads.
- exFAT: full mutation script round-trips; internal-lane fsck passes.
- NTFS: the files written by `mkntfs` on the host are byte-exact in the
  guest; a write attempt returns `-EROFS`.

#### Deliverables

`patches/FS_F5_exfat.patch`, `patches/FS_F5b_ntfs.patch`

---

### Phase F6 — Headers, claims and docs catch up with the code ✅ planned

**Objective:** nothing in this tree claims more than the code delivers.

#### Tasks

- [ ] `ext4.h`, `f2fs.h`, `btrfs.h`, `exfat.h`, `ntfs.h` rewritten to state
      exactly what each driver implements, what is refused, and what is
      out of scope (the retracted claims: btrfs "subvolumes and snapshots",
      "checksums" until F4b, ext4 "full journaling", "HTree-ready"
      writing).
- [ ] `docs/status.md` rows for the five move from "Experimental" to the
      per-FS capability list with the same honesty bar as the w32 row.
- [ ] `docs/filesystem.md` gains a "Filesystem support matrix" section
      (mount points, read/write/mutation surface, external-tool
      verification status).
- [ ] `TODO.md` entries for the five are annotated with their F-phase
      receipts in the file's own style.

#### Test gate

- A new `tools/check_fsfull_claims.py` (in the style of
  `check_fixes_claims.py`/`check_selfhost_claims.py`): parses this plan's
  phase checkboxes and asserts the claimed receipts exist in the tree —
  with a negative-control self-test (planted violation caught). Registered
  in `make test-unit`.

#### Deliverable

`patches/FS_F6_docs_claims.patch`

---

### Phase F7 — Coverage shard and the ledger's machine-checkable row ✅ planned

**Objective:** RES-46's exit gate — the five filesystems have CI cases and
the ledger can prove it.

#### Tasks

- [ ] `test_ext4.sh`, `test_f2fs.sh`, `test_btrfs.sh`, `test_exfat.sh`,
      `test_ntfs.sh` registered in `tests/integration/run_all.sh`
      `ALL_CASES` (each with its own disk; no shared state between cases).
- [ ] A `--group fsfull` shard (mirroring the existing `--group usb` shard
      partition) wired into the CI workflow.
- [ ] The five kernel self-tests (`ext4_self_test` et al.) run in the
      `full` selftest lane when their volumes are mounted — today they are
      wired to `test_ext4_smoke` and the experimental-tests gate that is
      disabled in normal boot.
- [ ] Ledger: RES-46 flips `DONE@F7` with the coverage row as its exit
      gate; `tools/residue_baseline.txt` moves in the same commit.

#### Test gate

- `make test-integration --group fsfull` green on a clean checkout.
- The shard partition check (`run_all.sh --check-groups`) passes with the
  new group.
- `tools/check_residue_claims.py` green with the F7 row.

#### Deliverable

`patches/FS_F7_coverage.patch`

---

## 4. Order and rationale

| Phase | Why here |
|---|---|
| F1 | Nothing else is safe until no driver can destroy a foreign volume; every later case needs the knob and the refusal line anyway |
| F2 | One I/O path before per-FS work means the performance and correctness work is done once, not five times |
| F3 | ext4 is the largest and closest; its external harness becomes the template the other four copy |
| F4 | f2fs then btrfs: same discipline, own tooling; btrfs last because its tooling install is the heaviest |
| F5 | The skeletons are dishonest; exFAT's FAT-family shape makes full support cheap, NTFS lands a read-only core with a named boundary |
| F6 | Claims-first is how this tree works; the headers must not outrun the code at the end of a plan any more than at the start |
| F7 | The series' exit gate: coverage in the shard and a machine-checkable ledger row |

**If only two phases are ever built, build F1 and F2.** F1 removes the
data-loss behaviour; F2 makes every later filesystem phase smaller. They
are the only phases whose value does not depend on any other phase.

---

## 5. Risks

**Interop reveals format drift.** The current code was written against the
formats from memory and against its own formatter. The first real
`mkfs.ext4`/`mkfs.f2fs`/`mkfs.btrfs`/`mkfs.exfat`/`mkntfs` volume will
almost certainly expose layout assumptions (field widths, checksums,
feature flags, reserved areas). This is the plan working, not failing —
the external lane exists precisely to surface it — but F3's first milestone
is expected to be a list of "what the real format does differently", not a
green case.

**F4b (btrfs) could end at a documented refusal.** If the real `mkfs.btrfs`
layout diverges beyond the phase's scope, the honest outcome is a read-only
mount with a named feature list, exactly as NTFS lands in F5. The plan
prefers a working read-only driver to a broken read-write one.

**Auto-format removal changes the dev loop.** Everyone who currently
attaches a blank disk and expects `/ext4` to appear will see a refusal
until they pass the knob. The probe line and the README/`docs/status.md`
note are the mitigation; the negative control in every case is the proof
the change is real.

**The cache move touches the boot path.** Moving `bc_init()` earlier
changes allocation order in `kmain`; the existing boot self-tests are the
regression net, and F2's gate requires the full suite, not just the new
case.

**NTFS is a different universe.** MFT attribute lists and runlists have no
in-tree precedent (the current 79 lines are a fake). F5b is written as a
read-only core with a named boundary precisely so the risk is contained;
if even the read-only core proves too large, the phase still delivers the
"stop lying" half, which is independently valuable.

**Timestamps are a lie today.** `f2fs_truncate` writes `inode.mtime =
1337`. The `.settimes` work in F3/F4/F5 makes the *plumbing* real; the
wall-clock RTC remains the tree-wide limitation (epoch 0 in the boot log)
and is out of scope here, named in this plan's D-notes.

---

## 6. What this plan does not do

- **No journaling (JBD2) replay, no ext4 HTree writing, no ext4
  flex_bg-group allocation.**
- **No F2FS garbage collection, no SIT/NAT journals, no fsync barriers.**
- **No btrfs subvolumes, snapshots, RAID, checksum trees beyond F4b's
  CRC32C, no reflink.**
- **No NTFS writes, compression, $LogFile replay, ACLs.**
- **No partitioning or GPT/MBR parsing** (RES-04 stays re-affirmed: the raw
  mount skip is F1's negative control, not a partitioner).
- **No exFAT on USB** — the `usbfs` FAT reader stays as-is; wiring the real
  `fat32.c`/`exfat.c` into `usbfs` is a separate plan.
- **No RTC wall-clock timestamps** — the `.settimes` plumbing lands; the
  clock does not.
- **No multi-user permissions, quotas, or fsck-in-guest.**
- **No promise the list is complete.** The external lanes will find what
  they find; the phase gates are written so that a discovered gap is
  receipted and either scheduled or refused with a name, never silently
  absorbed.

---

## 7. Checklist

- [x] F1 mount safety (knob, refusal, no-write negative control)
- [x] F2 buffer-cache seam (one I/O path, bc before ahci, `.sync` slots)
- [ ] F3 ext4 (interop harness, 64-bit, rmdir/rename/link/settimes,
      symlinks, HTree-parse, journal refusal)
- [ ] F4 f2fs (NAT/SIT/SSA, CP validation, rename/rmdir, multi-segment)
- [ ] F4b btrfs (tree COW, CRC32C, rename/rmdir/truncate)
- [ ] F5 exFAT (real structs, chains, full mutation, interop)
- [ ] F5b NTFS (stop lying, read-only core, runlists, interop)
- [ ] F6 headers/claims/docs + `check_fsfull_claims.py`
- [ ] F7 coverage shard + RES-46 DONE@F7 + baseline move
