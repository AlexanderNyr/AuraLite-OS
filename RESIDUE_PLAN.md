# AuraLite OS — Residue Ledger Plan (every named leftover, found, classed, scheduled)

## Status: IN PROGRESS — R0–R9 complete; R10 next; plan committed 2026-08-22

| Phase | Result | Deliverable |
|-------|--------|-------------|
| R0 — the rig: the harvester + the machine-checked registry | ✅ complete | `patches/RESIDUE_R0_rig.patch` |
| R1 — the small-payoff cluster (flake remedy, oddity, seam pins) | ✅ complete | `patches/RESIDUE_R1_small.patch` |
| R2 — VFS on the ports (the vfs.c:71 unlock) | ✅ complete | `patches/RESIDUE_R2_vfs.patch` |
| R3 — i386 TCP/sockets (the last I8 line) | ✅ complete | `patches/RESIDUE_R3_tcp32.patch` |
| R4 — GICv3 + the a64 x16 run | ✅ complete | `patches/RESIDUE_R4_gicv3.patch` |
| R5 — schedulers: per-CPU runqueues on the tenants, APs beyond idle on x86 | ✅ complete | `patches/RESIDUE_R5_sched.patch` |
| R6 — libc v2: mmap/brk + malloc + stdio-lite on the ports | ✅ complete | `patches/RESIDUE_R6_libc2.patch` |
| R7 — PCIe ECAM on virt (rv64 + a64), virtio-pci as second transport | ✅ complete | `patches/RESIDUE_R7_ecam.patch` |
| R8 — Rust rows: rv64 + a64 editions of rustes/rsbr | ✅ complete | `patches/RESIDUE_R8_rust.patch` |
| R9 — the net cluster (SLAAC/dual-stack, TCP-DNS fallback, libahttp port) | ✅ complete | `patches/RESIDUE_R9_net.patch` |
| R10 — the crypto width line: 32-bit limbs for atls_fe | pending | `patches/RESIDUE_R10_fe32.patch` |
| R11 — the real-hardware package v2 (user-executable; PCID's D-PCID-5 trigger EXISTS) | pending | `patches/RESIDUE_R11_metal.patch` |
| R12 — close-out: re-affirmed non-goals, the ledger arithmetic | pending | `patches/RESIDUE_R12_close.patch` |

## 1. Where this plan comes from

Every closed plan in this tree ends the same honest way: a residue
list, "recorded not hidden".  Twenty-two plans later the records
live in twenty-two places, plus TODO.md (570 lines), plus the 🚧
rows of docs/status.md — and nothing MACHINE-CHECKS that the sum of
those lists is still true, still complete, or still shrinking.  The
user's instruction for this series: find them ALL, and do them.

"Do them all" needs an honest split first, because the ledger holds
four different kinds of debt:

- **W (work)** — doable under QEMU/TCG today; these get phases.
- **M (metal)** — measurable only on real hardware (PAT/WC wins,
  ERMSB crossover, PCID's D-PCID-5 gate — which the user's own WHPX
  boot log ALREADY satisfied with `pcid=1`); these get a
  user-executable package, not QEMU theatre.
- **N (non-goal)** — decisions, not debt (the W32 registry, MSG_COPY,
  SHM_LOCK...); these get RE-AFFIRMED with their original D-numbers,
  or consciously reversed — never silently dropped.
- **S (sub-series)** — items too large for one phase here (full USB
  transfer scheduling on EHCI/xHCI, Bluetooth/Wi-Fi transports, GUI
  isolation); these get an OPENER FACT measured here and a named
  hand-off, the way OPT §7 opened HW_PLAN.

## 2. The harvest — every named residue, classed (the R0 rig pins this table)

Sources: the 22 *_PLAN.md files (grep markers: residue / non-goal /
deferred / follow-up / follow-on / future work), TODO.md's unchecked
boxes, docs/status.md's 🚧/🔶 rows, tests/posix2024/known_partials.txt,
and the per-file pins inside the claim checkers.  Marker counts at
harvest: OPT 21, ARM64 21, POSIX 15, HW 15, PARITY 13, I386 12,
WIN32 11, USB 11, REALINTERNET 11, RISCV 10, MATURITY 10, INTERNET
10, POSIX2024 4, GL 2, DOOM 2, WEBVIEW 1; TODO.md 27 unchecked
boxes; status.md 16 🚧/🔶 rows.

| ID | Item | Source | Class | Lands in |
|----|------|--------|-------|----------|
| RES-01 | UHCI TD waits are iteration-bounded, not time-bounded (1 observed runner flake) | PARITY §5 | W | R1 |
| RES-02 | `-cpu max` userspace shell-banner oddity (pre-H2 control, recorded) | HW H2 | W | R1 |
| RES-03 | pit.h ×2 + msc.h couplings in kernel/fs (time/USB seam questions, pinned per-file) | PARITY P1 | W | R1 |
| RES-04 | GPT partition refusal (seam mounts raw offsets only, no partition parse) | PARITY §6 | W | R1 |
| RES-05 | TSS IST allocated but unused (ist1 filled, no gate uses it) | status.md | W | R1 |
| RES-06 | vfs.c does not compile off-x86: raw `sti` at vfs.c:71 (+ scheduler coupling) | PARITY P2 | W | R2 |
| RES-07 | buffer_cache/tmpfs/devfs/cwd/symlink adoption on tenants (blocked by RES-06) | PARITY P2 | W | R2 |
| RES-08 | i386 shell fd layer serves initrd only; ext2 reaches the shell with VFS | PARITY P4/P7 | W | R2 |
| RES-09 | Path-level VFS mounts on rv64/a64 (`[vfs] mounted /` — the draft receipt that stayed home) | PARITY P2 | W | R2 |
| RES-10 | i386 TCP/sockets (the struck-through I8 line; net32 is e1000 miniproto only) | I386 I8 | W | R3 |
| RES-11 | i386 compositor/GUI (same struck-through line; VBE is the enabler) | I386 I7/I8 | S | R12 hand-off |
| RES-12 | i386 VBE graphics (text mode is the bring-up console by design) | I386 I7 | S | R12 hand-off |
| RES-13 | a64 -smp 16 needs GICv3 (v2 = 8 CPU interfaces, architectural) | PARITY P6 | W | R4 |
| RES-14 | Tenant SMP is receipts-only: 1 scheduled, secondaries park (D5) | PARITY P5/P6 | W | R5 |
| RES-15 | x86 user scheduling is BSP-only (APs idle-loop; per-CPU runqueues exist) | status.md | W | R5 |
| RES-16 | Device IRQ observed to wake a hlt-ed AP (MATURITY deferral) | MATURITY | W | R5 |
| RES-17 | Full libc on ports: errno/TLS/stdio/malloc-over-mmap (libcmini is the floor) | RISCV V8 / PARITY P8 | W | R6 |
| RES-18 | PIE loading on ports (waits on RES-17) | ARM64 close | W | R6 |
| RES-19 | Dynamic user-space allocation once brk/mmap exist (TODO) | TODO.md | W | R6 |
| RES-20 | PCIe ECAM on virt: rv64 (V-D7) and a64 (A-D7, `pcie@10000000` measured present) | RISCV/ARM64 D7 | W | R7 |
| RES-21 | virtio-blk/scsi/NVMe as modern storage targets (TODO; ECAM unlocks virtio-pci on tenants) | TODO.md | W | R7 |
| RES-22 | Rust row rv64 (`riscv64gc-unknown-none-elf` exists; porting is "a follow-up plan's opening fact") | RISCV V-close | W | R8 |
| RES-23 | Rust row a64 (`aarch64-unknown-none` exists; same class) | ARM64 close | W | R8 |
| RES-24 | IPv6 SLAAC + sockets + dual-stack/happy-eyeballs (X7 first landing recorded them) | REALINTERNET X7 | W | R9 |
| RES-25 | TCP DNS fallback on truncated UDP replies | REALINTERNET X3 | W | R9 |
| RES-26 | HTTPS fetch over IPv6 receipt; dual-stack host reached by whichever family works | INTERNET N8 | W | R9 |
| RES-27 | Port /apps/http onto libahttp (plain HTTP/1.0 client still hand-rolled) | INTERNET | W | R9 |
| RES-28 | virtio-net IRQ-driven RX (polls today); vmxnet3/e1000e data paths | TODO/status | W | R9 (IRQ RX) / S (new NICs) |
| RES-29 | i386 X25519/Ed25519/P-256: atls_fe uses `__int128`; 32-bit limb path required | I386 I9 / status | W | R10 |
| RES-30 | PAT/WC framebuffer win — measured only on metal (H3 shipped correctness half) | OPT §7 / HW H3 | M | R11 |
| RES-31 | PCID + generation scheme; D-PCID-5 re-open gate — **user's WHPX log shows pcid=1: the gate has FIRED** | HW H4 | M→W | R11 |
| RES-32 | ERMSB crossover tuning on real hardware (TCG showed erms=1 only under -cpu max) | HW H2 | M | R11 |
| RES-33 | O3 UART wall-clock claim; O8 ThinLTO entry bar; membench metal numbers (§6 paste-back) | OPT §7 / HW §6 | M | R11 |
| RES-34 | O2 fast-boot knob on i386 (fw_cfg port I/O exists, not wired) + AMEND-5 a64 fw-cfg | OPT §7 / ARM64 AMEND-5 | W | R11 (with the knob receipts) |
| RES-35 | O1 i386 string ops (`rep movsd` transfer); O6 sizeclass for port kheaps; O8 gc-sections on ports | OPT §7 | W | R1 (measure first; do if the numbers say so) |
| RES-36 | MSI/MSI-X for virtio + virtio-gpu (legacy INTx works) | MATURITY | S | R12 hand-off |
| RES-37 | IOAPIC follow-ups: real-hardware base discovery (QEMU-hardcoded today) | MATURITY | M | R11 |
| RES-38 | OHCI ED/TD + EHCI qTD + xHCI rings full transfer scheduling; HID beyond UHCI; BOT short-packet residue line | USB/TODO | S | R12 hand-off (opener facts measured at R12) |
| RES-39 | Bluetooth USB transport; Wi-Fi chipset backend | TODO/status | S | R12 hand-off |
| RES-40 | virtio-gpu init hang found by G13 (bisected pre-G11d, recorded in TODO) | GL G13 / TODO | W | R1 (repro + fix or a sharper record) |
| RES-41 | TGSI backend for VirGL DRAW_VBO (a compiler phase, said G13) | GL G13 | S | R12 hand-off |
| RES-42 | POSIX leftovers: readline, scanf, shell jobs/fg/bg, epoll (P10 list); posix2024 known_partials remaining entries | POSIX/POSIX2024 | W | R12 (triage: each entry re-affirmed or scheduled) |
| RES-43 | W32 deferred surface: W entry points, BitBlt, LoadLibraryW, kernel-side import binding (D-numbered deferrals) | WIN32 | N | R12 re-affirm |
| RES-44 | Non-goals to re-affirm: W32 registry (D8), MSG_COPY/SHM_LOCK/proc-sysvipc, DOOM §6 list, slab single-caller rule | several | N | R12 |
| RES-45 | TODO.md hygiene tail: fsck tooling, block cache writeback, AHCI beyond QEMU, GDB scripts, CI screenshots, spawn-timing flakes | TODO.md | W/S mix | R12 triage, each line dispositioned |
| RES-46 | Skeleton filesystems grow data paths (exFAT/NTFS/ext4/btrfs/f2fs are 🚧 prototypes) | status.md | S | R12 hand-off |
| RES-47 | GUI isolation/permissions, clipboard/focus, persisted settings | TODO.md | S | R12 hand-off |
| RES-48 | HW §6 metal receipts from the user's machine (paste-the-line-back protocol armed, never exercised) | HW §6 | M | R11 |

Class totals: **W 33 · M 5 · N 2 · S 8** — recounted by the R0 rig;
this plan's own draft hand-summed 27/6/3/12 and was WRONG (mixed-
class rows resolved: RES-28/45 count W with their S-parts folded
into RES-46/R12; RES-31 counts W because the user's log fired the
gate).  The R0 checker pins these numbers and the table's living
copy (docs/residue_ledger.md); the harvester's regex supersedes the
draft's wider grep (which also counted "blocked").  A new residue
anywhere that skips the ledger is a checker failure.

