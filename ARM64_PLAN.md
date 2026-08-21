# AuraLite OS — ARM (aarch64 / ARMv8-A) Support Plan

## Status: COMPLETE ✅ — A0–A9 landed (phases A0–A9; A5 split into a/b/c); closed 2026-08-21

| Phase | Result | Deliverable |
|-------|--------|-------------|
| A0 — toolchain gates + the EL1 stub | ✅ complete | `patches/A64_A0_boot.patch` |
| A1 — `boot_info_t` from the Device Tree, walker promoted | ✅ complete | `patches/A64_A1_dtb.patch` |
| A2 — exceptions, the generic timer, GICv2 | ✅ complete | `patches/A64_A2_traps.patch` |
| A3 — memory: TTBR1 39-bit VA, PMM, heap — W^X twice over | ✅ complete | `patches/A64_A3_mm.patch` |
| A4 — threads, scheduler, EL0, `svc` | ✅ complete | `patches/A64_A4_proc.patch` |
| A5a — the Image exit ramp + shared strings | ✅ complete | `patches/A64_A5a_image.patch` |
| A5b — libca64 + the fourth tenant | ✅ complete | `patches/A64_A5b_tenant.patch` |
| A5c — ELF loading, EL0 shell, cross-refusals | ✅ complete | `patches/A64_A5c_shell.patch` |
| A6 — the sweep, fourth backend: DAIF behind the contracts | ✅ complete | `patches/A64_A6_sweep.patch` |
| A7 — drivers: virtio-mmio (shared transport), blk, net, PL011 RX | ✅ complete | `patches/A64_A7_drivers.patch` |
| A8 — parity: storage, network, full crypto, fourth tenant | ✅ complete | `patches/A64_A8_parity.patch` |
| A9 — CI matrix, docs, the claim check | ✅ complete | `patches/A64_A9_ci.patch` |

> **Amended 2026-08-20 (post-OPT audit).**  Between A4 and A5 the tree
> moved ten phases under this plan's feet: `OPT_PLAN.md` O0–O9 landed
> (measuring rig, shared word-wide string ops, the self-test knob, the
> UART TX ring core, precise TLB shootdown, size-class cache, blocking
> waits, linker GC).  A0–A4's results were re-verified against today's
> tree before amending (checker 47/47, full A4 gauntlet green, all four
> kernels build).  §1.5 lists what the OPT series changed that A5–A9
> can now lean on — including one A7 trap this audit caught before the
> phase could step on it.

This document answers:

> *AuraLite now boots three kernels — two x86 widths and one RISC-V —
> from shared contracts. The RISC-V port already paid for the expensive
> abstractions: the irqflags contract family, the DTB walker, the
> virtio-mmio transport, the multi-tenant initrd audit. What does a
> fourth architecture cost when the machinery for "different ISA, same
> contracts" already exists — and which parts of that machinery turn out
> to be secretly riscv-shaped when aarch64 leans on them?*

It follows the structure of the existing plans (`I386_PLAN.md`,
`RISCV_PLAN.md`, `FIXES_PLAN.md`, `USB_PLAN.md`): dependency-ordered
phases, a definition of done and a test gate for every phase, one
`.patch` per phase.

**Baseline:** the current `main` tree, on top of the completed
`RISCV_PLAN.md`. This plan **inherits** decisions where they transfer —
D4 one-syscall-table (now four trap mechanisms), D6 typing rules and the
four irqflags contracts, D7 ship-the-first-thing-alone, D8
claim-checker-from-birth — and says so per phase instead of re-arguing
them.

`RISCV_PLAN.md` §7-equivalent framing said the rv64 port was "the audit
the i386 port could not perform". This plan is the audit the *RISC-V*
port could not perform: the one that tests whether the abstractions
built during V0–V9 are actually architecture-neutral or merely
two-architecture-neutral. A contract that two parties honour may still
be a bilateral treaty; the third signatory is where it becomes law.

---

## 1. Where things actually stand

Everything in this section was measured on this tree's build
environment before a line was planned — a minimal EL1 stub was
assembled, linked, and booted under `qemu-system-aarch64` during
fact-finding, and every claim below cites what the machine printed.

### Fact 1 — The toolchain is already here, again, Rust included

```
$ clang --target=aarch64-unknown-none-elf -c boot.S     # compiles
$ ld.lld -m aarch64linux -T stub.ld boot.o -o stub.elf  # links
$ rustup target list | grep aarch64-unknown-none
aarch64-unknown-none
aarch64-unknown-none-softfloat
```

The same clang/lld pair that builds the other three kernels emits
aarch64 with a `--target` flag and an emulation name. `REQUIRED_TOOLS`
does not grow. Assembly is GNU as via clang (no NASM on this arch —
same as riscv64). The optional-tools pattern V0 established
(qemu absent = targets skip loudly) transfers verbatim:
`qemu-system-aarch64` and `gcc-aarch64-linux-gnu` are the A-plan's
optional pair.

One apt fact re-measured rather than assumed: `gcc-14-aarch64-linux-gnu`
declares `Conflicts: gcc-multilib` — the same solver conflict the
riscv64 cross compiler has. The V9 rule (each width's toolchain in its
own CI job; the conflict never meets) applies unchanged, and A9's
dependency list must also name `libc6-dev-arm64-cross`: with
`--no-install-recommends`, the bare cross-gcc arrives without a libc
and the crypto gate's `-static` link fails on `bits/libc-header-start.h`
(measured — the first cross-compile attempt on this machine did exactly
that).

### Fact 2 — The stub booted, and it reported the world it landed in

```
A64-STUB EL=1 DTB=0x0000000000000000 CNTFRQ=0x03b9aca0
```

Three measured facts in one banner line, each of which shapes a phase:

1. **QEMU's `virt` machine enters an ELF `-kernel` at EL1** (EL2 only
   with `-machine virt,virtualization=on` — also measured, prints
   `EL=2`). There is no EL3, no secure world, no vendor BL31 to chain.
   The kernel is born in exactly the privilege level it wants to run
   in.
2. **`x0` is NOT the DTB pointer for ELF payloads.** The stub read
   `x0 = 0`. The Linux `Image` boot protocol's "x0 = DTB phys" promise
   applies to raw images, not ELF. Instead the DTB is parked at the
   **base of RAM**: a second probe read `0x40000000` and found
   `0xedfe0dd0` — the FDT magic, little-endian view of big-endian
   `0xd00dfeed`. Verified stable with and without `-initrd`. A1 reads
   the DTB from the RAM base and validates the magic; a kernel that
   trusted `x0` here would deref NULL on its first day.
3. **`CNTFRQ_EL0 = 0x03b9aca0` = 62.5 MHz.** The generic timer's
   frequency is architecturally discoverable from a register — no DTB
   `timebase-frequency` property, no PIT calibration. That is one
   fewer boot_info field to plumb than riscv64 needed.

And the output path itself is a fourth fact: the banner was printed
through the **PL011 at `0x9000000` with zero initialisation** — QEMU's
reset state has the UART enabled, exactly like the ns16550a on the
riscv `virt` board. Day-0 console costs one `str` per byte.

### Fact 3 — The `virt` machine's device world, from its own mouth

`qemu-system-aarch64 -machine virt,dumpdtb=` decompiled (393 lines):

| Node | What it is | Address / IRQ |
|------|-----------|----------------|
| `pl011@9000000` | ARM PrimeCell UART | `0x09000000`, SPI 1 level |
| `intc@8000000` | **GICv2** (`arm,cortex-a15-gic`) | GICD `0x8000000`, GICC `0x8010000` |
| `timer` | armv8 generic timer | PPIs 13/14/11/10, `always-on` |
| `virtio_mmio@a000000…` | **32** virtio-mmio windows | `0xa000000`+, `0x200` stride, SPI 16–47 |
| `pcie@10000000` | PCIe ECAM (`pci-host-ecam-generic`) | present, deferred (D7) |
| `psci` | PSCI 1.0, **method = "hvc"** | `CPU_ON`=0xc4000003 &c. |
| `memory@40000000` | RAM base | `0x40000000` |
| `pl031` / `pl061` / `fw-cfg` / `cfi-flash` | RTC, GPIO, fw_cfg, flash | ignored this plan |

No PS/2, no VGA, no PIT, no i8259, no port I/O — the same clean sweep
riscv64 saw, so every compile-fence V6/V7 installed already guards the
right files. What is *new* relative to riscv64: the interrupt
controller is a **GICv2** (banked distributor + CPU interface, INTID
spaces: SGI 0–15, PPI 16–31, SPI 32+), not a PLIC — a genuinely
different programming model and A2's main cost; and the DTB encodes
interrupts as 3-cell `<type nr flags>` where virtio's `SPI 16` means
**INTID 48** — an off-by-32 that the A1 walker must normalise once,
centrally, so no driver ever adds 32 itself.

### Fact 4 — Power and SMP go through PSCI, and it already works

```
PSCI SYSTEM_OFF via hvc: (QEMU exits 0, immediately)
```

The stub issued `x0 = 0x84000008; hvc #0` and the machine powered off.
PSCI is this platform's SBI: firmware-shaped services reached by a
trap-to-higher-agent instruction (`hvc`, per the DTB's
`method = "hvc"`). The mapping from the riscv64 port is exact —
`sbi_shutdown` ↔ `PSCI SYSTEM_OFF`, SBI HSM `hart_start` ↔ `PSCI
CPU_ON` — and A0 uses it the same way V0 did: every smoke run ends by
power-off, not timeout. One asymmetry worth naming: unlike OpenSBI,
PSCI is *not* in the boot path. Nothing runs before `_start`; there is
no "Domain0 Next Address" indirection, no payload-base-vs-e_entry trap
to re-learn (the `.text.boot`-first linker discipline V0 bought stays
anyway — it is free and it makes "first byte" and "`_start`" the same
fact on all four kernels).

### Fact 5 — The contracts survive; the costs move again, twice

**What transfers free or nearly free:**

- **LP64, little-endian** — no I6 width battle, no byte-order battle.
- **`__int128` exists** — the full crypto suite is compilable AND
  executable: a static aarch64 test binary ran under `qemu-aarch64`
  user-mode emulation on this machine during fact-finding
  (`a64 exec OK, int128 hi=1000000000`). The A8 gate is the rv64
  gate's shape with `EM_AARCH64 = 183` (also measured, `od` on the
  ELF header).
