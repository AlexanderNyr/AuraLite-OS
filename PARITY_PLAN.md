# AuraLite OS — Platform Parity Plan (i386 / rv64 / a64 catch-up)

## Status: IN PROGRESS — P0–P6 complete; P7 next; plan committed 2026-08-22

| Phase | Result | Deliverable |
|-------|--------|-------------|
| P0 — the rig: blkdev seam audit + claims checker | ✅ complete | `patches/PARITY_P0_rig.patch` |
| P1 — the block-device layer (x86_64 byte-honest refactor) | ✅ complete | `patches/PARITY_P1_blkdev.patch` |
| P2 — ext2 mounted on rv64 (vblk behind the seam) | ✅ complete | `patches/PARITY_P2_rvfs.patch` |
| P3 — ext2 mounted on a64 (the shared-transport dividend) | ✅ complete | `patches/PARITY_P3_a64fs.patch` |
| P4 — the syscall widening: open/read/close on three ports | ✅ complete | `patches/PARITY_P4_syscalls.patch` |
| P5 — SMP rv64: SBI HSM hart_start | ✅ complete | `patches/PARITY_P5_rvsmp.patch` |
| P6 — SMP a64: PSCI CPU_ON (+ the x16 amendment) | ✅ complete | `patches/PARITY_P6_a64smp.patch` |
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

### P1 — the block-device layer — ✅ COMPLETE
- [x] `kernel/fs/blkdev.{c,h}`: ops table + registry.  AHCI wraps
      into `ahci_register_blkdevs()` (driver-side, detection order,
      so blkdev id N is exactly the old N-th port); **all 41 call
      sites** converted; ratchet 41 → 0 (pin edited same-commit).
- [x] Proof of byte-honesty: the fs integration shard (11 cases)
      green after the cut; `results-fs.txt` quoted in the phase
      result.  procfs's diskstats row is the one user-visible diff,
      named: label `ahci0` → `blk0`, and the counters are
      filesystem-layer now (the AHCI self-test's probe sectors no
      longer count).
- [x] Host unit test: `tests/unit/test_blkdev.c` — RAM-backed fake
      device with fault injection, ASan+UBSan, 27 checks: ids/order,
      the 512-refusal, batch-vs-per-sector agreement, bounds, stats
      counting only successful seam traffic, the ext2 superblock
      magic 0xEF53 parsed through the seam, the slot cap.
      **Deviation, named:** the draft promised mkfs.ext2 + sha256
      here; that wants e2fsprogs on every runner, so the end-to-end
      file-read proof stays with the in-guest ext2/fat32 cases that
      now run through the seam, and the unit test pins the seam
      semantics themselves.

#### P1 result

Kernel: 2 364 904 → 2 371 128 bytes (+6 224 for the seam + wrappers).
The fs shard, clean run through the seam — results-fs.txt verbatim:
11/11 (ahci_large_read 100s, ahci_rw 25s, fat32_persistence 50s,
fat32_full 35s, ext2 66s, fs_stress 80s, devfs 30s, procfs 30s,
tmpfs 35s, diskfs 60s, fat32_mkdir 60s; total 571s).  A rig lesson
paid on the way: the first shard attempt ran TWICE concurrently
(a stray background copy survived a tool timeout), two QEMUs shared
the scratch disks and results files, and fs_stress/diskfs "failed"
with interleaved markers — caught by the .out file showing 13/12
assertions from two interleaved runs, re-run serially, 11/11.
`make test-unit` green end-to-end (27/27 blkdev checks; all seven
claim checkers; width sweep untouched — the x64-include ratchet
counts `kernel/arch/x86_64/` includes, and this cut removed driver
includes, which ratchet 2 never tracked).