## 3. Decisions

### D1. Measured, not assumed (inherited, absolute)
No number, no claim.  Every phase quotes counts; the harvest itself
is re-runnable (`tools/residue_harvest.py`, R0) so the marker counts
in §2 are LIVE, not prose.

### D2. The ledger is append-only and machine-checked
R0 ships the registry as data (`docs/residue_ledger.md` table +
`tools/check_residue_claims.py`).  Closing an item needs its exit
gate green IN THE SAME COMMIT that flips the row; adding debt
anywhere without a ledger row fails CI.  The PARITY registry-
reservation shape, generalised.

### D3. Metal items get a package, not an emulator excuse
R11 collects every M row into one user-executable bundle (scripts +
paste-back protocol v2, extending HW §6).  D-PCID-5 deserves its own
line: the user's WHPX boot log already printed `pcid=1 invpcid=0` —
the H4 re-open gate has FIRED, so PCID implementation (CR3-toggle
fallback, no INVPCID) is scheduled WORK inside R11, with the perf
counters un-pinned from zero the same commit the smoke lane proves
them.

### D4. Non-goals die loudly or live loudly
Every N row is either re-affirmed in R12 with its original decision
number quoted, or reversed into a W row by an explicit amendment.
Silence is the only forbidden disposition.

### D5. Sub-series get opener facts, not promises
Each S row's R12 disposition includes one MEASURED fact gathered in
this series (the OPT-§7-opens-HW_PLAN shape), so the next plan
starts from numbers, not from archaeology.