- **The four irqflags contracts** — `arch_irq_save/restore`,
  `arch_wait_for_interrupt`, `arch_cpu_relax` map to DAIF ops
  (`mrs/msr daifset/daifclr`), `wfi`, `yield`. The V6 sweep left the
  portable files speaking only these four verbs plus C11 atomics
  (ratchet 4, baseline 29 and falling); aarch64 is backend number
  four, ~40 lines, no portable file changes.
- **W^X comes back stronger** — aarch64 PTEs carry *two* execute-never
  bits (`UXN`/`PXN`), so the elfperm gates return with a bonus: kernel
  pages can be PXN-mapped while user pages are UXN-mapped, and a
  user-page-executed-in-kernel bug becomes a fault instead of a
  postmortem.
- **Rust** — `aarch64-unknown-none` exists; the stretch recorded for
  rv64 applies here symmetrically.

**Where the new costs are (the honest list):**

1. **Alignment before the MMU.** With the MMU off, aarch64 treats all
   memory as Device-nGnRnE, and *unaligned accesses fault*. Every line
   of C that runs before A3 turns paging on must be compiled
   `-mstrict-align`; the flag comes off (for the kernel proper) only
   when `SCTLR_EL1.M` is set. Getting this wrong looks like a random
   early-boot exception with no console. It is named here so it is a
   task, not a debugging session.
2. **Memory attributes are load-bearing.** riscv64's virtio-mmio
   worked through vanilla PTEs; on aarch64 the MMIO windows must be
   mapped Device-nGnRE via MAIR indices, or the CPU may reorder and
   combine device writes. A3 owns the MAIR layout (one Normal WB
   index, one Device index) and A7's transport asserts it.
3. **Barriers are explicit.** `dsb`/`isb`/`dmb` discipline around page
   table writes (`TLBI` + `dsb ish` + `isb`) and around virtio ring
   publishing. The C11 atomics the V6 sweep installed emit the right
   `dmb`s for the portable code; the arch code must do its own.
4. **The GICv2.** A real driver (distributor + CPU interface,
   enable/priority/EOI flow, the INTID+32 normalisation) — smaller
   than the PLIC driver in registers touched, different in every
   detail.
5. **The vector table is a table of code, not pointers.** `VBAR_EL1`
   points at 16 slots of 128 bytes each (4 exception kinds × 4
   origins). A2 writes it in assembly with the same
   one-assembly-entry-path discipline trapentry.S established.
6. **FPU state is bigger.** 32 × 128-bit `q` registers + `fpcr/fpsr`.
   The M1 eager-save lesson transfers (it is already a lineage comment
   in the rv64 context switch); the frame grows to 528 bytes.

### Fact 6 — The reuse dividend, and where it is riscv-shaped

Two files built during the RISC-V port are *almost* portable already,
and this plan's A1/A7 promote them rather than fork them:

- `kernel/arch/riscv64/fdt.c` (418 lines) — the DTB walker. Grep finds
  7 riscv-specific references, all includes/HHDM plumbing, none
  structural: the walker itself is big-endian-safe portable C. A1
  moves it to `kernel/dt/fdt.c` behind a tiny arch-provided
  phys-to-virt hook, and the riscv64 kernel starts consuming the
  shared copy in the same patch (the claim check asserts both kernels
  link the same object).
- `kernel/arch/riscv64/virtio_mmio.c` (201 lines) — the legacy-MMIO
  transport. Its only arch dependencies are the paging/PMM includes
  and the IRQ claim path. A7 promotes it to `kernel/drivers/` with the
  same hook treatment, and the rv64 kernel switches to the shared
  copy in-patch.

This is the plan's thesis made concrete: the fourth architecture should
cost *drivers and CPU bring-up*, not *another copy of everything*. If
promotion turns out harder than forking — if the walker or the
transport is secretly riscv-shaped in some way grep cannot see — the
phase result records that honestly and forks with a named reason.

### §1.5 — Post-OPT audit (2026-08-20): what moved under the plan's feet

Everything below was verified against the tree, not assumed; each item
names the phase it amends.

**AMEND-1 (A7, mandatory).  The promotion path `kernel/drivers/` walks
into the A0 trap.**  A7 plans to promote `virtio_mmio.c` to a new
`kernel/drivers/` directory — and the x86_64 kernel's source list is
`find kernel drivers ...` with an exclusion list that will not know the
new directory (measured: Makefile line 85; `kernel/dt/*` had to be
excluded for exactly this reason in A1).  A0's Result already named
this find(1) as "the one place where adding a directory IS editing
shared build logic"; this time the trap is predicted instead of
stepped on.  A7's task list now carries the exclusion edit explicitly,
and its gate keeps the A0 rule: prove it by building all four kernels.

**AMEND-2 (A5/A6).  The shared string ops the OPT series left at the
door.**  OPT O1 rebuilt `kernel/lib/string.c` so the portable bodies
(memcpy/memset/memmove, word-wide memcmp/strlen) compile under
`#ifndef ARCH_X86_64` — written for exactly this consumer, recorded in
OPT §7 as rv64/a64 residue.  Neither the rv64 nor the a64 kernel
carries private string functions today (grep: none), which means the
first large struct copy clang lowers to a `memcpy` CALL is a link
error waiting in ambush.  A5 adds `kernel/lib/string.c` to
`KERNELA64_SHARED` (the fdt.c promotion shape: shared object, claim
asserted), closing the OPT residue line instead of forking a fifth
copy.

**AMEND-3 (A7).  PL011 TX can take the O3 ring core for free.**
`drivers/uart/uart_ring.h` is pure C (no I/O, no locks), host-tested
across wrap/full/empty and the 2^32 counter crossing (75 checks).
A7's PL011 RX task grows a sibling: TX through the same ring shape —
"same shape, different registers" was the OPT §7 assessment, and the
index core is the part that is identical.

**AMEND-4 (A5, note).  Do not import x86's TLB history.**  OPT O5
built ranged shootdown for x86_64 out of mailboxes and IPIs because
the ISA gave it nothing; aarch64 has `TLBI VAE1IS` — per-VA,
inner-shareable, one instruction.  When A5's user teardown creates the
first real unmap traffic, the precise form costs an instruction, not a
phase.  A3's `vmalle1` helper stays correct as the fallback; this note
exists so nobody copies the broadcast-first evolution.

**AMEND-5 (A8/A9, named deferral).  fw_cfg is no longer "ignored".**
Fact 3 waved the `virt` board's fw-cfg node off; since OPT O2 the tree
has a fw_cfg *protocol* in production — `opt/auralite.selftest`
selects the self-test intensity and the integration lib pins CI boots
through it.  The aarch64 fw-cfg is MMIO (not port I/O), so the x86
reader does not transfer as-is; the knob's *interface* does.  Recorded
as a deferral with a name, not an absence: when a64 boots grow
self-tests worth scaling, the protocol already exists.

**AMEND-6 (A8/A9, gate hardening).  Assert the cross-toolchain EXISTS
after install.**  This audit's own environment lost
`riscv64-linux-gnu-gcc` to silent apt dependency failures three times
("Setting up ..." printed, binary absent — the Conflicts field Fact 1
measured is exactly the mechanism).  A8's crypto gate and A9's CI job
must `command -v` the cross-gcc AFTER the install step, not trust the
installer's exit status; the loud-SKIP path already exists for the
genuinely-absent case.

**AMEND-7 (A5).  User links inherit O8's GC from birth.**  The a64
user linker script (`user_a64.ld`) ships with `--gc-sections` on its
link line from the first patch — the x86 user ELFs measured −65%
initrd from it, and a fourth tenant has no legacy to protect.  One
measured fact transfers with it: lld roots SHT_INIT_ARRAY sections by
built-in rule, so KEEP there is belt-and-braces, not load-bearing
(OPT O8's negative control refuted the folk theorem — the linker
scripts say so).

---

## 2. Decisions

### D1. aarch64 (ARMv8-A) on the QEMU `virt` machine is the target; arm32 is not

`-cpu cortex-a72 -machine virt`. 32-bit ARM would re-fight the I6
pointer-width war on an ISA that is being retired from the world; it
buys no new audit. The rv32 refusal transfers with its reasoning
intact. `virtualization=on` (EL2 entry) is refused the same way: the
kernel targets EL1, and a measured `CurrentEL != EL1` at boot gets an
honest refusal banner — the I0 lesson, fourth edition.

### D2. No firmware chained, PSCI is the platform API — and the DTB comes from the RAM base

QEMU loads the ELF and enters it at EL1 directly; there is no BIOS, no
UEFI, no OpenSBI equivalent in the boot path. PSCI over `hvc` is a
*service*, not a *stage*: power-off and (later) CPU_ON. Measured facts
this decision rests on: EL1 entry, PSCI SYSTEM_OFF working, and —
the one that would have cost a debugging session — **`x0` is NOT the
DTB pointer for ELF payloads; the DTB sits at `0x40000000`**, magic
verified. A1 validates the magic and refuses loudly on mismatch rather
than trusting placement.

### D3. 39-bit VA with 4 KB granule — deliberately the same geometry as Sv39, and the HHDM constant survives

`TCR_EL1.T0SZ = T1SZ = 25` gives 39-bit VA spaces with 4 KB granule:
three levels, 512-entry tables, 1 GiB block entries at level 1 — the
exact shape of Sv39. This is chosen *because* it is the same shape:
the rv64 paging code's structure (early gigapage window, then real
tables) transfers move-for-move. And the TTBR1 canonical range for
T1SZ=25 is `0xFFFFFFC000000000`–`0xFFFFFFFFFFFFFFFF` — meaning
**`HHDM_OFFSET = 0xFFFFFFC000000000` is the same number on aarch64 as
on riscv64**, by arithmetic, not by copy-paste. D3's rule stays: the
kernel validates the field, never assumes it; the claim check asserts
the two constants are equal *and* asserts the comment explaining it is
arithmetic, not coincidence. What does NOT transfer: aarch64 has two
TTBRs (user tables in TTBR0, kernel in TTBR1), so the
kernel-half-shared-in-every-address-space property is hardware-given
rather than software-maintained — one fewer invariant to defend, and
the phase result says so.

### D4. `svc #0` from EL0, AuraLite's numbers, Linux's register convention — inherited

The fourth trap mechanism enters the same one syscall table:
`x8` = number, `x0`–`x5` = args, in-band negative errno in `x0`.
Numbers stay AuraLite's (Linux-x86_64-tracking) — and it is worth
noting aarch64 is the arch where Linux's *own* numbers diverge from
x86_64, which is exactly why AuraLite's D4 (one table, ours) was the
right call: nothing needs renumbering. The wrapper is
`lib/libca64/syscall_a64.S`; the "main return already in x0" accident
holds here too.

