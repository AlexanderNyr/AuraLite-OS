# AuraLite OS — Residue Ledger Plan (every named leftover, found, classed, scheduled)

## Status: IN PROGRESS — R0 complete; R1 next; plan committed 2026-08-22

| Phase | Result | Deliverable |
|-------|--------|-------------|
| R0 — the rig: the harvester + the machine-checked registry | ✅ complete | `patches/RESIDUE_R0_rig.patch` |
| R1 — the small-payoff cluster (flake remedy, oddity, seam pins) | pending | `patches/RESIDUE_R1_small.patch` |
| R2 — VFS on the ports (the vfs.c:71 unlock) | pending | `patches/RESIDUE_R2_vfs.patch` |
| R3 — i386 TCP/sockets (the last I8 line) | pending | `patches/RESIDUE_R3_tcp32.patch` |
| R4 — GICv3 + the a64 x16 run | pending | `patches/RESIDUE_R4_gicv3.patch` |
| R5 — schedulers: per-CPU runqueues on the tenants, APs beyond idle on x86 | pending | `patches/RESIDUE_R5_sched.patch` |
| R6 — libc v2: mmap/brk + malloc + stdio-lite on the ports | pending | `patches/RESIDUE_R6_libc2.patch` |
| R7 — PCIe ECAM on virt (rv64 + a64), virtio-pci as second transport | pending | `patches/RESIDUE_R7_ecam.patch` |
| R8 — Rust rows: rv64 + a64 editions of rustes/rsbr | pending | `patches/RESIDUE_R8_rust.patch` |
| R9 — the net cluster (SLAAC/dual-stack, TCP-DNS fallback, libahttp port) | pending | `patches/RESIDUE_R9_net.patch` |
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

### R1 — the small-payoff cluster
- [ ] RES-01: UHCI TD wait → guest-time bound (PIT ticks), smoke
      unchanged (the flake remedy, pre-paid).
- [ ] RES-02: reproduce the -cpu max banner oddity with today's
      tree; fix or record the sharper diagnosis.
- [ ] RES-05: give IST1 its gate (double-fault lane) or unfill it —
      whichever the measurement argues.
- [ ] RES-04: partition-table probe that REFUSES loudly (GPT/MBR
      detected → named skip, not silent raw mount).
- [ ] RES-40: attach-GPU repro of the G13 init hang; fix or narrow.
- [ ] RES-35: measure first (i386 rep movsd candidate sites, port
      kheap sizeclass counters, ports' dead-code bytes under
      gc-sections); do what the numbers justify, record what they
      refuse.
- [ ] RES-03: write the time-seam/USB-seam decision notes (fix or
      re-pin with a design owner).

### R2 — VFS on the ports (the vfs.c:71 unlock)
- [ ] The `sti` at vfs.c:71 becomes an arch-irqflags call (the A6
      DAIF/V6 pattern already in the tree); measure what ELSE vfs.c
      pulls (llvm-nm, the P2 method) and provide it via the fsglue
      files — no forks.
- [ ] vfs.c + tmpfs/devfs/cwd/symlink/buffer_cache join the three
      shared lists where the link survives; `[vfs] mounted /` prints
      on rv64/a64 for real (RES-09); i386 shell open() resolves
      through the mounts (RES-08); the P4 fd layers swap their
      ad-hoc resolvers for vfs lookups.
- [ ] Exit: fs smokes gain `mounted /` + shell `cat /ext2/...` on
      all three ports; every existing pin holds.

### R3 — i386 TCP/sockets
- [ ] Measure the gap: net32 speaks the e1000 miniproto; the x86_64
      TCP lives in kernel/net/tcp.c (width-clean? the sweep says —
      run the lane).  Adopt tcp.c + socket.c behind the netdev seam
      or record the measured blocker per file.
- [ ] Exit: an i386 smoke round-trips one TCP payload against the
      QEMU user-net stack, `results` receipts counted.

### R4 — GICv3 + a64 x16
- [ ] GICv3 driver (redistributors + ICC sysregs + affinity SGIs
      via ICC_SGI1R_EL1), selected by DTB compatible; GICv2 stays
      for -smp<=8 boards.
- [ ] Exit: a64_smp_smoke grows the -smp 16 lane (15 report-ins,
      IPI 15/15, gic-version=3); the v2 lane stays.

### R5 — schedulers
- [ ] Tenants: per-hart/core runqueues, timer-tick preemption on
      secondaries, one user thread observed to RUN on a secondary
      (receipt: `pid=N on hart 2`).
- [ ] x86: user threads schedule beyond the BSP (status.md's own
      conservative note), device IRQ wakes a hlt-ed AP (RES-16).
- [ ] Exit: counted receipts on all three SMP-capable ports; every
      existing single-CPU pin holds.

### R6 — libc v2 on the ports
- [ ] brk/mmap-lite syscalls (the D4 number table grows; checker
      pins move), malloc over them, stdio-lite (FILE over fd,
      fprintf/fgets subset), errno-thread-safety NOT claimed (no
      TLS yet — named).
- [ ] Exit: one port program allocates, writes a file through
      stdio, reads it back — three ports, one source.

### R7 — PCIe ECAM on virt
- [ ] ECAM walker on rv64+a64 (the measured `pcie@10000000`),
      virtio-pci as the SECOND transport behind the same virtio
      core, one device (blk) proven over it.
- [ ] Exit: `[pci] ECAM: N functions` + vblk-over-PCI mount lane.

### R8 — Rust rows
- [ ] rustes/rsbr built for `riscv64gc-unknown-none-elf` and
      `aarch64-unknown-none`, run from initrd on both tenants.
- [ ] Exit: the same receipt line the x86_64 edition prints.

### R9 — the net cluster
- [ ] RES-24/25/26/27/28(IRQ-RX): SLAAC, dual-stack resolution,
      TCP-DNS fallback, /apps/http on libahttp, virtio-net IRQ RX.
- [ ] Exit: `ping6` a SLAAC address; one HTTPS-over-IPv6 fetch
      receipt; DNS answers >512B resolve via TCP.

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