### D6. Phase hygiene (inherited)
CHANGELOG top entry, plan table update, one patch per phase
including plan+changelog, `git apply` verified on a clean upstream
clone, commit `<name> update`, bundle refresh.  QEMU output to
files only; 200KB fuses; no sed-derived tests; no truncated audits.

## 4. Phases

### R0 — the rig — ✅ COMPLETE
- [x] `tools/residue_harvest.py` (the metric IS the tool: marker
      regex, TODO boxes, status WIP rows; ledger files excluded so
      debt bookkeeping never counts as debt) + baseline snapshot in
      `tools/residue_baseline.txt`.  `tools/check_residue_claims.py`
      (8 claims, selftest, in test-unit): ledger arithmetic (48
      sequential ids), class pins, status sums, and the DEBT
      RATCHET — the harvester runs LIVE and any drift from the
      baseline fails with the exact file and delta named.  Negative
      control run at birth: one planted "deferred" line in a plan
      → FAIL naming `DOOM_PLAN.md ('2','3')`.
- [x] `docs/residue_ledger.md`: all 48 rows, statuses, and a
      one-line EXIT GATE for every W row.
- [x] Two rig catches against this plan's own §2 draft, amended
      same-commit: the hand-summed class totals were WRONG
      (27/6/3/12 → **W 33 · M 5 · N 2 · S 8**, mixed-class rows
      resolved), and the draft's harvest counts came from a wider
      grep (included "blocked") — the harvester's regex supersedes
      them (live counts: OPT 20 not 21, POSIX 14 not 15, TODO 26
      not 27, total 155 marker lines).