### D5. Boot CPU only at first; `PSCI CPU_ON` is the named exit ramp

The DTB advertises `cpu_on = 0xc4000003`. Inherited from D5/V-D5 with
the mechanism renamed. The A0 stub parks nothing (QEMU releases one
CPU for `-kernel` ELF boots unless `-smp` says otherwise), but the A0
smoke still runs `-smp 4` and asserts the banner prints exactly once —
assert, don't assume, even when the assumption is documented QEMU
behaviour.

### D6. The sweep is a backend now, not a phase of discovery

V6 did the expensive work: portable files already speak only the four
irqflags contracts and C11 atomics, policed by ratchet 4 (baseline 29).
A6 therefore ships `kernel/arch/aarch64/irqflags.h` (DAIF/`wfi`/
`yield`), proves the contract closes over a fourth ISA, and pays the
ratchet down where the port trips over residue — but it plans no new
ratchet. If A-phases discover portable-file asm the regex missed, that
is a checker bug to fix in-phase, not a new baseline.

### D7. virtio-mmio first via the *shared* transport; PCIe ECAM measured, named, deferred

32 MMIO windows exist (Fact 3) and the transport code exists (Fact 6).
A7's job is promotion + a GICv2 IRQ path + Device-nGnRE mappings — not
a new driver family. PCIe ECAM (`pci-host-ecam-generic` at
`0x10000000`) is real on this board and stays deferred with the same
argument as V-D7: the first transport alone, shipped working, then the
next. The deferral is recorded in status.md, not silently.

### D8. The claim checker ships in A0 — inherited, now a tradition