The include-rule claim (stronger than the call ratchet: kernel/fs
may include no driver header at all) earned its keep immediately —
it found THREE pre-existing non-storage couplings the plan never
measured: `drivers/timer/pit.h` in procfs.c and select.c (a time
seam question, some future plan's), `drivers/usb/msc.h` in usbfs.c
(the USB seam question).  All three are pinned per-file in the
checker as named residue; a new driver include anywhere in fs fails
even at the same total.

Failure-path strings ("no AHCI disk available", "no second AHCI
disk") are deliberately UNTOUCHED: the disk-present cases pin their
absence as negative greps, and rewording them in the same commit
that touches the I/O path would blind exactly the tripwires meant
to catch this commit's mistakes.  P2 rewords them when the strings
become lies (on a tenant they would be), together with the greps.

### P2 — ext2 mounted on rv64 — ✅ COMPLETE
- [x] `vblk_rv` registers as blkdev (single-sector backend, the
      seam's count looped arch-side per §6); the adoption set that
      MEASUREMENT picked — `blkdev.c ext2.c kprintf.c spinlock.c` —
      joined KERNELRV_SHARED unchanged (the string.c promotion
      shape).  **Deviation, named: the task list above was written
      from the plan's draft; llvm-nm re-scoped it.**  vfs.c does not
      even COMPILE on this target (a raw x86 `sti` at vfs.c:71 — one
      of the width sweep's 29 allowed asm files — plus the
      scheduler/thread coupling); buffer_cache/devfs/tmpfs/cwd/
      symlink link against vfs.c symbols (vfs_now aside) or serve
      paths no rv64 consumer exists for yet.  ext2 needs none of
      them: measured undefined-symbol surface = blkdev_*, kmalloc/
      kfree, kprintf, vfs_now, string.h — provided by 40 lines of
      arch glue (`fsglue_rv.c`), not forks.
- [x] Boot receipts (all pinned by the smoke): `[blkdev] blk0 =
      vblk0 (virtio-mmio, 8192 sectors)`, ext2.c's own `[ext2]
      mounted existing volume: block_size=1024, groups=1,
      blocks=4096, inodes=1024`, `[rvfs] mounted ext2 on blkdev 0
      (ops-level; VFS waits on P4)` — the draft's `[vfs] mounted /`
      line was vfs.c's to print, and vfs.c stayed home; the honest
      receipt names what actually mounted.  `rv_fs_smoke.sh` (12
      asserts) seeds LINUX.TXT via debugfs with a per-run token and
      the kernel cats it back through `ext2_ops.lookup/read` —
      byte-exact token match.
- [x] Size delta: kernelrv.elf 601 656 → 862 144 (+260 488 for
      kprintf+spinlock+blkdev+ext2, measured).  The pattern-disk
      lane is intact: sector-0 sniff dispatches (parity pattern →
      V7 selftest gate verbatim; anything else → the seam), and
      rv_parity_smoke.sh re-run green after the change.
- [x] The two ext2 strings that would have been lies off-x86
      reworded WITH grep audit ("no AHCI disk available"/"AHCI port
      %d mounted" → "no block device available"/"blkdev %d
      mounted"); x86's test_ext2 pins neither (checked before the
      edit), and diskfs's identical-sounding string stays because
      its case pins the ABSENCE of it (the P1 tripwire argument).

#### P2 result

The chain, from the first boot with the seam in place: vblk 8192
sectors → blk0 registered → shared ext2.c recognises the
host-mkfs'd volume → self-test PASS (write/dir/indirect/rename on
rv64) → `cat LINUX.TXT (25 bytes)` returns the seeded line.
rv_fs_smoke.sh 12/12; rv_parity_smoke.sh still green (pattern lane
untouched); x86 fs cases unaffected by the reword (test_ext2
re-run green).  ext2.c itself: ZERO edits beyond the two strings —
the same object list x86 links, now mounting on a second
architecture.

### P3 — ext2 mounted on a64 — ✅ COMPLETE
- [x] Same seam, fourth consumer: vblk_a64 registers, the identical
      shared set (blkdev.c ext2.c kprintf.c spinlock.c) joined
      KERNELA64_SHARED, `a64_fs_smoke.sh` (12 asserts) mirrors P2's
      proof.  kernela64.elf 308 144 → 408 104 (+99 960 — smaller
      than rv64's +260 488 for the same objects; codegen differs,
      both numbers recorded as measured).
- [x] Deviation budget CLOSED AT ZERO: no portable line changed at
      all.  The entire a64 cost is arch-side — `fsglue_a64.c`
      (fsglue_rv.c's mirror: pl011 sink, kmalloc_a64/kfree_a64,
      vfs_now from cntvct/cntfrq, looped single-sector vblk ops)
      plus the same sector-0 sniff in main_a64.c keeping A7's
      pattern-disk selftest gate verbatim.

#### P3 result

Same receipts, second DTB tenant: blk0 = vblk0 (8192 sectors) →
`[ext2] mounted existing volume` → self-test PASS →
`cat LINUX.TXT (25 bytes)` byte-exact.  a64_fs_smoke.sh 12/12;
a64_parity_smoke.sh and a64_boot_smoke.sh re-run green (pattern
lane intact).  One rig catch on the way, recorded because the rig
bit ITSELF: the smoke was derived from rv_fs_smoke.sh with sed, and
the expression `s/\[rvfs\]/[a64fs]/` — a backslash-escaped
bracket in shell single quotes — parsed as `\` + character class
{r,v,f,s,\}, matched the `\r` inside `tr -d '\r'`, and rewrote it
to `tr -d '[a64fs]'`: a filter deleting the LITERAL characters
a 6 4 f s [ ] from every log line.  The first smoke run failed with
receipts that LOOKED like serial corruption ("conole+hell",
"AurLite") — diagnosed by the character SET being constant across
the whole log, control-run against the untouched drivers smoke, and
a clean `-serial file:` boot.  Derivation by sed is now on the
same list as quoting QEMU output: convenient, and it lies.

Also folded here: CI run 32560908558 (the deployed P1) came back
red on ONE assert — test_sysmon_data pins the /proc/diskstats row
label, and P1's grep audit MISSED it because the audit command
ended in `| head -8` and the sysmon match was line nine.  The case
now pins `blk0` (green locally, 6/6).  A truncated audit is not an
audit; recorded next to the sed lesson.

### P4 — the syscall widening — ✅ COMPLETE
- [x] OPEN/CLOSE/READDIR/STAT/LSEEK on all three ports, 6 → 11
      cases each (checker pins 11 exactly).  Same numbers at every
      width (2/3/4/8/78 — the D4 one-table rule), arch-private
      dispatchers, and ONE new ABI header — `lib/abi/fsabi.h` —
      included by all six trap-boundary files (three libcs, three
      dispatchers; the checker counts 6/6), so the struct layout
      cannot drift because there is exactly one of it.
      Backing stores per port, honest: rv64/a64 read the mounted
      ext2 through fsglue's ops getter (NULL ops → every file
      syscall answers -ENODEV); i386 serves the INITRD read-only
      until P7 mounts ext2 through the seam.  fd 0/1/2 stay the
      console; 3..10 are files; the fd table is dispatcher state
      by the same right as the dispatcher itself is arch code.
- [x] smallsh grew `ls` (open+readdir+close), `cat`
      (open+lseek(END)+rewind+read loop) and `stat` builtins —
      shared source, three builds, and the old help line "absent
      on purpose: ls/cat (VFS port)" is GONE because it stopped
      being true.
- [x] Receipts on all three ports, live shell sessions in the
      smokes: rv_fs_smoke and a64_fs_smoke type ls//stat/cat at
      the prompt (18 asserts each — directory slash, byte-exact
      token, lseek size receipt with the seed length computed
      per-run, not quoted); i386_shell_smoke lists the initrd and
      cats /etc/motd through the fd path (5 new asserts).

#### P4 result

One draft bug caught by the first live run: the smoke asserts
pinned "25 bytes" from a manual seed, but the token embeds PID and
epoch — the seed length VARIES per run.  The asserts now compute
`wc -c` of the seed they just wrote.  A quoted number rots even
when it is one run old.
Regression: rv_parity, a64_drivers, i386_shell full sessions
green; x86_64 kernel untouched and rebuilt; full test-unit green
(25 parity claims; width sweep untouched at 359/69/0/29).

### P5 — SMP rv64 — ✅ COMPLETE
- [x] `sbi.c` gained HSM (EID 0x48534D, hart_start) AND sPI (EID
      0x735049, send_ipi).  Secondary entry `_secondary_start` in
      boot.S: the winner path's three moves minus the lottery (satp
      on, long jump high via `secondary_jump_pool` — which had to
      live in the LOW literal pool next to boot_jump_pool, because
      auipc from a 0x8020xxxx PC cannot span the HHDM gap; the
      first link said so with a relocation error, quoted in the
      result).  Per-hart 8 KiB stacks handed over as HSM's opaque
      argument; the entry PA travels as `kernel_layout[8]` (the
      medany pool rule, third user).
- [x] `rv_smp_smoke.sh` (8 asserts, counted not assumed): `-smp 4`
      → boot hart (id 3 on QEMU virt, NOT 0 — the smoke does not
      assume) starts the other three, `grep -c` finds EXACTLY 3
      report-ins and EXACTLY 3 named ack lines, `online: 3/3`,
      `IPI round-trip: 3/3 ack(s)`.
- [x] Secondaries poll sip.SSIP and park in wfi — deliberately OFF
      the trap path (the trap vector, its stacks and sscratch
      discipline stay single-hart property until a scheduler phase
      claims otherwise; D5 named in smp_rv.c).  Single-hart runs
      print an honest `[smp] nothing to start` — rv_boot, rv_parity
      and rv_fs smokes re-run green.

#### P5 result

First -smp 4 boot: 3/3 online from their own stacks, IPI 3/3, and
the report-in lines are CLEAN under concurrent printing — the P2
adoption of kernel/lib/kprintf.c brought its spinlock along, so the
first three-hart print storm this port ever had was serialized by
code that landed three phases earlier.  One relocation lesson
recorded above (the low pool); zero portable-line changes; the
kernel still shuts down by PSCI at the end of a healthy run, which
makes the smoke sub-second.

### P6 — SMP a64 (+ the x16 amendment) — ✅ COMPLETE
- [x] `psci.c` delivers CPU_ON (0xC4000003 SMC64 over the hvc
      conduit — the function its own A0 comment promised as "D5's
      named exit ramp").  `_secondary_start` in boot.S: the boot
      path's moves minus the header dance — DAIF masked, SPSel=1,
      FPEN open, the SAME early roots into TTBR0/1, SCTLR.M, long
      jump high through `secondary_jump_pool` (placed in the LOW
      pool from the start — the P5 relocation lesson paid forward).
      Entry PA rides `kernel_layout[8]`, stacks ride CPU_ON's
      context argument.
- [x] The IPI receipt is a GICv2 SGI, and secondaries POLL their
      BANKED CPU interface (own ISENABLER0 bit, own GICC_CTLR/PMR,
      claim from IAR with PSTATE.I masked — the interface signals,
      the core never vectors): off the trap path, same D5 shape as
      rv64.  `a64_smp_smoke.sh` (written by hand — the P3 sed
      lesson): -smp 8 → exactly 7 report-ins counted, `online:
      7/7`, `IPI round-trip: 7/7 ack(s)`, 7 named ack lines.
- [x] **The x16 amendment (user request), measured not wished:**
      both tenants now carry code max 16 (SMP_RV_MAX/SMP_A64_MAX);
      rv64 PROVES it — rv_smp_smoke.sh grew a second lane, `-smp
      16` → exactly 15 report-ins, `IPI round-trip: 15/15 ack(s)`.
      On a64 the ceiling is ARCHITECTURAL, not ours: GICv2 has 8
      CPU interfaces and 8 SGIR target bits, and QEMU virt refuses
      `-smp 16` with gic-version=2.  The a64 x16 run needs a GICv3
      driver (affinity-routed SGIs via ICC_SGI1R_EL1) — NAMED
      RESIDUE, carried to P9's list.

#### P6 result

First -smp 8 boot: 7/7 online from their own stacks, SGI acked
7/7 through seven banked interfaces.  First -smp 16 boot on rv64:
15/15 online, IPI 15/15.  Single-core runs on both tenants print
the honest `nothing to start`; a64_boot, a64_parity, a64_drivers,
a64_fs, rv_boot, rv_parity, rv_fs all re-run green.  Zero portable
lines changed; the whole phase is arch-side plus two smokes.

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

## 6. The seam, designed once (amended at P1, deviations named)

    struct blkdev_ops {
        int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
        int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
        uint64_t (*sector_count)(void *ctx);   /* optional */
    };
    int blkdev_register(const char *name, const struct blkdev_ops *ops,
                        void *ctx, uint32_t sector_size);

Two P1 amendments to the draft, both named:

- **Eight slots, not four.**  The x86 boot already mounts SEVEN
  disk-backed filesystems (kernel.c: diskfs, fat32, ext2, exfat,
  ext4, btrfs, f2fs/ntfs on ids 0–6 — counted, not assumed); the
  draft's four would have refused the fifth disk on day one.
- **The ops carry a count.**  AHCI does one DMA for a multi-sector
  read; the draft's per-sector-only shape would have silently split
  ext2's 2-sector superblock read into two DMAs — a performance
  regression smuggled in as a refactor.  Single-sector backends
  (vblk today) loop on their side of the seam.

512-byte sectors only (all three backends present 512 today —
measured at P0, asserted by the checker), and `blkdev_register`
REFUSES any other `sector_size` rather than lying (rc -2; the unit
test pins it).  fs code takes an `int dev` where it used to imply
"the AHCI port"; boot-time mount assignments keep their meaning to
the byte because AHCI registers in detection order.  No partition
table parsing enters this plan — the fs-lba offsets stay where they
are (fat32.c already carries them), and GPT is residue if anyone
asks.