#### R0 result

The ledger is now the ONLY place debt lives: 48 rows, 33 of them
executable under QEMU, every one with a gate.  The rig caught the
plan twice before its first phase closed — which is the entire
argument for building rigs first.

### R1 — the small-payoff cluster — ✅ COMPLETE (6 of 7 closed, 1 sharpened)
- [x] RES-01 DONE: the UHCI TD wait is bounded by GUEST TIME (2 s of
      PIT ticks; interrupts are on in that window) with the
      iteration count demoted to a broken-timer backstop.
      test_usb_hub re-run green.
- [x] RES-02 SHARPENED, stays open: reproduced on today's tree —
      the shell PROCESS starts (kernel receipts print) but its
      first SYS_WRITE never reaches serial.  Two suspects
      EXONERATED by A/B boots: qemu64,+erms shows the banner
      (string_fast innocent), max,-x2apic still hides it (x2APIC
      innocent).  Facts recorded in the cpumax smoke's comment.
- [x] RES-05 DONE — the debt was in the DOC: FIX_R1 armed
      `idt_set_ist(8, 1)` long ago, with a live gate
      (test_ist_double_fault.sh) and an A/B unarm knob.  The stale
      status.md TSS row (🚧 "allocated but unused") corrected to ✅ —
      and the harvester's own status-wip ratchet TRIPPED on that
      edit (16→15), proving the debt machinery watches its own
      bookkeeping; baseline moved same-commit per protocol.
- [x] RES-04 DONE: `blkdev_partition_kind()` — pure sniff through
      the ops (GPT at LBA1; MBR only when 0x55AA AND a non-empty
      entry, so a bare boot sector is never called a partition
      table).  Loud IGNORE receipts at all four registration
      sites; 9 new host asserts pin the logic under ASan.
- [x] RES-40 DONE — no longer reproduces: a current
      `-device virtio-gpu-pci` boot answers GET_DISPLAY_INFO
      (`scanout 0: 1280x800`) and reaches the shell; the fix was
      never made for this symptom (the G13/K1 backing era removed
      it).  test_virgl_gpu.sh's ENABLE_FULL_ASSERTS flipped to 1 —
      the case's full battery is green for the first time.  TODO
      entry rewritten as RESOLVED with the trail kept.
- [x] RES-35 DONE, measured-and-refused where the numbers said so:
      **O8 REFUSED** — naive `--gc-sections` on kernelrv "saves"
      606 160 bytes (895 432 → 285 728, −68%) by DELETING live
      boot/trap/pool sections: all five rv smokes red; a KEEP()
      audit of three linker scripts is the real price, recorded in
      the ledger.  **O1 SUPERSEDED** — P7 already links the H1
      word-wide string.c on i386.  **O6 DEFERRED to R6** where port
      allocation actually grows.  Experiment reverted; smokes green
      again.
- [x] RES-03 DONE: `docs/seams.md` — the time seam folds into R6
      (`ktime_ticks()`, both pit.h pins drop there); the msc.h pin
      STAYS as the honest record of an x86-only coupling until
      RES-38's sub-series grows a second USB architecture.

#### R1 result

Six rows moved, one sharpened; two of the six closed by CORRECTING
RECORDS rather than code (RES-05 stale doc, RES-40 stale TODO) —
the harvest's first lesson is that debt lists rot in both
directions, and the ratchet now guards the WIP rows too (it fired
on our own status.md edit within minutes of existing).

### R2 — VFS on the ports (the vfs.c:71 unlock) — ✅ COMPLETE
- [x] **Measurement re-scoped the unlock (the P2 method, again):**
      the `sti` at vfs.c:71 is not a splinter to pull — it sits in
      the PIPE wait, and the whole fd/OFD layer around it is
      honestly coupled to the x86 thread layer (tcb fd tables, wait
      queues, signals, slab).  What IS portable — the mount table,
      longest-prefix matching, lookup across mounts — was split
      VERBATIM into `kernel/fs/vfsmount.c` (the 21st fs file; the
      parity checker's live lanes caught the count within minutes
      and the pin moved 20→21 same-commit).  vfs.c DELEGATES to it:
      one copy of the mount logic, four widths linking it.
