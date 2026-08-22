# AuraLite OS — Platform Parity Plan (i386 / rv64 / a64 catch-up)

## Status: IN PROGRESS — P0 complete; P1 next; plan committed 2026-08-22

| Phase | Result | Deliverable |
|-------|--------|-------------|
| P0 — the rig: blkdev seam audit + claims checker | ✅ complete | `patches/PARITY_P0_rig.patch` |
| P1 — the block-device layer (x86_64 byte-honest refactor) | pending | `patches/PARITY_P1_blkdev.patch` |
| P2 — ext2 mounted on rv64 (vblk behind the seam) | pending | `patches/PARITY_P2_rvfs.patch` |
| P3 — ext2 mounted on a64 (the shared-transport dividend) | pending | `patches/PARITY_P3_a64fs.patch` |
| P4 — the syscall widening: open/read/close on three ports | pending | `patches/PARITY_P4_syscalls.patch` |
| P5 — SMP rv64: SBI HSM hart_start | pending | `patches/PARITY_P5_rvsmp.patch` |
| P6 — SMP a64: PSCI CPU_ON | pending | `patches/PARITY_P6_a64smp.patch` |
| P7 — i386: the fs width pay-down + ATA behind the seam | pending | `patches/PARITY_P7_i386fs.patch` |
| P8 — libc subset promotion for the DTB tenants | pending | `patches/PARITY_P8_libc.patch` |
| P9 — CI lanes, docs, the honest matrix flip | pending | `patches/PARITY_P9_ci.patch` |

## 1. Where this plan comes from

Three COMPLETE plans each closed with the same residue class named,
not hidden: I386_PLAN §I8 ("VFS/FAT32/ext2 mounts, TCP/sockets,
compositor — residue, named"), RISCV_PLAN §V8 ("the full lib/libc
port and the VFS mount of the rv64 blk device are follow-on work"),
ARM64_PLAN's closing matrix (three 🚧 cells in the Userspace row, SMP
"D5's named ramp on both DTB tenants").  HW_PLAN then equalized the
string-ops floor across all four builds, which removed the last
excuse that the ports were too slow to bother mounting anything on.

This plan is the catch-up series: it exists to flip 🚧 cells to ✅
in the parity matrix that ARM64_PLAN.md carries, and to do it in the
order the measurements (below) say is cheapest-first.

## 2. The facts, measured 2026-08-22 (tree = upstream 747d008)

### Fact 1 — the fs tree is ALREADY portable; the blocker is a missing seam

`kernel/fs/` is 19 C files, 11 788 lines.  All 19 compile clean
TODAY under both DTB-tenant flag sets (`-fsyntax-only`, `-Wall
-Wextra`):

    --target=riscv64 -march=rv64gc -mabi=lp64d .. : 19/19 pass, 0 errors
    --target=aarch64-unknown-none-elf -mstrict-align .. : 19/19 pass, 0 errors

The A6/V6 width sweeps did their job before this plan existed.  What
stops the link is not width — it is **41 direct `ahci_*` call sites
on 28 grep lines across 8 fs files** (buffer_cache.c, diskfs.c,
ext2.c, ext4.c, fat32.c, f2fs.c, btrfs.c, procfs.c; the plan's draft
counted lines with `grep -rn .. | wc -l` = 28, the P0 rig counts
occurrences = 41 — several lines carry two calls, and the ratchet
keeps the stricter number).
The fs tree is hard-wired to one x86 SATA driver.  Meanwhile
`vblk_rv.h` / `vblk_a64.h` already export exactly the shape a seam
wants: `int read(uint64_t lba, uint8_t *buf512)`, same for write,
plus an `available()` probe.  The missing piece is one narrow
interface, not a port.

P0 amendment, recorded not hidden: the draft measurement above ran
without `-Werror`.  With the tenant's own `-Werror`, three files
(btrfs.c, ext4.c, f2fs.c) trip `-Wunused-function` — helpers whose
only callers are `#ifdef`'d out — a warning class the x86_64 kernel
build silences with `-Wno-unused-function` and the tenant CFLAGS do
not.  Zero PORTING errors either way; the rig's live lanes mirror
the x86_64 warning policy and the three files are named here so the
flag question is settled the day one of them joins a tenant build.

### Fact 2 — the same files cost 32 errors on i386, in 14 files