`tools/check_arm64_claims.py`, same skeleton as the riscv checker
(which is the i386 checker's skeleton): claims-vs-tree, `--selftest`
with a doctored tree, terminal Status arithmetic armed from birth so
the plan cannot be declared COMPLETE by prose. Registered in
`make test-unit` beside its two siblings in A0, not A9.

---

## 3. Phases

### Phase A0 — Toolchain gates + the EL1 stub ✅ COMPLETE

**Objective:** the fourth architecture exists in the build system and a
banner proves the whole path: clang → lld → QEMU ELF load → EL1
`_start` → PL011 → PSCI power-off.

#### Tasks

- [x] `Makefile`: `kernela64` target family (`--target=aarch64-unknown-
      none-elf -mgeneral-regs-only -mstrict-align`, `ld.lld -m
      aarch64linux`); `deps-check` learns the optional aarch64 tools
      the mingw/riscv way (absent = a64 targets skip loudly, all other
      builds untouched). `-mstrict-align` is load-bearing before the
      MMU (Fact 5.1) and stays on until A3 measures it safe to drop.
- [x] `kernel/arch/aarch64/boot.S`: `.text.boot` first (the V0 linker
      discipline, kept even though QEMU honours ELF entry — free
      insurance); assert `CurrentEL == EL1` and print the honest
      refusal banner otherwise (D1); set `sp`, clear `.bss`, mask DAIF,
      call `kmain_a64`.
- [x] `kernel/arch/aarch64/pl011.c`: TX-only day-0 console (reset-state
      UART, Fact 2); `kernel/arch/aarch64/psci.c`: `SYSTEM_OFF` via
      `hvc` (Fact 4) so every smoke run exits, never times out.
- [x] `main_a64.c`: banner + `CurrentEL` + `CNTFRQ_EL0` echo + the DTB
      magic probe at the RAM base (Fact 2.2 — print the address and
      the magic, refuse on mismatch). `kernela64.ld`:
      `OUTPUT_ARCH(aarch64)`, link at `0x40200000`, full linker script
      (the V0 bare-`-Ttext` footgun is a lineage comment here).
- [x] `tools/check_arm64_claims.py` (D8): first claims + the structural
      Status-vs-table checks + `--selftest`, registered in
      `make test-unit`.
- [x] `tests/integration/a64_boot_smoke.sh`: assert the banner, `EL=1`,
      the DTB magic line, power-off exit; `-smp 4` prints the banner
      exactly once (D5); skip cleanly when `qemu-system-aarch64` is
      absent.

#### Result

Delivered as specified — and unlike V0, with **no unpredicted facts**:
§1's fact-finding had already stepped on the traps this phase would
otherwise have found (the x0-is-not-the-DTB trap and the EL2 entry
mode were both measured before a line of the kernel existed, which is
the whole argument for fact-finding-first). Measured under
`qemu-system-aarch64 -machine virt -cpu cortex-a72`:

```
Hello from AuraLite OS kernel (aarch64)!
[kernel] AuraLite OS aarch64, ARM64_PLAN phase A0
[boot] CurrentEL: EL1
[boot] x0 at entry: 0x0000000000000000 (not the DTB pointer for ELF payloads -- measured, plan Fact 2)
[boot] DTB probe at RAM base 0x0000000040000000: magic 0x00000000D00DFEED OK (big-endian read)
[boot] CNTFRQ_EL0: 62500000 Hz
[kernel] A0 stub complete; powering off via PSCI (A1 adds the DTB -> boot_info walk)
```

The smoke test's 10 assertions pass: every §1 receipt is now a
regression gate (a QEMU behaviour change shows up as a *named* red
line), the run ends by PSCI `SYSTEM_OFF` in under a second, `-smp 4`
prints the banner exactly once, and `virtualization=on` gets the D1
refusal banner with zero kernel output after it — the refusal path
parks in `wfi` rather than attempting PSCI from EL2 (an `hvc` from
EL2 would trap into our own empty EL2 vector, so the honest option is
to stop moving; the smoke asserts the banner, not an exit code, on
that path). `build/kernela64.elf` is 28K. The kernel is compiled
`-mstrict-align -mgeneral-regs-only` (Fact 5.1 and the M1 FPU lesson
respectively — the second flag keeps the compiler from spilling
through q-registers that no context switch saves yet).
`check_arm64_claims.py` opens at 11 claims (9 phase + 2 structural,
the terminal Status arithmetic armed from birth per D8).

**One deviation from the phase's "nothing shared is edited" claim,
measured and named**: `KERNEL_SRCS`/`KERNEL_ASMS` in the Makefile
gather sources by `find kernel drivers`, excluding other arch
directories by `-not -path` — and the exclusion list did not know
about `kernel/arch/aarch64/` yet, so the x86_64 kernel swept the new
directory into its own build and failed on aarch64 register names in
`psci.c`. Fixed by extending the exclusion (two lines); `kernel32`
and `kernelrv` use per-directory finds and were never exposed. The
lesson recorded for A1–A9: the x86_64 target's find(1) is the one
place where "adding a directory" IS "editing shared build logic", so
each later phase's "suites untouched" gate is proven by building all
four kernels, not by diff inspection. All four built and all
pre-existing gates re-ran green after the fix.

#### Test gate

- Stub banner on the PL011 under `-machine virt -cpu cortex-a72`; all
  smoke assertions; the x86_64, i386 and riscv64 suites untouched
  (nothing shared is edited in A0).

#### Deliverable

`patches/A64_A0_boot.patch`

---

### Phase A1 — `boot_info_t` from the Device Tree, the walker promoted ✅ COMPLETE

**Objective:** the DTB at the RAM base becomes a populated
`boot_info_t`, through a *shared* walker.

#### Tasks

- [x] Promote `kernel/arch/riscv64/fdt.c` → `kernel/dt/fdt.c` (Fact 6):
      the walker's 7 riscv references become a two-function arch hook
      (phys-to-virt for the early window; nothing else). The riscv64
      kernel consumes the shared object **in this same patch** — the
      claim check asserts both kernels link `kernel/dt/fdt.o`, so the
      promotion cannot silently bitrot into two copies.
- [x] aarch64 consumers: memory from `memory@40000000`, the PL011
      `reg`, the GICD/GICC pair from `intc@8000000`, the 32 virtio
      windows, the PSCI method string (assert `"hvc"` — a `"smc"`
      board would need one instruction changed, and the assert names
      it), `/chosen` for initrd start/end when `-initrd` is present.
- [x] **Interrupt-cell normalisation, once, centrally** (Fact 3): the
      walker's aarch64 consumer converts `<GIC_SPI n>` to INTID n+32
      and `<GIC_PPI n>` to INTID n+16 at parse time; drivers see final
      INTIDs only. A comment names the off-by-32 as the bug class
      being prevented.
- [x] `rv_boot_smoke.sh` re-run green with the shared walker (the
      promotion's non-regression gate); `a64_boot_smoke.sh` grows
      boot_info assertions (RAM size, UART base, INTID for SPI 1 = 33
      printed and checked).

#### Result

Delivered as specified — the promotion held, and it caught exactly the
kind of bug the plan's thesis predicted it would. Measured boot:

```
[boot] handoff magic OK, path=PSCI, boot_info filled from DTB
[mm]   mmap entries: 3, usable RAM: 256 MiB
[hw]   cpus: 1 (boot cpu 0)
[hw]   uart: 0x0000000009000000 irq 33 (INTID, normalised)
[hw]   gicd: 0x0000000008000000 gicc: 0x0000000008010000
[hw]   virtio-mmio windows: 32 (irq 48..79)
[hw]   psci: method hvc (matches psci.c's conduit)
```

**The promotion flushed out a riscv-shaped assumption, as designed.**
The walker tracked the current device node in scalars (`cur_dev`,
`cur_reg`); the aarch64 GIC node carries a `v2m@8020000` *child*, so
the child's `END_NODE` wiped the parent's state before the parent's
own exit could pair reg with compatible — first A1 boot printed
`gicd: 0x0`. The riscv virt tree has no device nodes with children,
so the scalar design passed every V1 gate for the port's whole life.
Fixed with per-depth state (`ndev[depth]`), lesson recorded in the
walker's comment. This is the plan's argument for promotion over
forking made concrete: a fork would have hidden the assumption
forever; the second consumer *was* the test.

Two more facts measured and pinned:

- **QEMU does not load `-initrd` for ELF payloads on this board** —
  no `/chosen` initrd properties (dumpdtb) AND the payload bytes
  absent from RAM (scanned). Same family as the x0-is-not-the-DTB
  fact: the initrd machinery activates on the Linux-`Image` path
  only. Measured on a raw-Image probe: there `/chosen` gets both
  properties and `x0` carries the DTB. The smoke *pins* the ELF
  behaviour (`initrd: none` asserted with `-initrd` passed); A5 owns
  the exit ramp — the initrd-carrying boots will need a raw-Image
  packaging step (`llvm-objcopy -O binary` + the 64-byte header),
  which also un-defers the x0 path. The walker's `/chosen` code is
  real and the riscv64 suite exercises it every push.
- **The interrupt normalisation deferral matters**: `intc_kind` is
  discovered from the GIC node, which the walk may meet *after* the
  devices (it does on the aarch64 tree — the UART sits before the
  intc). Raw `interrupts` properties are therefore remembered
  per-device and normalised at `done:`, when the controller kind is
  settled. INTIDs measured: UART SPI 1 → 33, virtio SPIs 16–47 →
  48..79, exactly the plan's Fact 3 arithmetic.

One ratchet payment: the promotion makes `kernel/dt/fdt.c` portable
code, so ratchet 1 started counting its single `(uint64_t)` cast
(359 → 360). Paid by widening through assignment in `be64()` —
baseline restored, and the walker now plays by the same rules as the
rest of kernel/. `check_riscv_claims` grew the single-object claim
(60 total), `check_arm64_claims` grew 7 A1 claims (18 total), and
both kernels' smokes run green on the one walker: 20/20 (a64, +10
over A0) and the full rv suite untouched.

#### Test gate

- Both DTB-consuming kernels boot green on the shared walker; the a64
  smoke asserts normalised INTIDs; riscv64 assertions unchanged.

#### Deliverable

`patches/A64_A1_dtb.patch`

---

### Phase A2 — Exceptions, the generic timer, GICv2 ✅ COMPLETE

**Objective:** `VBAR_EL1` vectors, a ticking `TICK_HZ=100` from the
virtual timer, and SPIs delivered through a real GICv2 driver.

#### Tasks

- [x] `kernel/arch/aarch64/vectors.S`: the 16×128-byte table (Fact
      5.5), one shared spill path (the trapentry.S discipline —
      one assembly entry, C dispatch); `SP_EL1`/`SPSel` arranged so
      EL0 traps land on a dedicated kernel stack (the I7 esp0 lesson,
      fourth edition — lineage comment required).
- [x] `kernel/arch/aarch64/gic.c`: GICv2 distributor + CPU interface
      bring-up (group enable, priority mask, per-INTID enable), IAR
      claim / EOIR complete flow. INTIDs arrive pre-normalised from A1.
- [x] Timer: virtual timer (`CNTV_CTL/CNTV_TVAL`), PPI INTID 27, at
      `CNTFRQ_EL0` (Fact 2.3 — read the register, no DTB field, and
      the smoke prints the frequency it measured); `TICK_HZ=100` wired
      to the shared tick path.
- [x] Exception decode: `ESR_EL1` class/ISS printed on unexpected
      traps — the honest-panic shape the other three kernels have.
- [x] Smoke: tick counter advances, a deliberate unaligned read before
      `-mstrict-align`-relaxation faults with a decoded ESR (assert
      the fault class, proving the vector path), spurious-INTID path
      exercised.

#### Result

Delivered as specified — and this phase found the plan's first two
*unpredicted* measured facts, both of the shape fact-finding cannot
catch (they only exist once real trap traffic flows):

1. **QEMU enters ELF payloads with `SPSel = 0`.** The A0/A1 kernel
   unknowingly ran on SP_EL0 the whole time — harmless while no trap
   ever fired, fatal the moment the first timer IRQ arrived: it
   landed in the `IRQ/SP_EL0` vector row, the row whose tag exists
   precisely to name this bug. The dump named it, one `msr spsel, #1`
   in boot.S fixed it, and the SP_EL0 rows stay panic rows *because*
   the kernel now declares its stack discipline instead of inheriting
   it. (Bonus recorded for A4: SP_EL0 is a free register for the EL0
   story precisely because the kernel never squats on it.)
2. **The tag formula must exist exactly once.** vectors.S tagged
   slots `kind*4+origin` while the dispatcher assumed position-major
   `origin*4+kind`; the self-test UDF was dispatched as a different
   row entirely. Fixed by making the tag equal the hardware's own
   slot index (origin selects the 0x200 block, kind the 0x80 slot),
   so `tag == slot index` and `kind_names[]` reads straight down the
   table. One formula, used twice, or none — recorded in vectors.S.

Measured gauntlet (one boot):

```
[isr]  PASS: undefined instruction named and resumed
[isr]  PASS: unaligned load faulted (Device memory, pre-MMU -- Fact 5.1 measured)
[timer] PASS: 48 ticks observed at 100 Hz (virtual timer, INTID 27)
[gic]  PASS: claim/complete round-trip (48 completions)
[rng]  jitter events collected: 48
```

The alignment gate deserves its sentence: the probe is a handwritten
`ldr` at stack+1 (the compiler under `-mstrict-align` will never emit
one, which is exactly why it is handwritten), and it Data-Aborts with
EC 0x25 as Fact 5.1 predicted — the flag's premise is now measured,
not folklore. The timer INTID is written as `11u + 16u` so the 27 has
a paper trail. The TVAL re-arm carries the `sbi_set_timer` property
(the write un-asserts the line), so A4's post-EOI preemption
placement transfers unchanged. Deviation from the task text: the
spurious-INTID path is *handled* (IAR 1023 returns) but not
separately asserted — a spurious interrupt cannot be provoked on
demand from inside the guest; the claim-vs-ticks equality gate covers
the dispatch loop's exit condition instead. `check_arm64_claims` 27
(+9), a64 smoke 27/27, all other suites green.

#### Test gate

- 100 Hz ticking measured over a second of guest time; ESR decode
  asserted; all other suites green.

#### Deliverable

`patches/A64_A2_traps.patch`

---

### Phase A3 — Memory: TTBR1 39-bit VA, PMM, heap — W^X twice over ✅ COMPLETE

**Objective:** paging on with the Sv39-shaped geometry (D3), the same
HHDM constant, PMM + kheap up, and both execute-never bits earning
their keep.

#### Tasks

- [x] `paging_a64.c`: T0SZ=T1SZ=25, 4 KB granule; MAIR with exactly two
      indices (Normal WB, Device-nGnRE) — Fact 5.2, with the virtio
      windows and GIC/PL011 mapped Device; early 1 GiB block window,
      then real tables; `TLBI vmalle1` + `dsb ish` + `isb` discipline
      (Fact 5.3) in one helper, not scattered.
- [x] `HHDM_OFFSET 0xFFFFFFC000000000` — same value as riscv64 *by
      TTBR1 arithmetic* (D3); the claim check asserts equality across
      the two headers and the explanatory comment.
- [x] PMM + kheap: the rv64 shape (`pmm_rv.c`/`kheap_rv.c` structure)
      ported; heap window placed clear of the HHDM.
- [x] W^X: kernel `.text` RX+PXN-clear, data RW+PXN+UXN, user pages
      UXN-from-kernel (Fact 5 bonus); the elfperm refusal gates return;
      a deliberate W+X mapping attempt is refused and the smoke asserts
      the refusal text.
- [x] `-mstrict-align` measured: either dropped post-MMU with a
      comment citing the measurement, or kept with the cost measured
      and named (whichever the numbers say — decided by measurement,
      recorded in the phase result).

#### Result

Delivered as specified. The kernel boots through the high half from
the first C instruction (boot.S: MAIR → TCR → early TTBR0/TTBR1
gigapage roots → `SCTLR.M` behind `dsb ish; isb` → literal-pool jump),
and the final tables land the full V3 gate set plus the UXN bonus:

```
[vmm]  TTBR1 final tables live: .text RX+UXN, .rodata R, data RW, HHDM Normal-WB; MMIO Device-nGnRE; TTBR0 blanked
[vmm]  store to .text faulted (W^X write half)
[vmm]  execute-from-data faulted (W^X execute half -- PXN earning its keep)
[vmm]  identity window confirmed dropped (TTBR0 blank: low load faults)
[pmm]  PASS  [heap] PASS  (bitmap.h's fourth consumer; the kheap window at the SAME VA as rv64's)
```

D3 held by computation: `HHDM_OFFSET` came out `0xFFFFFFC000000000`
because T1SZ=25 puts a 512 GiB window at `0xFFFFFF8000000000` and the
constant is 256 GiB in — level-1 index 256, the same index it hits in
Sv39. The claim check asserts both headers carry the value AND the
index-256 argument. The identity window died *differently* than Sv39
— TTBR0 handed a blank root, one register write — and the file argues
the difference instead of hiding it.

Three measured facts this phase added:

1. **The day-0 console becomes a page-fault generator the moment the
   MMU turns on**: the first A3 boot hung silently because
   `pl011.c`'s base was still physical — the banner's own printer was
   the unmapped address. The base is HHDM-shaped now, with the
   measurement in the comment.
2. **Ordering: vectors before fault probes.** The first arrangement
   ran a3 before a2; a W^X probe with VBAR unset is a hang, not a
   test. `a2_bringup()` precedes `a3_bringup()` with the reason in
   the call site — and the timer keeps ticking straight through the
   table switch, which is its own small proof the switch is sound.
3. **QEMU's TCG does not model alignment faults on *mapped* Device
   memory.** Architecturally an unaligned access to Device-attributed
   RAM faults; A2 measured the fault with the MMU off, but with the
   early tables mapping RAM as Device, the same load sails through.
   Both polarities are gated (Device-mapped: pinned as not-faulting
   under TCG; Normal WB after the final tables: succeeds), and the
   `-mstrict-align` decision follows FROM the gap: **the flag stays**,
   precisely because real hardware may fault where TCG does not — the
   compiler must not emit the access class the emulator under-models.
   A QEMU behaviour change flips the pinned gate and names itself.

`check_arm64_claims` 38 (+11), a64 smoke 35 assertions green, all
other suites untouched and green.

#### Test gate

- Boot via the high half; W^X refusal asserted; unaligned access on
  Normal memory works post-MMU; PMM/heap self-tests; HHDM equality
  claim green.

#### Deliverable

`patches/A64_A3_mm.patch`

---

### Phase A4 — Threads, scheduler, EL0, `svc` ✅ COMPLETE

**Objective:** the shared scheduler runs aarch64 threads; EL0 entered;
`svc #0` reaches the one syscall table (D4).

#### Tasks

- [x] `context_a64.S`: callee-saved x19–x30 + sp switch; FPU eager
      save of q0–q31 + fpcr/fpsr (Fact 5.6; M1 lineage comment), with
      `CPACR_EL1.FPEN` open — the "trap-and-lazy-save" tradition is
      refused with the same argument the rv64 port used.
- [x] EL0 entry: `spsr_el1` crafted, `eret`; per-thread kernel stack in
      `SP_EL1` (the esp0/sscratch contract, now `SPSel`-shaped —
      the claim check gets a claim on the lineage comment).
- [x] `svc` dispatch: ESR class 0x15 routed to the shared table; x8/x0
      convention per D4; in-band negative errno.
- [x] Numbers spot-checked in the smoke: GETPID=39, EXIT=60,
      SCHED_YIELD=158 — the same numbers as the other three trap
      mechanisms, asserted from EL0.

#### Result

Delivered as specified — with one measured fact that cost the phase
its only debugging session, and it is a keeper:

**The low half is a different tree.** The first EL0 entry
Instruction-Aborted at its own entry point (`ec=32
far=0x40000000`): the user text had been mapped through `walk()`
into TTBR1's tree, but VA `0x40000000` is the LOW half, and the low
half translates through **TTBR0** on this ISA. One Sv39 root covers
all of VA; a VMSAv8 pair does not — the same "one address space"
assumption the A1 walker promotion flushed out, now in its paging
edition. `walk()` chooses the root BY THE VA now (low → TTBR0's
tree, high → TTBR1's, the unmappable middle refused), the A3 claim
tracked the change (TTBR0 is blank *at switch time*, populated later
by user pages only), and the identity-window probe keeps the old VA
honest forever.

What the ISA gave back, in exchange: **the I7 esp0 lesson costs zero
instructions here.** Any EL0 trap lands on SP_EL1 by hardware SPSel
switch — no scratch-CSR swap-and-test, no TSS field; "arm the trap
stack" is just "hold it in sp when eret'ing down" (user_enter_a64,
four instructions plus eret).

The gauntlet, one boot:

```
[sched] PASS: two never-yielding workers both finished (timer preemption is real)
[fpu]  PASS: q8/q9 survived preemptive clobbering (eager save earns its 528 bytes)
A64-U-OK!
[user] exit(42) via svc
[user] PASS: privileged op contained (code 128), kernel intact
```

Notes with paper trails: the 624-byte switch frame put fpcr/fpsr LOW
(stp-x reach ends at #504 — the assembler said so); CPACR_EL1.FPEN
opens in boot.S *before any switch exists* (reset value traps the
first `stp q0,q1` as EC 0x07); `-mgeneral-regs-only` stays for the
compiler while `.arch_extension` opens the assembler's gate exactly
where q-registers are touched on purpose (context_a64.S, the FPU
gate's probes); the EL0 test programs are assembler-measured bytes,
not hand-rolled (the V0 pad-byte lesson — and the pasted literal
pool is the one clang emitted). The privileged-mrs negative control
came back EC 0x00 (QEMU raises Unknown, not the trapped-MSR class)
— the assertion accepts any contained 128+EC, and this note records
the measured value. Preemption is post-EOI, after `gic_dispatch`
returns — EOIR must complete the timer INTID before a switch can
suspend the interrupted thread (phase-6 freeze, third inheritance).
`check_arm64_claims` 47 (+9), a64 smoke 43 assertions, all sibling
suites green.

#### Test gate

- Two kernel threads round-robin; an EL0 payload makes syscalls and
  exits; FPU state survives a context switch under deliberate
  clobbering (the M1 regression test, fourth edition).

#### Deliverable

`patches/A64_A4_proc.patch`

---

### Phase A5 — Userspace (split 2026-08-20, before execution)

A5 as originally drafted bundled three separable risks: a *boot
protocol* change (the raw-Image packaging A1's result assigned here),
a *build/artefact* step (the fourth tenant), and a *kernel execution*
step (ELF loading + EL0 shell).  Each has its own failure modes and
its own gate, so each is now a phase — the D7 ship-the-first-thing-
alone rule applied to the plan's own structure.  Dependency order:
A5a's Image boot is what makes `-initrd` load at all (A1's measured
fact), so the tenant (A5b) has nothing to ride in before it; the
loader (A5c) has nothing to load before A5b.

---

### Phase A5a — The Image exit ramp + the shared strings ✅ COMPLETE

**Objective:** initrd-carrying boots become possible — the raw-Image
packaging un-defers the `x0 = DTB` path A1 pinned — and the kernel
stops being one clang-lowered `memcpy` away from a link error
(AMEND-2).

#### Tasks

- [ ] `boot.S` grows the 64-byte Linux arm64 Image header at the top
      of `.text.boot`: `code0` = a branch over the header (the header
      IS the entry point on both paths — the V0 first-byte discipline
      pays off a fourth time), `text_offset = 0x200000` so the Image
      lands at the ELF link address and ONE binary layout serves both
      boot paths, `image_size` from linker arithmetic, LE/4K flags,
      `"ARM\x64"` magic.  The ELF path executes the same branch and
      never notices.
- [ ] Makefile: `build/kernela64.img` via `llvm-objcopy -O binary` +
      `run-a64-img`; `llvm-objcopy` join the optional-tools check the
      qemu way (absent = img target skips loudly).
- [ ] `kmain_a64`: if `x0 != 0` and the FDT magic validates there, use
      it (Image path — the promise A1 measured as Image-only becomes
      load-bearing); else the RAM-base probe (ELF path).  BOTH echoes
      stay printed; both facts stay pinned by their smokes.
- [ ] **[AMEND-2]** `kernel/lib/string.c` joins `KERNELA64_SHARED`
      (portable bodies under `#ifndef ARCH_X86_64` since OPT O1);
      claim asserts the shared object links.
- [ ] `tests/integration/a64_image_smoke.sh`: Image boot reaches the
      A4 gauntlet's end; `x0` path taken (echo asserted); with
      `-initrd build/initrd.tar`: `/chosen` initrd-start/end parsed
      and `ustar` found at the parsed address (the A1 "initrd: none"
      assertion stays in the ELF smoke — the polarity pair).

#### Result

Delivered as specified, first boot clean on both paths.  Measured:

```
[boot] x0 at entry: 0x0000000048400000 (DTB pointer, Image path -- magic verified)
[boot] DTB source: x0 (Image boot protocol)
[mm]   initrd magic: ustar OK
[mm]   initrd: 3061760 bytes at phys 0x0000000048000000
A64-U-OK!
```

- **One binary layout serves both paths, as designed**: the header's
  `text_offset = 0x200000` places the Image at the ELF link address,
  so `llvm-objcopy -O binary` of the ELF IS the Image — 148K ELF,
  56K Image, no second linker script.  The ELF path executes the
  header's code0 branch and never notices; every A0–A4 pin in
  `a64_boot_smoke.sh` still greps the same bytes.
- **A1's Image-only facts became load-bearing on schedule**: x0
  carried the DTB (parked AFTER the initrd at `0x48400000` — noted:
  NOT the RAM base, which is why the source choice verifies magic
  rather than assuming either address), `/chosen` grew the initrd
  properties, and the initrd bytes measured present by tar-magic
  read-back — 3 061 760 bytes, which is O8's GC'd initrd riding in
  for A5b's tenant.
- **[AMEND-2] paid immediately**: `kernel/lib/string.o` links into
  the a64 kernel (OPT O1's portable bodies) — the fdt.c promotion
  shape, claim-checked.
- The full A4 gauntlet runs to the end on the Image path (12/12 smoke
  assertions).  `check_arm64_claims` 54 (+7).  Sibling suites: x86_64
  boot 17/17, rv smoke green, i386 builds.

#### Test gate

- ELF smoke unchanged and green (both A1 pins intact) ✓; Image smoke
  12/12 ✓; all sibling suites green ✓.

#### Deliverable

`patches/A64_A5a_image.patch`

---

### Phase A5b — libca64 + the fourth tenant ✅ COMPLETE

**Objective:** `/bina64` exists in the one initrd, audited, before any
kernel code tries to run it.

#### Tasks

- [ ] `lib/libca64/`: `crt0_a64.S`, `syscall_a64.S` (D4: `x8`/`x0`–`x5`,
      `svc #0`), `libca64.h` — the libcrv surface, same names.
- [ ] `user_a64.ld`: the shared ELF window `[0x08000000, 0x40000000)`;
      **[AMEND-7]** `--gc-sections` on the link line from birth
      (O8's −65% receipt; SHT_INIT_ARRAY is an lld GC root — KEEP is
      convention).
- [ ] `initrv`/`smallsh` sources compiled for a64 (shared sources, no
      forked shell) — `usera64` target family.
- [ ] `mkinitrd.sh`: `audit_tenant bina64 183 aarch64`; the
      cross-copied-binary negative control re-run for the new tenant.
- [ ] The x86_64 initrd staging picks up `/bina64` exactly as it did
      `/binrv` — tenant bytes identical in every kernel's initrd.

#### Result

Delivered as specified.  The fourth tenant exists, is audited, and no
kernel has been taught to run it yet — by design (that is A5c's one
job).

- `lib/libca64/`: the libcrv surface at the fourth trap mechanism.
  The svc wrapper is six moves — the D4 "arguments shuffle down one
  register" accident holds a fourth time, and `main`'s return arriving
  where SYS_EXIT wants it holds too (crt0 comment carries the lineage).
- The SHARED shell compiled unmodified through the AURA_LIBC seam —
  the V5 promotion pays its fourth dividend; `smallsh` needed zero
  edits for a fourth ISA it has never met.
- **[AMEND-7] paid**: both a64 user links carry `--gc-sections` from
  birth (`-ffunction-sections -fdata-sections` in CFLAGSA64_USER).
- `mkinitrd` audits `bina64` against `e_machine 183`; the packaged tar
  measured: `/bina64/init` + `/bina64/smallsh`, 92 files, 3.2M.  The
  **negative control executed live**: an rv64 binary planted in
  `bina64/` fails the pack with the guilty file NAMED
  (`bina64/evil-rv has e_machine=243, expected 183`) and exit 1.
- Sibling gates after the initrd change: x86_64 boot 17/17, rv smoke
  green, a64 ELF + Image smokes green (the Image smoke now carries the
  four-tenant tar into RAM every run).  `check_arm64_claims` 60 (+6).

#### Test gate

- `initrd.tar` carries `/bina64` with `EM_AARCH64` ELFs ✓; the audit
  passes and its negative control fails the doctored tenant (named,
  exit 1) ✓; x86_64/rv64/a64 suites untouched ✓.

#### Deliverable

`patches/A64_A5b_tenant.patch`

---

### Phase A5c — ELF loading, the EL0 shell, cross-refusals ✅ COMPLETE

**Objective:** the fourth tenant runs: initrd parsed on a64, ELF loaded
with the window/W^X refusals, the shared shell interactive at EL0.

#### Tasks

- [ ] `initrd_a64.c`: the USTAR tenant walk (initrd_rv.c's shape) over
      the A5a-delivered initrd.
- [ ] `elfa64load.c`: `EM_AARCH64 = 183` accepted; window checks; W^X
      (UXN/PXN both earning keep — A3's bonus extended to user text);
      the other three `e_machine`s exec-refused, AND the other three
      kernels' refusal tables grow the 183 row (asserted at least for
      a64-rejects-rv and rv-rejects-a64).
- [ ] **[AMEND-4]** teardown/unmap paths use `TLBI VAE1IS` per-VA —
      the precise form is one instruction on this ISA; `vmalle1`
      remains the documented fallback.
- [ ] `a64_shell_smoke.sh`: the rv_shell shape — prompt, builtin sweep,
      syscall round-trips, exit.

#### Result

Delivered — `auralite#` lives on the fourth architecture, and the
phase paid for two measured facts that are now regression pins:

```
[init] PASS: inita64 ran and exited 7 as built
auralite# uname
AuraLite OS aarch64 (TTBR1 higher half, ARM64_PLAN A5)
auralite# run bina64/init      -> depth 1 ... exit code 7
auralite# run binrv/init       -> refused: machine 243 (riscv64 -- the /binrv tenant, wrong kernel)
[shell] exited 0
[kernel] A5c complete; powering off via PSCI
```

1. **The banked register the trap frame does not carry.**  The first
   interactive session flooded 20 MB of `auralite#` in 40 s: after a
   nested spawn returned, every parent `read()` was an instant
   `-EFAULT`.  Hunted with throttled named-refusal prints: the parent
   shell's buffer address was INSIDE the dead child's stack — the
   vectors spill GPRs+ELR+SPSR but not SP_EL0, so the child's
   `user_enter_a64` silently re-pointed the PARENT's EL0 stack, and
   the child's exit unmapped it out from under the parent.  Fixed by
   saving/restoring SP_EL0 with the rest of the parent context in
   `user_a64_run_elf`; the smoke pins it twice (no `READ EFAULT` +
   the log-size fuse that fails the test at 200 KB instead of letting
   a flood eat the workspace).  A first "fix" (wider stacks) was a
   misdiagnosis — the "20 KiB deep shell" was the two levels'
   top-to-top distance wearing a disguise — recorded because the
   wrong fix LOOKED plausible for one whole rebuild.
2. **The rv64 exit convention does not transfer.**  rc-1 encoding is
   riscv's; the A4 trampoline travels `exit_code_box` — the first
   draft copied the former and every child "exited 0".  The smoke's
   `exit code 7` assertion is the pin.

The rest as specified: `initrd_a64.c`/`elfa64load.c` are the rv
shapes (the walk unchanged; the loader speaks A64_MAP_* bundles, so
every user page carries PXN, W+X refused, and a new `A64_MAP_RO_USER`
makes PF-neither rodata enforced-read-only); the cooked `read()`
blocks on `wfi` with DAIF re-opened (the I7 deadlock's fourth
spelling — and the difference between a shell and a prompt flood);
user stacks are 8 pages with an unmapped guard hole between nesting
levels; **[AMEND-4] paid** — `paging_a64_unmap` is one `TLBI VAE1IS`,
not `vmalle1`; the other three kernels' refusal tables grew the named
183 row, and both cross-refusal directions ran live (a64-rejects-rv
in-session above; rv-rejects-a64: `refused: machine 183 (aarch64 --
the /bina64 tenant)`).  `check_arm64_claims` 68 (+8).

#### Test gate

- `a64_shell_smoke.sh` 15/15 (prompt, builtins, nested spawn
  round-trip, named cross-refusal, PSCI ending, the flood fuse) ✓;
  rv-rejects-a64 measured live ✓; siblings green: a64 boot + image
  smokes, rv shell smoke, x86_64 boot 17/17, i386 builds ✓.

#### Deliverable

`patches/A64_A5c_shell.patch`

---

### Phase A6 — The sweep, fourth backend: DAIF behind the contracts ✅ COMPLETE

**Objective:** prove the D6 thesis — the four irqflags contracts close
over a fourth ISA with zero portable-file edits.

#### Tasks

- [x] `kernel/arch/aarch64/irqflags.h`: `arch_irq_save/restore` via
      DAIF (`mrs daif` / `msr daifset, #2` — note: no single-
      instruction read-and-mask like `csrrc`; the two-instruction
      window is safe because an interrupt taken between them is not
      lost, merely early — comment explains, mirroring the pushfq;cli
      note), `arch_wait_for_interrupt` = `wfi`, `arch_cpu_relax` =
      `yield`.
- [x] `kernel/arch/arch.h`: the third `#elif` becomes a fourth; the
      "one contract, N backends" comment updated to stop counting.
- [x] Ratchet 4 re-measured: if the port needed zero portable-file
      edits, that *is* the phase result headline; any residue paid
      down is listed file-by-file. Baselines only go down.
- [x] `test_width_sweep.sh` gains the a64 compile lanes (the V6
      pattern: portable files compiled `--target=aarch64` with asm
      forbidden outside arch/).

#### Result

**Zero portable files needed edits — the headline number is 0.** The
fourth backend went in without touching a single file outside
`kernel/arch/` and `tests/`: all four ratchets sit exactly at
baseline after the phase — casts 359/359, x64-includes 69/69,
cross-arch 0, asm-files 29/29. Nothing to pay down file-by-file
because the V6 sweep already left portable code speaking the four
contracts; aarch64 only had to answer them.

The backend itself (`kernel/arch/aarch64/irqflags.h`): `arch_irq_save`
is honestly TWO instructions (`mrs daif`; `msr daifset, #2`) — aarch64
has no `csrrc`-style read-and-mask, and the header documents why the
window is safe (an interrupt inside it is delivered *early*, never
lost, and the mask lands before the caller's next instruction — the
pushfq;cli argument, DAIF spelling). `arch_wait_for_interrupt` is
`msr daifclr, #2; wfi` — the *exact* two-instruction sequence A5c's
prompt-flood debugging measured its way to; A6 makes it the contract
instead of an inline idiom, and `cons_a64_readline` now calls
`arch_wait_for_interrupt()` on the shell's blocking read path. The
backend is EXECUTED under the 15-assert shell smoke, not just
compiled (a64_shell_smoke re-run: 15/15, fuse intact at 4933 bytes).
`arch_cpu_relax` = `yield`. The saved-flags polarity flip (DAIF.I set
= masked, where IF/SIE set = enabled) is documented as one more
reason no portable file may peek at the bits.

`arch.h` grew the fourth `#elif` in BOTH blocks: irqflags forwards to
the DAIF backend, and port I/O gets the aarch64 copy of the riscv
fence — `inb`..`outl` declared `unavailable` with the message naming
virtio-mmio (A7) as the device route. The comment stopped counting
backends: the count was the only thing in the file that needed
editing when aarch64 arrived, which is the D6 thesis proving itself.

The byte-identity control, upgraded from V6's whole-`.text` diff to
object granularity because the link is not deterministic here
(`kernel.c` prints `__DATE__ __TIME__`): two clean x86_64 builds,
base vs A6, all 129 objects compared — **128 byte-identical; the one
difference is kernel.o, exactly 3 bytes, all inside the banner
timestamp string, its .text byte-identical**. The fourth branch
changed zero generated x86 code, measured at the only granularity
where the claim is falsifiable.

`test_width_sweep.sh` gained four a64 lanes, all green: (1) the
boot_info offset contract compiles at the fourth width (LP64 again —
must agree with AMD64/rv64 by _Static_assert); (2) an irqflags probe
TU exercising all four contracts through `arch.h`, compiled once per
target — x86_64, i386, rv64, a64 — so a drifting backend signature
turns exactly one lane red; (3) the NEGATIVE control: a TU calling
`inb()` must *refuse* to compile at aarch64 and the error must name
the virtio-mmio route (a fence that silently stubs is the xHCI
lesson); (4) zero `__asm__` reaches the a64 compile of the
`KERNELA64_SHARED` files — checked on the *preprocessed* output,
because a textual grep would lie about string.c's `#ifdef`-fenced x86
fast paths, and with the file list read from the Makefile variable so
the lane cannot drift from the build.

One measured drift found and fixed while re-running the full gate:
`check_riscv_claims.py`'s V8 mkinitrd claim matched `audit_tenant`
lines with exact spaces, and A5b's column realignment (when `/bina64`
joined the audit) had silently broken it — caught by the first full
`make test-unit` since the A5c string edits, fixed by making the
claim whitespace-tolerant (the in-phase checker-fix D6 prescribes).
Full regression sweep after: test-unit green end-to-end, x86_64
boot-to-shell 17/17, rv_boot + rv_shell green, a64_boot + a64_image +
a64_shell green, `make iso` clean.

#### Test gate

- All four ratchets at-or-below baseline; the sweep's a64 lanes green;
  the phase result states, with a number, how many portable files
  needed edits (the thesis predicts 0).

#### Deliverable

`patches/A64_A6_sweep.patch`

---

### Phase A7 — Drivers: virtio-mmio (shared transport), blk, net, PL011 RX ✅ COMPLETE

**Objective:** storage and network through the *promoted* transport
(Fact 6), interrupt-driven console RX.

#### Tasks

- [x] Promote `kernel/arch/riscv64/virtio_mmio.c` → `kernel/drivers/
      virtio_mmio.c` behind the same hook treatment as the A1 walker
      — **[AMEND-1] and the x86_64/i386 source-list exclusions learn
      the new directory IN THE SAME PATCH** (the A0 find(1) lesson,
      this time predicted: the gate builds all four kernels);
      the rv64 kernel switches to the shared copy in-patch; claim
      check asserts single-object linkage. The legacy-vs-modern lesson
      (`-global virtio-mmio.force-legacy=true`) carries over verbatim
      and stays in the QEMU lines.
- [x] a64 wiring: Device-nGnRE mappings asserted (A3's MAIR index —
      the transport refuses to attach over a Normal mapping; Fact
      5.2's bug class prevented by refusal, not convention), GICv2
      INTIDs from A1's normalisation, contiguous-vring PMM discipline
      inherited (adjacency-checked, fail loudly).
- [x] `vblk`/`vnet` on a64: the rv64 driver pair's shape over the
      shared transport; miniproto parity strings kept identical so
      the smoke assertions are shared text.
- [x] **[AMEND-3]** PL011 TX through `drivers/uart/uart_ring.h` (the
      O3 pure index core, already host-tested) — same shape, PL011
      registers;
- [x] PL011 RX: IRQ-driven (SPI 1 → INTID 33), `IMSC`/`ICR` handling;
      the blocking-read path re-tests the cleared-interrupt-mask
      deadlock lesson (I7's, already re-tested in V7 — the DAIF twin
      this time).

#### Result

**The promotion.** `kernel/drivers/virtio_mmio.c` is the rv64
transport with its arch reach-outs (pmm_rv_alloc_frame, p2v_rv,
sbi_puts, rv_rdtime) replaced by a `vmmio_arch_ops` table — the fdt.c
treatment, one source file, and BOTH MMIO kernels link the shared
copy (`KERNELRV_SHARED` and `KERNELA64_SHARED` both list it; the
kernel/arch/riscv64/ copy is deleted, not forked).  [AMEND-1] paid
exactly as predicted: `kernel/drivers/` joined the x86_64 find(1)
exclusion list in the same patch (the i386 list needed nothing — it
is a positive `find` over its own arch directory plus an explicit
shared list, measured, not assumed), and the gate built all four
kernels first try.  The promoted file is portable code now, and the
ratchets prove it stayed clean: `fence rw, rw` became
`__atomic_thread_fence(SEQ_CST)` (the compiler lowers it back to that
very fence on rv64, `dmb ish` on a64), and all four counters sit at
baseline — casts 359/359, x64-includes 69/69, cross-arch 0, asm-files
29/29.  The rv64 non-regression gate: rv_parity_smoke 21/21 over the
shared object, rv_boot + rv_shell green.

**The a64 wiring, and the hook that is the port's whole point:** the
transport now REFUSES a window that is not Device-attributed.
`paging_a64_attr_index()` walks the live tables and reports the MAIR
index; `vmmio_a64.c`'s `mmio_is_device` requires `MAIR_IDX_DEVICE`
before the first register read (Fact 5.2's reordered/combined device
access — prevented by refusal, not convention).  The rv64 hook
returns 1 with the asymmetry documented where it lives: Sv39 without
Svpbmt has no per-PTE attribute bits to check — PMAs decide, and the
hook says so instead of faking a walk.  The contiguous-vring
adjacency check transfers unchanged and held (both PMMs walk their
bitmaps upward; the check is what makes that a contract).

**blk + net on the fourth tenant, measured on the first boot:**
virtio-blk attached legacy version 1 (the force-legacy lesson pinned
in the smoke's QEMU line AND asserted in the banner), 8192 sectors —
known-bytes sector-0 read + write/readback/restore on LBA 8191,
PASS.  virtio-net: MAC 52:54:00:12:34:56 from config space, then the
SHARED miniproto — the THIRD consumer — took DHCP (10.0.2.15), ARP,
and a payload-verified ICMP echo, pinging as `auralite-a64-ping`
(each tenant pings as itself so a crossed wire is visible).  Log
strings are vnet_rv.c's byte for byte, so the smoke assertions are
shared text by construction.

**The console went interrupt-fed both ways.**  [AMEND-3]: PL011 TX
rides `drivers/uart/uart_ring.h` — the O3 index core, host-tested
across wrap/full/empty and the 2^32 crossing before it ever saw this
UART; the file adds only the PL011 spelling of "FIFO has room"
(FR.TXFF) and "interrupt me when it drains" (IMSC.TXIM), with the
sync path kept for early boot and panic (uart.c's rule).  The ring
manipulation runs under `arch_irq_save/restore` — the A6 DAIF
backend earning its keep a week after it landed.  PSCI SYSTEM_OFF
now drains the ring first: a power-off with ringed-but-unsent bytes
would eat the log tail where every smoke assertion lives.  RX: IRQ
33 (SPI 1, A1's normalisation, taken from the DTB not hardcoded),
IMSC.RXIM + receive-timeout, the handler drains ALL pending bytes
per claim (the V2 level-line lesson) into a second ring;
`pl011_try_getc()` pops it, and the A5c polled read remains as the
pre-GIC fallback.  The receipt is counted and printed —
`rx bytes via GIC irq: 11` for the smoke's 11-keystroke session
(`uname\n` + `exit\n`), a number a poll-fed session would leave 0.

**Gates:** the new a64_drivers_smoke — 15 assertions incl. the
log-size fuse, the force-legacy pin, both driver PASSes, the IRQ
receipt, no-FAIL/no-UNHANDLED — green; a64_shell_smoke re-pinned to
the A7 ending and grew the receipt assert (15 green lines incl. the
fuse); a64 boot/image green; rv_parity
21/21, rv boot/shell green; x86_64 boot-to-shell 17/17; test-unit
end-to-end; `check_arm64_claims` 80 (+7); check_riscv_claims' V7
transport claim follows the promoted file (in-flight checker edit,
D6's rule).

#### Test gate

- blk read/write + net echo under the a64 kernel; PLIC-receipt-style
  RX assertion for the PL011; rv64 suite green on the shared
  transport (the promotion's non-regression gate).

#### Deliverable

`patches/A64_A7_drivers.patch`

---

### Phase A8 — Parity: storage, network, full crypto, fourth tenant ✅ COMPLETE

**Objective:** the I8/V8 gauntlet on aarch64 — one boot, every
subsystem asserted; the crypto suite EXECUTED on the target ISA.

#### Tasks

- [x] `a64_parity_smoke.sh`: the V8 shape — one QEMU run from banner
      through drivers, `assert_no_grep FAIL` over the whole log.
- [x] **Full libatls at aarch64**: cross-compiled
      `aarch64-linux-gnu-gcc -static` and EXECUTED under
      `qemu-aarch64` (both measured present; Fact 5 has the `__int128`
      execution receipt), with the compile-only fallback + loud SKIP
      when the cross toolchain is absent. `libc6-dev-arm64-cross`
      named in the dep documentation (Fact 1's measured miss).
      **[AMEND-6]** the gate `command -v`'s the cross-gcc AFTER the
      install step — silent apt dependency failures were measured
      three times during the OPT audit ("Setting up" printed, binary
      absent).
- [x] The initrd's fourth tenant audited end-to-end (A5's audit is the
      pack-time gate; A8 re-asserts at boot: each of the four kernels
      exec-refuses the other three tenants' binaries).
- [x] Status-matrix rows drafted for A9.

#### Result

**The parity boot: 26 green lines in one run.**  `a64_parity_smoke.sh`
boots the Image path with the full device set and asserts one gate
per phase — A0 banner, A1 handoff, A2 isr+timer/GIC, A3
pmm/vmm/heap with BOTH alignment polarities (the Device-faults /
Normal-succeeds pair only this arch measures), A4
sched/fpu/EL0/`A64-U-OK!`, A5 init-exits-7 + the shared shell, A7
blk+net+the IRQ receipt — then `assert_no_grep FAIL` over the whole
log, the A5c log-size fuse first (5266 bytes, intact), the PSCI
ending, and the standing x86_64 no-regression pair (the four-tenant
tar must not break the first tenant's boot; it did not).

**The refusal matrix got its fourth row, live.**  One shell session
runs all three foreign tenants and the a64 kernel refuses each the
way the contract demands: `/bin/init` → `refused: machine 62 (x86_64
-- wrong kernel)`, `/bin32/init32` → `refused: not ELFCLASS64` (the
i386 tenant falls at the class check, before machine is even read),
`/binrv/init` → `refused: machine 243 (riscv64 -- the /binrv tenant,
wrong kernel)`.  With V8's and A5c's assertions this completes the
measured half of the 4×3 matrix that matters at boot: every kernel
that can be handed a foreign tenant's binary in a smoke refuses it
by name, not by crash.

**Crypto: the COMPLETE suite EXECUTED at aarch64.**
`test_libatls_a64.sh` — the rv64 gate's fourth spelling:
`aarch64-linux-gnu-gcc -static` + `qemu-aarch64`, all five suites
(hash/AEAD/X25519/Ed25519/ECDSA) pass EXECUTED, making aarch64 the
second LP64 tenant through the `__int128` path the -m32 boundary
cannot reach — umulh here where rv64 lowers to mulhu, which is
exactly why execution and not compilation is the gate (a wide-multiply
bug produces plausible-looking wrong field elements).  The deps are
named in the gate (`gcc-aarch64-linux-gnu`, `libc6-dev-arm64-cross`,
`qemu-user` — Fact 1's measured miss), the compile-only fallback
SKIPs loudly, and **[AMEND-6]** is recorded in the gate itself: the
`command -v` is the truth, an installer's exit status is not a
binary's existence (A9's CI job must repeat the check after its
install step).  Registered in `make test-unit` beside the rv64 gate.

**Status matrix, drafted for A9** (the V8 table + the fourth column;
A9 lands it in the docs):

| Subsystem | x86_64 | i386 | riscv64 | aarch64 |
|---|---:|---:|---:|---:|
| Boot path | ✅ BIOS+UEFI ISO | ✅ same ISO, refusal-gated | ✅ OpenSBI `-kernel` | ✅ ELF `-kernel` + Image protocol (A5a) |
| Memory (PMM/VMM/heap) | ✅ | ✅ (no PAE ⇒ no NX) | ✅ Sv39 | ✅ TTBR1 39-bit, W^X twice over |
| W^X enforced | ✅ NX | ❌ honest (D3) | ✅ PTE X-bit, loader refuses W+X | ✅ PXN/UXN, loader refuses W+X |
| Threads/sched | ✅ SMP | ✅ BSP-only | ✅ boot-hart only (D5) | ✅ boot-CPU only (D5, PSCI ramp named) |
| User mode + syscalls | ✅ SYSCALL | ✅ int 0x80 | ✅ ecall (D4 numbers) | ✅ svc #0 (D4 numbers) |
| Userspace | ✅ full libc | 🚧 libc32 subset | 🚧 libcrv subset (V8 residue) | 🚧 libca64 subset (same residue class) |
| Shell | ✅ init shell | ✅ smallsh (shared) | ✅ smallsh (same source) | ✅ smallsh (same source, fourth build) |
| Storage | ✅ AHCI+virtio-PCI | ✅ ATA PIO | ✅ virtio-mmio blk | ✅ virtio-mmio blk (PROMOTED transport) |
| Network | ✅ e1000+virtio stack | ✅ e1000 miniproto | ✅ virtio-mmio + shared miniproto | ✅ virtio-mmio + shared miniproto (third consumer) |
| Crypto vectors | ✅ host suite | 🧪 symmetric only (-m32 boundary) | ✅ COMPLETE suite executed | ✅ COMPLETE suite executed (umulh edition) |
| Console input | ✅ PS/2+serial ring | ✅ PS/2+serial ring | ✅ 16550/PLIC irq ring | ✅ PL011/GIC irq ring (O3 core both ways) |

Residue, recorded not hidden: the full libc port (errno/TLS/malloc
over mmap) stays a named non-goal exactly as it does for rv64; PIE
loading waits on it; fw-cfg self-test knob is AMEND-5's named
deferral; SMP is D5's named ramp on both DTB tenants.

**Gates:** a64_parity 26/26; atls-a64 5/5 EXECUTED; a64
shell/drivers/boot/image green; rv_parity 21/21; x86_64 17/17;
test-unit end-to-end with the new gate registered;
`check_arm64_claims` 85 (+5).

#### Test gate

- Parity smoke green in one boot; `[atls-a64] PASS: the COMPLETE
  suite` with EXECUTED provenance; all other arch suites green.

#### Deliverable

`patches/A64_A8_parity.patch`

---

### Phase A9 — CI matrix, docs, the claim check ✅ COMPLETE

**Objective:** the fourth line in the matrix; the docs tell the truth;
the plan closes only through checker arithmetic.

#### Tasks

- [x] `.github/workflows/integration.yml`: `aarch64-parity` job — the
      riscv-parity shape: measured dep list (**no `gcc-multilib`**,
      same Conflicts, same job-separation rule; **with
      `libc6-dev-arm64-cross`**, Fact 1), assert-not-assume tool
      checks, artefact-first order (`/bina64` grepped from the tar
      before any boot), width sweep, EXECUTED crypto gate, all claim
      checkers + selftests, smokes, logs-on-failure.
- [x] `docs/status.md`: the aarch64 section, by-design entries naming
      their decisions (D1 arm32/EL2 refusals, D7 PCIe deferral); the
      Rust row honest, fourth edition.
- [x] `docs/architecture.md`: the fourth boot diagram (QEMU ELF load →
      EL1 `_start` → DTB-at-RAM-base → MMU high half); "four kernels,
      no shared binary artefacts — only contracts" updated from three.
- [x] `docs/syscall_abi.md`: the `svc #0` section — one number table,
      four trap mechanisms.
- [x] `README.md`: the fourth boot-path row (`make kernela64 && make
      run-a64`).
- [x] `check_arm64_claims.py` closed out to full phase coverage;
      terminal Status arithmetic (armed since A0) satisfied by turning
      the last table row green, not by editing prose.

#### Result

**The plan closes the way it opened: through arithmetic.**  The D8
checker has counted table rows against COMPLETE headings since A0,
learned the sub-phase grammar at the A5 split, and now closes at the
terminal condition — `## Status: COMPLETE` is accepted only when all
12 phase rows are green, and this paragraph could not be written
before they were.  Final count: **90 claims** verified against the
tree, selftest detecting a doctored ROOT, wired into `make test-unit`
and both parity CI jobs.

**CI: the fourth matrix line.**  `aarch64-parity` is riscv-parity's
shape with the measured lessons carried over: no `gcc-multilib` in
the dep list (the Conflicts field, same job-separation rule),
`libc6-dev-arm64-cross` named explicitly (Fact 1 — the cross gcc
alone leaves the static crypto gate dying at `<string.h>`), and
**[AMEND-6]** as its own step: `command -v aarch64-linux-gnu-gcc`,
`command -v qemu-aarch64` and a literal `test -e` on the cross libc's
`string.h` AFTER the install — an installer's exit status is not a
binary's existence, measured three times.  Artefact-first order:
`/bina64/{init,smallsh}` grepped from the tar and both kernel
artefacts `test -s`'d before any boot; then the width sweep with the
fourth-width lanes, the EXECUTED crypto gate, the claim checker +
selftest, and all five a64 smokes (boot/image/shell/drivers/parity),
serial logs uploaded on failure.

**Docs: the truth, restated where people read it.**  `docs/status.md`
grew the ARM section — every ✅ tied to the gate that measures it,
every ❌ naming its decision (D1: arm32 and EL2 entry refused with a
banner, the EL2 case parking in `wfi` because an `hvc` from EL2 traps
into our own empty vectors; D7: PCIe ECAM measured, named, deferred),
the deferrals honest (AMEND-5 fw-cfg, the libc residue class, the
Rust row's fourth edition: `aarch64-unknown-none` exists, porting
stays a follow-up's opening fact).  `docs/architecture.md` carries
the fourth boot diagram — both entry paths (ELF `-kernel` with the
DTB parked at the RAM base and x0 measured NOT the DTB; the Image
header with x0 magic-verified) — and the closing paragraph now reads
**four kernels, no shared binary artefacts, only contracts**, with
the contract list grown to name the promoted single-source files.
`docs/syscall_abi.md` documents `svc #0`: one number table, FOUR trap
mechanisms, the x8/x0..x4 convention, and the A5c `SP_EL0` lesson
pinned in the ABI doc because that is where the next port will look
first.  `README.md` gained the fourth boot-path row.

**The measured facts of this port, restated at the close (the
RISCV_PLAN tradition):** x0 at ELF entry is NOT the DTB pointer — the
DTB sits at the RAM base, and only the Image protocol delivers it in
x0 (Fact 2, load-bearing since A5a).  CNTFRQ is a register, not a DTB
field (Fact 2.3).  Pre-MMU memory is Device-nGnRnE and unaligned
loads FAULT there while succeeding on Normal WB — both polarities
carry standing gates (Fact 5.1).  Device registers behind Normal
mappings are a bug class, prevented since A7 by attach-time refusal,
not convention (Fact 5.2).  The GIC banks SPI+32/PPI+16 — normalised
in ONE place, the walker, and nowhere else (A1).  The trap frame does
not carry SP_EL0 (A5c, paid for in a 20 MB prompt flood, pinned in
three smokes and the ABI doc).  DAIF's polarity is inverted relative
to IF/SIE — documented in the one header allowed to look (A6).  And
the D6 thesis held to the end: the fourth ISA cost **zero
portable-file edits**, all four ratchets sit exactly where V6 armed
them — 359/69/0/29.

**Gates at close:** a64_parity 26/26, atls-a64 5/5 EXECUTED, all five
a64 smokes green, rv_parity 21/21, x86_64 17/17, i386 suite green,
test-unit end-to-end, `check_arm64_claims` 90/90 + selftest, terminal
arithmetic satisfied: 12 rows, 12 COMPLETE headings, one Status line.

**Post-close hotfix (2026-08-21, same day).**  The first four-job CI
run (run 32467266053) was read the measured way — jobs API + the
failure artifact — and taught three lessons, none of them new code:

1. *riscv-parity, crypto step*: red since the job existed — the dep
   list never named `libc6-dev-riscv64-cross`, and the runner image
   does not pull it transitively (the EXACT mechanism AMEND-6
   predicted, `<string.h>` edition; this sandbox measured the same
   miss during its own restores).  Fixed: the dep named, plus the
   AMEND-6 existence-assert step back-ported to the job that taught
   the lesson.
2. *qemu-integration, host unit tests*: red since V8 — the crypto
   gates' compile-only fallback trusts a bare clang triple to search
   `/usr/include`, which is a clang-version accident (`-none-elf`
   never does; reproduced locally).  Fixed in BOTH fallbacks: the
   hosted surface is four prototypes, now stubbed into
   `build/atls_stub_include/` — the fallback checks layout, not
   glibc.
3. *aarch64-parity, drivers smoke*: every gate in the artifact's
   serial log is GREEN; `timeout 60` killed QEMU at `auralite# un` —
   the prompt takes ~55 s under shared-runner TCG.  Fixed: the a64
   session smokes now treat the timeout as an upper fence (180 s),
   not a schedule — QEMU still ends by PSCI, so green runs pay
   nothing.

The `Run QEMU integration tests` step's failure predates this plan
entirely (red since at least I6, 2026-08-16, duration 2 h 03 m at the
V7 run) and is tracked separately — the full 129-case suite re-run
locally to split tree-truth from runner-truth.

#### Test gate

- The full four-arch matrix green in CI; every A-claim verified;
  `RISCV_PLAN`-style closing result with the measured-facts restated.

#### Deliverable

`patches/A64_A9_ci.patch`

---

## 4. What this plan deliberately does not do

- **arm32 / AArch32.** Refused in D1 with reasons.
- **EL2/EL3, secure world, virtualisation.** The kernel is an EL1
  citizen; `CurrentEL != EL1` gets a refusal banner, not support.
- **PCIe ECAM.** Present on the board, measured, deferred (D7).
- **SMP.** Boot CPU only; `PSCI CPU_ON` is the named exit ramp (D5).
- **Real hardware.** QEMU `virt` only, as with riscv64; a Raspberry Pi
  bring-up would be its own plan with its own firmware facts (the
  GPU-boots-first tradition deserves its own §1, not a footnote here).
- **Rust userspace.** Same recorded stretch as rv64
  (`aarch64-unknown-none` measured present); it lands, if it lands,
  as a follow-up spanning both ports.

---

## 5. Order of battle and the yardstick

A0 → A1 → A2 → A3 → A4 → A5 → A6 → A7 → A8 → A9, one patch each,
`git am`-clean in sequence on top of the completed RISC-V series.
Every phase leaves all other architectures' suites green — the
promotions in A1/A7 make this a *stronger* statement than it was in
the V-series: a shared-file regression now breaks a sibling arch's
gate immediately, by construction.

The yardstick, inherited and sharpened: **measured, not assumed** —
the stub booted before the plan was written; the DTB-not-in-x0 trap,
the EL1 entry, the PSCI power-off, the 62.5 MHz clock, the
Conflicts line, and the missing-cross-libc failure are all receipts
in §1, not predictions. Phases that discover the plan wrong record
the deviation in their Result section and adjust the successors —
the I3/V0 tradition. The plan is COMPLETE when the checker's
arithmetic says so, and not before.