- [x] `[vfs] mounted '/'` prints on rv64 AND a64 from SHARED code
      (RES-09 closed); the P4 fd layers swapped their ad-hoc
      resolvers for `vfsm_lookup()` and read through `vn->ops`
      (multi-fs-ready); i386 mounts ext2 at `/ext2`, its shell
      open()/stat()/readdir() resolve absolute paths through the
      mount table with the initrd as the relative-name fallback
      (RES-08 closed): `ls /ext2` and `cat /ext2/LINUX.TXT` typed
      at the live prompt, token byte-exact.
- [x] Exit gates ran: rv_fs 19 asserts, a64_fs 19, i386_fs 16 (with
      the new interactive session), i386_shell green; the x86_64
      fs group 11/11 through the DELEGATED vfs.c (571 s); full
      test-unit green.  RES-06 stays OPEN, honestly narrowed: the
      remaining coupling is the fd half, and its disposition
      belongs to a scheduler-aware phase, not to a mount phase.

### R3 — i386 TCP/sockets — ✅ COMPLETE
- [x] The measurement: the ENTIRE kernel/net tree is already
      -m32-clean (0 errors, all ten files), and llvm-nm showed
      tcp.c needs no scheduler at all — only the netdev seam, four
      net.c helpers, string/kprintf and a tick source.  netdev.c
      itself needs kprintf+memset: a pure seam.  So tcp.c and
      netdev.c joined KERNEL32_SHARED UNCHANGED; `netglue32.c`
      provides the surface (net32 ring wrappers behind `struct
      netdev`, gateway-only ARP — a default-route bring-up SAYS so,
      passthrough ipfrag_step with the X4 absence named, pit32
      ticks).  socket.c stays home: it needs sched_current, and the
      fd story belongs with RES-06's narrowed remainder.
- [x] **Two byte-order catches by packet capture** (filter-dump +
      a hand checksum walk): the first SYN went to 2.2.0.10 (dst
      swapped), the second went FROM 15.2.0.10 (src swapped — the
      capture showed a checksum-OK SYN with an impossible source).
      net32 speaks network order, tcp.c host order; one swap32 at
      the glue boundary, both directions.
- [x] Exit ran: `[tcp] [h=0] ESTABLISHED` + `[tcp32] PASS:
      round-trip 15 byte(s): HELLO-FROM-HOST` against a REAL host
      listener behind SLIRP's 10.0.2.2 (socat).  i386_parity_smoke
      carries the lane (with an honest-skip branch when no
      listener exists — i386_shell/i386_fs boots print the skip and
      stay green).  kernel32.elf 276 712 → 335 288.  The last
      struck-through I8 line — TCP — is paid; the compositor line
      remains with RES-11/12.
- [x] Folded in: CI run 32579828982 (deployed R1) came back red on
      ONE a64_smp assert — core 5's ack LINE lost to the PSCI
      power-off on a loaded runner while its ack COUNTER landed
      (log shows 6 ack lines + "7/7").  Deterministic fix on both
      tenants: the ack line prints BEFORE the counter ticks, so the
      boot CPU's N/N summary now implies every per-CPU line already
      drained through kprintf's lock.  Both smp smokes re-run
      green.

### R4 — GICv3 + a64 x16 — ✅ COMPLETE
- [x] gic.c grew the v3 lane: redistributor walk by GICR_TYPER
      affinity, WAKER wake, ICC sysregs by S-encoding (SRE/PMR/
      IGRPEN1/IAR1/EOIR1 under -mgeneral-regs-only), GICD ARE+G1NS,
      IROUTER written 0 explicitly (never trusting reset), SGI/PPI
      config in the PE's OWN redistributor frame.  smp_a64:
      affinity SGIs via ICC_SGI1R_EL1 per aff1 cluster; secondaries
      poll their own GICR_ISPENDR0 — off the trap path AND off the
      CPU interface (D5 unchanged).
- [x] **Three catches, each by a red run, each named:** (1) the
      draft detected v3 by READING GICD_PIDR2 — and hung every v2
      boot, because QEMU's v2 distributor is 4 KiB and +0xFFE8 is
      UNASSIGNED space; detection moved to the DTB compatible
      (fdt_platform_t.gic_is_v3), where it belonged.  (2) the first
      v3 IPI scored 0/15: ICC_SGI1R generates GROUP-1 SGIs and
      reset IGROUPR0 marks SGI0 group 0 — a group-mismatched SGI is
      DISCARDED; secondaries now claim their SGI into group 1
      before listening.  (3) the smoke's first x16 run scored
      10/15: the bringup's iteration-bounded waits were vCPU-speed
      bets — the R1 UHCI lesson re-learned; both tenants' waits now
      run on GUEST TIME (CNTVCT/rdtime deadlines).