Under `--target=i686-elf -Wshorten-64-to-32 -Werror` (the kernel32
flag set), 14 of the 19 fs files fail with **32 errors total**.
That is the entire measured price of I8's VFS residue — two dozen
truncation sites, not a rewrite.  The width-sweep ratchet (359
tracked casts) is DESIGNED for this: each fs file that goes clean
clicks the counter down in the same commit.

### Fact 3 — six syscalls per port; a mount would be mute

The x86_64 syscall surface is ~290 dispatch cases.  Each port
carries exactly **6**: READ, WRITE, SPAWN, GETPID, YIELD, EXIT
(`user_rv.c`, `user_a64.c`, `user32.c` — 6 `case` labels each,
counted).  Mounting ext2 without open/close/readdir/stat gives a
filesystem no shell can look at.  P4 is therefore not optional
polish; it is the difference between "linked" and "usable".

### Fact 4 — the SMP ramps exist on both DTB tenants; neither has an engine

rv64 `boot.S` already runs a `hart_lottery` — one hart proceeds,
the rest park (line 47).  a64 `boot.S` parks non-boot cores at
`.Lpark` (line 121).  But `sbi.c` contains **no HSM extension** (no
`hart_start`, grep empty) and `psci.c` contains **no CPU_ON** —
only the comment promising it ("D5's named exit ramp, unused until
SMP").  QEMU's `-machine virt` takes `-smp N` on both; no run target
passes it today.  The x86_64 side has the full pattern to copy:
`smp.c`, per-CPU stacks, `smp_get_schedulable_cpu_count()`.

### Fact 5 — the libc gap is 70 lines vs 10 921

`lib/libc` (x86_64, full): 10 921 lines.  `lib/libc32`,
`lib/libcrv`, `lib/libca64`: one header each, 70/71/68 lines — the
smallsh survival kit.  P8 does NOT promise the full port (that
stays the named non-goal it has been since V8); it promises a
**shared subset** — errno, string.h forwarding to kernel/lib
bodies, fd-based stdio-lite — sized by what P4's syscalls can
actually carry.

## 3. Decisions

### D1. Measured, not assumed (inherited)
No number, no claim.  Every phase result quotes counts from the tree
or receipts from a boot log; TCG numbers are labelled TCG.

### D2. One seam, four consumers — no per-arch fs forks
P1 introduces `kernel/fs/blkdev.h` (ops table: `read_sector`,
`write_sector`, `sector_count`, name) and converts all 41 ahci call
sites to it.  x86_64 registers AHCI as blkdev 0 — behavior
identical, and the proof is the existing fs integration shard
staying green plus `ext2.o`/`fat32.o` diffing to seam-only changes.
rv64/a64 register vblk, i386 registers ATA PIO.  The fs tree is
never #ifdef'd per arch; the seam is the whole story.

### D3. The ratchet counts ahci-in-fs, and it only goes down
P0 arms `tools/check_parity_claims.py` with the measured baseline
(41 occurrences; the draft said 28 — see Fact 1's amendment).  P1 takes it to 0 and it STAYS 0 — a new direct driver call
from `kernel/fs/` is a checker failure, same contract as the width
sweep's 359/69/0/29.

### D4. Syscall numbers are per-port ABI, names are shared
P4 keeps each port's register convention (a7/x8/eax number) and
extends each dispatcher with the same five names: OPEN, CLOSE,
READDIR, STAT, LSEEK.  No attempt to unify numbers with x86_64's
290-case table — parity of capability, not of ABI.

### D5. SMP is boot + park + IPI-receipt, not a scheduler rewrite
P5/P6 deliver: secondary bring-up (HSM `hart_start` / PSCI
`CPU_ON`), per-CPU stacks, one IPI round-trip receipt
(`[smp] hartN online` / `[smp] coreN online`, counted lines equal
`-smp N`), and idle-park on secondaries.  Migrating the scheduler
to per-CPU runqueues on the DTB tenants is NAMED RESIDUE — the x86
scheduler_rq work showed that is its own series.

### D6. i386 pays width debt through the existing ratchet
P7's fs fixes lower the 359 baseline in the same commits that make
files -Wshorten-64-to-32-clean.  No parallel counter, no second
source of truth.

### D7. QEMU output to files only (inherited, absolute)
Every smoke this plan adds writes serial to a file, greps with
limits, checks `wc -c` first, keeps the 200KB log fuse.

### D8. Phase hygiene (inherited)
Each phase: CHANGELOG top entry, this plan's table updated, one
patch in `patches/` (includes plan+changelog), verified with
`git apply` against a clean upstream clone, commit `<name> update`,
bundle refresh.

## 4. Phases

### P0 — the rig: seam audit + claims checker — ✅ COMPLETE
- [x] `tools/check_parity_claims.py`: pins Fact 1 (19/19 rv64+a64
      fs syntax passes — run live, not quoted), the ahci-in-fs
      count as an exact-pin ratchet starting at 41, the 6-case syscall
      count per port (grows only when P4 lands, then pins the new
      number), and the plan-table/patch-file consistency the other
      checkers enforce.  Wired into `make test-unit` + selftest.
- [x] The seam design note in this plan (§6): ops struct, device
      registry (fixed 4 slots), sector-size stance (512 only, the
      three backends agree today — measured at P0:
      `AHCI_SECTOR_SIZE 512`, vblk's `buf512` contract, ata32's
      256-word PIO loop; the checker asserts all three).
- [x] Registry entry for every new integration case this plan will
      add — **implemented as the checker's PHASE_ARTEFACTS table,
      a deviation, named**: `check_test_registry.py` rightly
      demands that ALL_CASES mirror case files that exist TODAY, so
      reserving future names there would redden the tree.  The
      reservation lives where it can be enforced: a phase row that
      flips ✅ without its reserved artefacts on disk fails the
      parity checker (rv_fs_smoke.sh, a64_fs_smoke.sh,
      rv_smp_smoke.sh, a64_smp_smoke.sh, i386_fs_smoke.sh,
      blkdev.{c,h} + test_blkdev.c, libcmini.h).

#### P0 result

12 claims verified, selftest PASS (doctored-tree detection).  The
rig earned its keep before the phase closed — two catches against
the plan's own draft measurements, both now amended in Fact 1:

- **41, not 28.** The draft counted grep LINES of ahci coupling;
  the rig counts call sites — several lines carry two.  The
  ratchet pins the stricter number and fails in BOTH directions
  (a drop without a same-commit pin edit is a phase forgetting to
  claim its own win).
- **The draft syntax lanes ran without `-Werror`.**  With it,
  btrfs.c/ext4.c/f2fs.c trip `-Wunused-function` (#ifdef'd-out
  callers) — the x86_64 build silences that class, tenant CFLAGS
  don't.  Zero porting errors either way; the live lanes mirror
  the x86_64 policy, the three files are named, and the flag
  question is settled the day one of them joins a tenant build.

Live lanes cost ~1.3 s per checker run (38 clang -fsyntax-only
invocations), paid on every `make test-unit` from now on.

### P1 — the block-device layer
- [ ] `kernel/fs/blkdev.{c,h}`: ops table + registry.  AHCI wraps
      into `blkdev_ahci` on x86_64; **all 41 call sites** converted;
      ratchet 41 → 0.
- [ ] Proof of byte-honesty: the fs integration shard (11 cases)
      green before/after; `results-fs.txt` quoted in the phase
      result.  procfs's ahci lines become blkdev lines (the one
      user-visible diff, named).
- [ ] Host unit test: `tests/unit/test_blkdev.c` — a RAM-backed
      fake device, mount ext2 image over it, read a file, compare
      sha256 (host-runnable, no QEMU).

### P2 — ext2 mounted on rv64
- [ ] `vblk_rv` registers as blkdev; `vfs.c ext2.c buffer_cache.c
      devfs.c tmpfs.c cwd.c symlink.c` join KERNELRV_SHARED (the
      string.c promotion shape).
- [ ] Boot receipt: `[vfs] mounted / (ext2, N inodes)` on the rv64
      serial log; `rv_fs_smoke.sh` boots with `-drive` +
      virtio-mmio-blk carrying the SAME ext2.img the x86 tests use,
      cats a known file, compares content.
- [ ] Linker-size delta measured and quoted (kernelrv.elf is
      601 656 bytes today — the phase result records the growth).

### P3 — ext2 mounted on a64
- [ ] Same seam, fourth consumer: vblk_a64 registers, the same
      shared objects join KERNELA64_SHARED, `a64_fs_smoke.sh`
      mirrors P2's proof (kernela64.elf baseline: 308 144 bytes).
- [ ] Deviation budget: if a64 needs ONE line different from rv64
      outside arch/, that line is quoted in the phase result and
      justified, or the seam is wrong.

### P4 — the syscall widening
- [ ] OPEN/CLOSE/READDIR/STAT/LSEEK on all three ports (6 → 11
      cases each; checker pins 11).
- [ ] smallsh grows `ls` and `cat` builtins THROUGH the new
      syscalls (shared source, three builds — the smallsh contract).
- [ ] Receipts: `ls /` output over serial on rv64, a64, i386
      smokes; each case registered.

### P5 — SMP rv64
- [ ] `sbi.c` gains the HSM extension (EID 0x48534D):
      `sbi_hart_start`; secondary entry in boot.S (skip the
      lottery, take a per-hart stack, report in).
- [ ] `-smp 4` in the smoke; receipt: 3 × `[smp] hart N online
      (stack=..)` + one IPI round-trip line via sbi_send_ipi.
- [ ] Secondaries park in wfi idle — no scheduler claims (D5).

### P6 — SMP a64
- [ ] `psci.c` gains CPU_ON (0xC4000003, the function the file's
      own comment promised); vectors/boot.S secondary path;
      GIC SGI for the IPI receipt.
- [ ] Same receipt shape as P5, `-smp 4`, counted lines.

### P7 — i386: fs width pay-down + ATA behind the seam
- [ ] The 32 shorten-64-to-32 errors in 14 files → 0; width-sweep
      baseline 359 clicks down by the sites actually fixed (the
      number lands in the phase result, not promised here).
- [ ] `ata32` registers as blkdev; KERNEL32 links the same fs
      objects (they are -m32-clean after the pay-down, by
      construction); i386 smoke mounts ext2, cats the known file.
- [ ] TCP/sockets on i386 stay NAMED RESIDUE (unchanged since I8;
      this plan does storage parity, not net parity).

### P8 — libc subset promotion
- [ ] `lib/libcmini/` (one source set, three builds): errno, the
      string.h family (forwarding to kernel/lib/string.c bodies
      userspace-side), open/read/write/close/lseek wrappers over
      P4's syscalls, a 256-byte fd-less printf into SYS_WRITE.
- [ ] libc32/libcrv/libca64 headers shrink to includes of the
      shared one; line counts before/after quoted.
- [ ] Full-libc port (TLS, malloc-over-mmap, stdio buffering)
      REMAINS the named non-goal — this phase moves the floor, not
      the ceiling.

### P9 — CI, docs, the matrix flip
- [ ] Integration lanes: the new smokes join their parity jobs
      (riscv-parity, aarch64-parity, i386-parity) — no new jobs
      unless a timing measurement says the shards need it.
- [ ] `docs/status.md` + ARM64_PLAN's matrix: Storage stays ✅ but
      gains "(mounted VFS)" on three ports; Userspace 🚧 cells
      annotated with the libcmini floor; SMP row appears with
      honest counts (`x86: N CPUs scheduled; rv64/a64: 4 online,
      1 scheduled — residue named`).
- [ ] Terminal arithmetic: claims counted, checker totals quoted,
      residue list (per-CPU runqueues on DTB tenants, i386 TCP,
      full libc, PCIe ECAM, Rust rows) carried forward by name.

## 5. Terminal arithmetic — filled at close

(Counts land here when P9 closes the plan; the checker enforces the
table above matches patches/ on disk.)

## 6. The seam, designed once

    struct blkdev_ops {
        int (*read_sector)(void *ctx, uint64_t lba, void *buf512);
        int (*write_sector)(void *ctx, uint64_t lba, const void *buf512);
        uint64_t (*sector_count)(void *ctx);
    };
    int blkdev_register(const char *name, const struct blkdev_ops *ops, void *ctx);

Four slots, 512-byte sectors (all three backends present 512 today
— AHCI logical, virtio-blk default, ATA PIO; measured at P0, and if
a backend ever reports otherwise the register call refuses loudly
rather than lying).  fs code takes a `int dev` where it used to
imply "the AHCI port"; diskfs/fat32/ext2 mount calls name the
device explicitly.  No partition table parsing enters this plan —
the fs-lba offsets stay where they are (fat32.c already carries
them), and GPT is residue if anyone asks.