- [x] Exit ran: a64_smp_smoke carries BOTH lanes — v2 -smp 8
      (7/7+7/7) and GICv3 -smp 16 (15 report-ins counted, IPI
      15/15).  Full a64 battery + rv_smp re-run green (one stale-
      IMG scare on the way: the .img lags the .elf unless rebuilt).
      The last ARCHITECTURAL ceiling of the x16 request is lifted;
      docs/status.md's living matrix updated.

### R5 — schedulers — ✅ COMPLETE (scoped by measurement, remainder named)
- [x] **x86 first, because the measurement flipped the task:** the
      "BSP-only user scheduling" status row was the THIRD stale doc
      line this series has caught — scheduler_rq.c has carried
      per-CPU queues, least-loaded placement and work stealing
      since SMP 3.2, with its own comment saying threads on AP
      queues "actually execute there".  D1 demanded a receipt, not
      a doc edit: the scheduler now prints ONCE when an AP first
      runs a user thread — `[sched] R5 receipt: user thread pid=6
      on AP cpu=1` on a -smp 4 boot — and test_fpu_smp pins it
      (RES-15 closed with evidence, the row corrected).
- [x] Tenants: user code RUNS OFF the boot CPU on both — a strictly
      serialized one-job mailbox hands `init` to a parked
      secondary, which adopts the final translation roots
      (satp / TTBR0+TTBR1 published by the paging layer), installs
      its own trap vector (stvec/VBAR, no timer, no interrupt
      unmask — exceptions only), and runs it to exit:
      `[smp] init ran at U-mode ON HART 3` / `at EL0 ON CORE 2`,
      with the user program's own lines printed from that CPU.
      user_rv/user_a64 statics stay single-entrant BY CONSTRUCTION
      (the boot CPU waits) — receipts, not a scheduler (RES-14's
      gate met; preemptive tenant runqueues named to the hand-off).
- [x] Three catches on the way, each by a counted red run: QEMU
      ignores `-initrd` for a64 ELF payloads (the A1 fact bit the
      smoke; the v2 lane boots the IMG now); wfe-parked v3 pollers
      lost the SGI wakeup at 13/15 (sev landed before two
      redistributors latched); the yield-spin "fix" made it WORSE
      at 10/15 (fifteen spinning vCPUs starve stragglers on a
      loaded host) — the shape that holds is wfe sleepers + the
      boot core RE-SEVING every wait pass, 15/15 again.  The
      deployed R4's CI run confirmed the diagnosis independently:
      2/15 on the shared runner, same race, fix already riding
      in this patch.
- [x] RES-16 stays OPEN, honestly: a DEVICE interrupt waking a
      hlt-ed AP needs the IOAPIC AP-routing increment — its own
      work, not this phase's afterthought.  Exit gates ran:
      rv_smp (+2 asserts), a64_smp (+2), test_fpu_smp (+1), and
      the single-CPU boots hold every old pin.

### R6 — libc v2 on the ports — ✅ COMPLETE
- [x] `brk` (number 12, the D4 table; checker pins moved 11→12
      same-commit) on all three ports: a fixed [0x20000000, +1 MiB)
      window demand-mapped U+RW, query-by-zero, shrink keeps pages
      (floor, stated).  The WRITE syscall grew the file lane
      (fd≥3 → vn->ops->write; the i386 initrd flavor answers an
      honest -EROFS), and OPEN grew O_CREAT-lite (flags bit 0 →
      the new `vfsm_create()`, ops->create through the mounts).
- [x] libcmini v2: K&R first-fit malloc/free over brk (no threads,
      no trim — the floor says so), stdio-lite (FILE=fd+err, four
      slots; fopen "r"/"w"-creates, fread/fwrite/fgets/fclose; no
      O_TRUNC yet — named, fsio sizes its data accordingly).
      errno-thread-safety NOT claimed (no TLS — named).
- [x] The ktime seam (RES-03's decision executed): `ktime_ticks()/
      ktime_hz()` in kernel/time.h, x86 bodies forward to the PIT;
      procfs.c and select.c dropped their driver includes — the
      parity checker's per-file pit pins DROPPED in this commit and
      kernel/fs carries exactly ONE non-storage driver include
      (msc.h, still honestly pinned).
- [x] Exit ran on ALL THREE ports, one source: `fsio` (initrd,
      three builds) mallocs, fopen("w")-creates /R6IO.TXT on the
      mounted ext2 (path fallback /ext2 on i386), fwrites 48 bytes,
      freads them back, byte-compares — `fsio: PASS malloc+stdio
      round-trip (48 bytes)` printed by the SAME line on rv64, a64
      and i386, asserted in all three fs smokes.  RES-17 and RES-19
      closed; RES-18 (PIE) stays OPEN — relocation is real work,
      not a floor.

### R7 — PCIe ECAM on virt — ✅ COMPLETE
- [x] ECAM walker on rv64+a64 (the measured `pcie@10000000`),
      virtio-pci as the SECOND transport behind the same virtio
      core, one device (blk) proven over it.
- [x] Exit: `[pci] ECAM: N functions` + vblk-over-PCI mount lane.

Result: fdt.c learned `pci-host-ecam-generic` (reg = ECAM window
WITH size; `ranges` decoded with the node's own 3/2 cells against
the parent's — the 32-bit non-prefetchable entry is where BARs go).
Measured, not assumed: rv64 ECAM 0x30000000 (+256 MiB), window
0x40000000; a64 ECAM 0x40_10000000 — ABOVE 4 GiB, where the HHDM
formula wraps out of the 39-bit window, so the a64 tenant maps bus 0
at a VA CARVE (HHDM+0x20000000, a hole by construction) while rv64's
full-4G HHDM just points.  Two new SHARED portable files (both
tenants' Makefile lists, zero width-casts, zero inline asm — the
sweep stayed 355/69/29): `kernel/drivers/pci_ecam.c` (config
accessors, bus-0 walk with the Fact 5.2 attribute gate, BAR
placement — `-kernel` boots arrive with BARs all-zero and decode
off, so placement is ours, bump-cursor over the DTB window) and
`kernel/drivers/virtio_pci.c` (MODERN transport: vendor-capability
walk, VERSION_1 REQUIRED and acked — accept-none is a legacy-mmio
habit a modern device refuses; same vring structs, same three-chain
blk request, same vmmio_arch_ops seam).  vblk on both tenants falls
through to PCI when the mmio windows are empty; fsglue's blkdev line
now prints the transport TRUTH (`vblk0 (virtio-pci, ...)`).  Exit
receipts on both: `[pci] ECAM: 2/3 function(s)` (host bridge 1b36:
0008 + 1af4:1001), `virtio-blk over PCI (modern, VERSION_1): 8192
sectors`, ext2 mounted + self-test PASS + token cat — new PCI lanes
in rv_fs/a64_fs smokes.  Deviation, named: the FIRST draft walked
ECAM before the mmio probes and the a64 legacy vring's contiguity
check refused — the walk's ~260 page-table frames moved the PMM
cursor onto the initrd hole.  The walk now runs AFTER the mmio
probes (lazily inside the vblk fallback when the PCI lane needs it
first; idempotent).  RIDERS (the R5/R6 CI reds, both dissected from
the runner logs): (1) smp_a64.c claims the SGI into group 1 BEFORE
counting itself online — a group-0 SGI is DISCARDED, not pended, so
a late claimer lost its only shot (14/15 on the R5 run — core 9's
online line printed after the boot core's count; 7/15 on R6 — the
race is load-sensitive; v2 never flaked because v2 SGIs latch
pending); (2) gic_enable's v3 branch now claims IGROUPR too — the
same R4 lesson unapplied HERE is why the GICv3 lane's timer read
`0 ticks in half a second` since R4: PPI 30 sat in group 0 under an
IGRPEN1-only interface.  After the rider: `[timer] PASS: 16 ticks
... INTID 27` and `[gic] PASS: claim/complete` on the v3 lane, both
now asserted (plus the banner honestly names v3; 15/15 held over
three consecutive local -smp 16 runs).  RES-20 and RES-21 closed.
kernelrv.elf 977936, kernela64.elf 462016.

### R8 — Rust rows — ✅ COMPLETE
- [x] rustes/rsbr built for `riscv64gc-unknown-none-elf` and
      `aarch64-unknown-none`, run from initrd on both tenants.
- [x] Exit: the same receipt line the x86_64 edition prints.

Result: ONE bridge, THREE ISAs — lib/rsbr/common.rs grew cfg
siblings of the x86_64 syscall shims (ecall a7 / svc #0 x8, the SAME
D4 numbers the C shims share) and rustes.rs a cfg'd cycle counter
(rdtsc / rdtime / cntvct_el0); the x86_64 blocks are the original
row untouched and every receipt string is shared text, so `=== Rust
Benchmark ===` … `Sum: 499999500000` … `Benchmark complete!` print
IDENTICALLY on all three — asserted (for the first time anywhere:
no test had ever pinned even the x86 row) in the rv_fs and a64_fs
smokes.  The tenants link no C archive, so the compiler's implicit
mem-intrinsics land in a cfg'd memset/memcpy/memcmp module (OFF on
x86_64, which keeps taking them from the user libc — duplicate
symbols refused by construction).  Link recipe = the fsio recipe:
rustc --emit obj + the tenant's own user layout script, _start from
the rlib, entries `/binrv/rustes` and `/bina64/rustes` in the one
four-tenant initrd.  MEASURED GATES, the phase's real kernel work:
U-mode/EL0 counter reads TRAP at both tenants' reset state —
scounteren (CY|TM|IR) and CNTKCTL_EL1 (EL0PCTEN|EL0VCTEN) are now
opened in trap_init AND trap_init_secondary (R5's lesson: init runs
ON a secondary; a boot-hart-only gate would have been a flake
factory).  CI: all six jobs build the one initrd, so all six
install both tenant targets.  status.md's two 🚧 Rust rows flipped
✅ — the harvest ratchet CLICKED (status-wip 15→13, baseline moved
in this same commit).  RES-22 and RES-23 closed.

### R9 — the net cluster — ✅ COMPLETE
- [x] RES-24/25/27/28(IRQ-RX): SLAAC, TCP-DNS fallback, /apps/http
      on libahttp, virtio-net IRQ RX.  RES-26 (HTTPS-over-IPv6)
      stays OPEN, NARROWED — deviation named below.
- [x] Exit: `ping6` a SLAAC address (fec0::2 answered end-to-end,
      CI-pinned); DNS answers >512B resolve via TCP (664-byte
      answer, real wire, CI-pinned).  The HTTPS-over-IPv6 receipt
      moves with RES-26: its ONE remaining blocker is the TCP
      layer (v4-wired conn state + ARP + inline IPv4 headers),
      not the v6 substrate — which this phase made real.

Result: the phase's biggest catch is HISTORICAL — X7's "QEMU SLIRP
filtering limitation (Launchpad #1724590), peer echo is a manual
run" was a LEGEND.  pcap -v named FIVE of our own bugs: (1) RS and
NS declared icmp_len 4 bytes long (header double-counted), so every
solicitation left with a checksum computed over uninitialised tail
bytes; (2) checksums were STORED byte-swapped (host-order helper,
struct store — the R3 byte-order class, ICMPv6 edition; the echo
paths were self-consistent so nothing internal ever noticed); (3)
the NA-wait read the target at ICMP+12 instead of +8, so a real NA
could never match; (4) the RA-wait parsed options from ICMP+8
instead of +16 (the fixed part is 16 bytes) AND demanded a unicast
dst while solicited RAs arrive on ff02::1; (5) the echo validator
checksummed the message WITH its checksum and compared against the
field — a coin that always said drop, invisible because dropped
and answered both count as consumed.  With NDP real: RA→SLAAC
(fec0::/64 + EUI-64), router learned, an NS→NA responder added
(without it no peer can DELIVER anything — SLIRP's reply died
resolving us), off-link routing + RFC 6724-floor source selection,
and `ping6 fec0::2` answers end-to-end in CI.  Fallout catches:
the DHCP builders never wrote tos/flags_frag (stack garbage,
blessed by a checksum computed over it — the R9 stack reshape
exposed it; measured as a SLIRP drop, fixed at both sites), and
the e1000 lane needed MAX_RTR_SOLICITATIONS retries (RS #1 races
the NIC bring-up; measured, RS #2 always lands) plus MPE+MTA
opened for 33:33 multicast.  RES-25: RFC 1035 s4.2.2 TCP retry
against the same server, driven by the NAMED one-shot knob
(DNSCTL_FORCE_TC → `dnstc`); the TCP wire and the 664-byte answer
are real (guestfwd fixture, no root-bound port 53) — test_dns_tcp,
7/7.  RES-27 was the FOURTH stale doc row (http.c has been
libahttp since X2/X6).  RES-28 was HALF-stale: ISR + wake existed,
nothing ever slept — timed waits now wq_wait_deadline and the
`RX via IRQ wake` receipt is pinned.  New case test_dns_tcp
(registry 129→130, net shard); ipv6_ping6 grew the SLAAC lane;
virtio_net pins the IRQ receipt.

### R10 — the crypto width line
- [ ] 32-bit limb path for atls_fe (X25519/Ed25519/P-256 at -m32),
      Wycheproof + RFC vectors at 32-bit width, the i386 crypto row
      flips 🧪→✅.

### R11 — the real-hardware package v2 (+ PCID, which stopped being metal-only)
- [ ] PCID: D-PCID-1..5 implemented (CR3-toggle fallback, no
      INVPCID on the user's machine), counters un-pinned from zero,
      a WHPX-targeted receipt block for the user to paste back.
- [ ] The metal bundle: PAT/WC fps probe, ERMSB crossover sweep,
      membench, O3 wall-clock, IOAPIC base discovery — one script,
      one paste-back section per HW §6 v2.  fw_cfg knobs (RES-34)
      wired so the metal runs can toggle fast-boot.
- [ ] Exit: the package exists and runs under QEMU as a NULL test;
      metal numbers arrive when the user runs it (recorded as
      pending-user, not failure).

### R12 — close-out
- [ ] Every N row re-affirmed with its D-number; every S row handed
      off with its opener fact measured; TODO.md rewritten to point
      at the ledger instead of duplicating it; POSIX partial-list
      triaged entry by entry.
- [ ] Terminal arithmetic; the ledger's class totals at close
      quoted against the harvest totals at open.

## 5. Terminal arithmetic — filled at close

(Counts land here at R12; the checker enforces the table above
against patches/ and the ledger against the tree.)
