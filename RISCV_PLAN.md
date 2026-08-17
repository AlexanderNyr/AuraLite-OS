# AuraLite OS — RISC-V (rv64gc) Support Plan

## Status: IN PROGRESS 🚧 — V0–V8 complete (phases V0–V9)

| Phase | Result | Deliverable |
|-------|--------|-------------|
| V0 — toolchain gates + the S-mode stub | ✅ complete | `patches/RV_V0_boot.patch` |
| V1 — `boot_info_t` from the Device Tree | ✅ complete | `patches/RV_V1_dtb.patch` |
| V2 — traps, timer, PLIC | ✅ complete | `patches/RV_V2_traps.patch` |
| V3 — memory: Sv39, PMM, heap — and W^X back | ✅ complete | `patches/RV_V3_mm.patch` |
| V4 — threads, scheduler, U-mode, `ecall` | ✅ complete | `patches/RV_V4_proc.patch` |
| V5 — userspace: libc-rv, init, the shared shell | ✅ complete | `patches/RV_V5_user.patch` |
| V6 — the inline-assembly sweep | ✅ complete | `patches/RV_V6_sweep.patch` |
| V7 — drivers: virtio-mmio, blk, net, UART RX | ✅ complete | `patches/RV_V7_drivers.patch` |
| V8 — parity: storage, network, full crypto | ✅ complete | `patches/RV_V8_parity.patch` |
| V9 — CI matrix, docs, the claim check | pending | `patches/RV_V9_ci.patch` |

This document answers:

> *AuraLite now boots two x86 kernels from one image. What is the honest,
> incremental path to a third architecture that shares no instruction set
> with the first two — and what does the tree's portability machinery
> (the I6 ratchets, `paddr_t`, `arch.h`, the shared smoke-test shapes)
> actually buy when the ISA changes rather than the pointer width?*

It follows the structure of the existing plans (`I386_PLAN.md`,
`FIXES_PLAN.md`, `WIN32_PLAN.md`, `USB_PLAN.md`): dependency-ordered
phases, a definition of done and a test gate for every phase, one
`.patch` per phase.

**Baseline:** the current `main` tree, on top of the completed
`I386_PLAN.md`. This plan **inherits** that plan's decisions where they
transfer (D4 one-syscall-table, D6 typing rules, D7 ship-the-first-thing
-alone) and says so per phase instead of re-arguing them.

`I386_PLAN.md` §7 called the i386 port "the cheapest available audit of
the claim that the kernel core is portable freestanding C". It was — and
it was also an *easy* audit, because i386 shares x86's instruction set,
byte order, port-I/O model and firmware tradition. RISC-V shares none of
those. This plan is the audit the i386 port could not perform: the one
where every `lock cmpxchg`, every `outb`, and every assumption that
"firmware means BIOS or UEFI" has to stand up and identify itself.

---

## 1. Where things actually stand

Measured against the tree and against QEMU on this machine, not assumed.
Five facts shape this plan.

### Fact 1 — The toolchain is already here, Rust included this time

Verified on the build environment:

```
$ clang --target=riscv64 -march=rv64gc -ffreestanding -c t.c
    # ELF 64-bit LSB relocatable, UCB RISC-V, RVC, double-float ABI ✓
$ ld.lld -m elf64lriscv                                            ✓
$ rustup target list | grep riscv64
    riscv64gc-unknown-none-elf                                     ✓
$ apt show qemu-system-misc     # ships qemu-system-riscv64        ✓
```

Clang and LLD cross-compile rv64 out of the box; `REQUIRED_TOOLS` does
not grow (the one new package, `qemu-system-misc`, is a test-time
dependency like `qemu-system-x86`). And the gap that made Rust
impossible on i386 **does not exist here**: `riscv64gc-unknown-none-elf`
is a tier-2 rustup target, so `rustes` and the `rsbr` bridge are
portable in principle — recorded as V9 stretch scope, not promised.

A minimal S-mode kernel was linked at `0x80200000` and booted with
`qemu-system-riscv64 -machine virt -kernel` during fact-finding: OpenSBI
v1.6 (bundled with QEMU) initialises, prints its banner, and jumps to
the ELF entry. The boot path exists before the plan does.

### Fact 2 — The porting cost moved: it is inline assembly now, not pointer width

RV64 is LP64 — the same `sizeof(long) == sizeof(void *) == 8` model as
x86_64. The entire I6 width battle (877 estimated sites, 361 measured,
the `-malign-double` ABI bug) **does not recur**: `paddr_t` is naturally
64-bit, `boot_info_t` compiles to the same layout, and the width
ratchets stay green by construction.

What recurs instead, measured:

- **33 portable files** (kernel/ + drivers/, `kernel/arch/` excluded)
  contain x86 inline assembly. Not arch code — *portable* code:
  `spinlock.c` (`lock cmpxchg`), `scheduler.c`/`vfs.c`/`gui.c`
  (`cli`/`sti`/`hlt` interrupt gating), `kprintf.c`, `rng.c`
  (`cpuid`/`rdseed`), `time.c` (`rdtsc`), and every polling loop that
  spells `pause`.
- **6 portable files** use port I/O (`outb`/`inb`) — an instruction
  class that **does not exist** on RISC-V. Everything is memory-mapped.
- **24 NASM files** are x86-only by definition (the i386 port could
  share the *dialect*; RISC-V cannot).

The i386 sweep's shape transfers exactly — a counted residue, a CI
ratchet that only goes down, arch headers absorbing sites batch by
batch — but the *subject* is new: V6 is `atomic.h`/`barrier.h`/
`cpu_relax()` where I6 was `paddr_t`/`uintptr_t`.

### Fact 3 — The QEMU `virt` machine's device world, from its own mouth

The DTB was dumped and decompiled during fact-finding
(`-machine virt,dumpdtb=`):

| Device | Where | What it means for the port |
|---|---|---|
| `ns16550a` UART | MMIO `0x10000000` | **The same 16550 programming model as COM1.** `uart.c`'s register offsets, FIFO setup and LSR polling carry over verbatim; only the access method changes (MMIO load/store instead of `in`/`out`). The first console is a recompile plus an access-shim, not a new driver. |
| `virtio,mmio` ×8 | `0x10001000`–`0x10008000` | The native disk/NIC transport. The tree's virtio-blk/net drivers exist **but speak virtio-PCI** (measured: `virtio_pci_common_cfg`, `pci_find_device` in both). The virtqueue logic is transport-independent; V7's work is an mmio transport underneath it, not a rewrite. |
| PLIC | MMIO | External interrupt routing — the IOAPIC-shaped problem, but documented in one spec and one DTB node. |
| CLINT / `sstc` | MMIO / CSR | Timer. Via SBI `set_timer` first (one `ecall`), native `stimecmp` later. |
| RAM | `0x80000000 + 256 MiB+` | Kernel loads at `0x80200000` (OpenSBI owns the first 2 MiB). |

No PS/2, no VGA text mode, no PIT, no PIC, no CMOS: **every** x86
bring-up device the first two kernels leaned on is absent. The console
story starts at the 16550 and stays there until virtio-gpu (out of
scope, §6).

### Fact 4 — The firmware tradition breaks, and pretending otherwise would be theatre

AuraLite's identity includes "no third-party bootloader anywhere in the
chain". On RISC-V that sentence needs honest re-examination. The
platform boot story is: M-mode firmware (OpenSBI — bundled with QEMU,
shipped in the mask ROM or SPI flash of every real board) brings up the
hart, stays resident as the **SBI** runtime (the Supervisor Binary
Interface: timer, IPI, hart management, console — the RISC-V analogue of
what BIOS interrupts were), and enters the kernel in S-mode with
`a0 = hartid`, `a1 = DTB pointer`.

Writing our own M-mode firmware is possible and is **refused** (decision
D2 below, with the argument). The plan's honest framing: OpenSBI is not
"a bootloader we chained" — it is the platform, exactly as SeaBIOS's
INT 13h was the platform under the BIOS MBR. AuraLite's *own* code still
owns everything from the entry point on, including the choice not to
need a Stage 2 at all: `-kernel` loads our ELF directly, and real boards
load it as the standard `fw_payload`/U-Boot payload.

### Fact 5 — The contracts survive even though the instruction set does not

What actually transfers from the two existing kernels, verified by
reading rather than hoped:

- **`boot_info_t`** is all fixed-width fields with a 64-bit
  `hhdm_offset` the kernel *validates rather than assumes* — the I3
  lesson that already paid once. V1 fills the same struct from the DTB;
  `kmain`'s contract ("a physical pointer to boot_info in a register")
  holds for the third time with a third register (`a1`-derived).
- **The syscall table** is trap-agnostic by I386_PLAN D4: one table,
  now three trap mechanisms (`SYSCALL`, `int 0x80`, `ecall`).
- **The self-test discipline** — `[pmm] PASS`, `[vmm] PASS`,
  `[sched] PASS`, negative controls, smoke families per phase — is
  ISA-independent and is the actual reason a third port is tractable.
- **`kernel/lib/bitmap.h`**, the USTAR reader shape, the ELF loader
  validation order, `libatls` — portable C with host tests.
- **W^X comes back.** Sv39 PTEs carry separate R/W/X bits: the property
  i386 lost to non-PAE (I386_PLAN D3) is enforceable again on RISC-V,
  and the plan restores the `elfperm`-class gates that i386 had to
  arch-condition away.
- **`__int128` exists on rv64** — the boundary that blocked
  X25519/Ed25519/P-256 at `-m32` (I8's measured finding) does not exist
  here. Full-suite crypto parity is a *test* on this arch, not a
  research project.

---

## 2. Decisions

### D1. rv64gc on the QEMU `virt` machine is the target; rv32 is not

`rv64gc` (RVA20-profile-shaped: IMAFDC + Zicsr/Zifencei) is what QEMU's
`virt` models, what every mainstream Linux-capable board implements, and
what both clang and rustup name as their bare-metal default. rv32 is the
i386 of this plan's world — a separate width port with a separate
audience — and it gets what x86 got: nothing is *broken* for it (the
plan's code refuses politely on a mismatched `misa`), and nobody
pretends it is supported.

QEMU is the gate, matching the repository's standing reality check. Real
hardware (VisionFive 2, LicheePi, the SBCs) is recorded as experimental
exactly as x86 real-hardware is.

### D2. OpenSBI is the platform, not a chained bootloader — and no own M-mode firmware

The one place this plan departs from the letter of the custom-bootloader
tradition, decided out loud. Writing an M-mode AuraLite firmware would
(a) re-implement hart bring-up, PMP setup and the SBI runtime that every
real board already ships in ROM, (b) not run on those boards *anyway*
(their mask ROM loads vendor SBI first), and (c) buy zero educational
novelty that the S-mode side doesn't already offer — trap delegation,
paging and drivers are all S-mode work.

What stays true to the tradition: **no third-party code decides what our
kernel does after entry.** No U-Boot scripts, no GRUB, no Limine
protocol; the kernel ELF *is* the payload, entered at `_start` with the
hart ID and a DTB, and everything after that instruction is this
repository's code. The SBI calls the kernel makes (console putchar
before the UART driver, `set_timer`, `hart_start` someday) are firmware
*services* used through a frozen public spec — the exact relationship
BIOS Stage 2 had with INT 13h.

### D3. Sv39, higher-half at the Sv39 canonical top, HHDM moves — by contract, not by grep

Three-level Sv39 paging (the guaranteed-available mode): 39-bit VAs,
512 GiB, 4 KiB/2 MiB/1 GiB pages. The kernel links higher-half; the
direct map lives at **`0xFFFFFFC000000000`** — because x86_64's
`0xFFFF800000000000` is not a canonical Sv39 address at all.

This is the I3 lesson collecting interest: `hhdm_offset` is a 64-bit
field the boot shim writes and the kernel validates. The constant moves
per-arch in exactly one producer and one consumer, and
`test_boot_info_width.c`'s cross-compile asserts keep the struct honest.
Nothing greps for `0xffff8000...` and nothing needs to.

W^X is enforced from the first user mapping (Fact 5): PTE X is real, the
user linker scripts' RX/R/RW segment discipline — which i386 kept "for a
PAE-capable future" — pays off on the *third* arch instead.

### D4. `ecall` from U-mode, AuraLite's numbers, Linux's register convention — inherited

I386_PLAN D4, third verse: the syscall *numbers* are the one shared
table; the trap is `ecall` with `a7` = number, `a0–a5` = arguments,
`a0` = return — the RISC-V Linux convention, chosen for the same reason
the i386 port chose Linux's i386 registers: every toolchain and every
reader already knows it. `stvec` points at one trap entry;
`scause`/`stval` split syscalls from faults; `sscratch` holds the
per-hart kernel stack pointer for the U→S stack switch (the TSS.esp0
lesson from I7, solved the way this ISA solves it).

### D5. Boot-hart only at first — inherited from D5, with the exit ramp named

The scheduler runs on the boot hart; secondary harts stay parked in
OpenSBI until someone calls SBI HSM `hart_start`. Same shape as i386's
BSP-only decision — a performance gap, not a correctness gap, recorded
in the matrix. The exit ramp is cleaner than x86's (no trampoline
real-mode dance; HSM hands a started hart straight to S-mode C code),
which is precisely why it can wait.

### D6. The sweep gets a fourth ratchet: inline assembly in portable code

The I6 machinery extends rather than forks. `check_width_sweep.py`
(renamed duties, same file) gains **ratchet 4: files in portable code
containing `__asm__`** — baseline 33, measured. The absorption targets
are three new arch-header contracts:

- `kernel/arch/arch.h` grows forwarding for `arch_irq_save/restore()`,
  `arch_wait_for_interrupt()`, `arch_cpu_relax()` — the `cli/sti/hlt/
  pause` quartet that accounts for most of the 33.
- `kernel/lib/spinlock.c` moves to C11 `<stdatomic.h>` (clang
  freestanding supports it on all three targets) with the x86 fast path
  kept only if a measured regression demands it — measure first.
- Port I/O consumers (6 files) route through the existing `arch.h`
  boundary; on RISC-V the header simply has no `outb` and the compiler
  enforces what the ISA enforces.

Rule inherited from I6: portable code that never runs on RISC-V keeps
its asm until it does; the ratchet guarantees the debt only shrinks.

### D7. virtio-mmio first; PCIe on `virt` is deferred

The tree's virtio-blk/net drivers carry working virtqueue logic wearing
a PCI transport. V7 splits transport from queue (`virtio_mmio.c` beside
the existing PCI probe) rather than duplicating the drivers — the same
one-implementation rule the w32 PE parser follows. QEMU `virt` does
expose PCIe ECAM, but mmio is the native path, needs no bus enumeration,
and its addresses arrive in the DTB the kernel already parses. PCIe
lands if/when a device that only exists on PCIe matters.

### D8. One repository discipline, three arches: the claim check ships WITH the plan's first phase

`check_i386_claims.py` shipped in the same phase as its plan's
completion and was proud of never having an unchecked day.
`check_riscv_claims.py` does one better: it lands in **V0** with three
claims (toolchain gate exists, stub boots, plan header matches the
table) and grows a claim per phase. A plan checked from birth cannot
drift at all.

---

## 3. Phases

### Phase V0 — Toolchain gates + the S-mode stub ✅ COMPLETE

**Objective:** the third architecture exists in the build system and a
banner proves the whole path: clang → lld → OpenSBI → our `_start` →
SBI console.

#### Tasks

- [x] `Makefile`: `kernelrv` target family (`--target=riscv64
      -march=rv64gc -mabi=lp64d -mcmodel=medany`, `ld.lld -m
      elf64lriscv`); `deps-check` learns the *optional* riscv tools the
      way it learned mingw (absent = riscv targets skip loudly, the
      x86 build is untouched).
- [x] `kernel/arch/riscv64/boot.S` (GNU as via clang — no NASM on this
      arch): park non-boot harts (`a0 != boot hart` → `wfi` loop), set
      up `sp`, clear `.bss`, save `a0`/`a1`, call `kmain_rv`.
- [x] `kernel/arch/riscv64/sbi.c`: the `ecall`-to-M-mode wrapper —
      `sbi_console_putchar` (legacy 0x01) and DBCN probe; the day-0
      output path before any UART driver.
- [x] `main_rv.c`: banner + hartid + DTB pointer echo, `wfi` idle.
      `kernelrv.ld`: `OUTPUT_ARCH(riscv)`, link at `0x80200000`
      (physical for now; V3 moves it higher-half — the same
      stub-then-grow shape I1→I3 used).
- [x] `tools/check_riscv_claims.py` (D8): eight claims (not three — the
      structural Status-vs-table checks the i386 checker ended with are
      installed here from day one), registered in `make test-unit`,
      `--selftest` included.
- [x] `tests/integration/rv_boot_smoke.sh`: `qemu-system-riscv64
      -machine virt -kernel build/kernelrv.elf`, assert the banner and
      the OpenSBI handoff line; skip cleanly when qemu-system-riscv64
      is absent.

#### Result

Delivered as specified, with one measured fact the plan did not
predict — and it cost the whole debugging session of this phase:

**OpenSBI ignores the ELF entry point.** fw_dynamic jumps to its
"Domain0 Next Address" — the *payload base*, `0x80200000` — not to
`e_entry`. The first link put `boot.o` last (find-order objects), so
`_start` landed at `0x802003d4`, `sbi_call`'s first instruction landed
at `0x80200000`, and the hart executed it with garbage arguments:
silence after the OpenSBI banner, then a reset loop fetching from the
mrom reset vector (`-d int` showed endless `fault_fetch @0x10000` and
zero traps in our range). The fix is structural, not an object-order
band-aid: `_start` lives in `.text.boot` and `kernelrv.ld` places
`KEEP(*(.text.boot))` first, so "first byte of the first segment" and
"`_start`" are the same fact by construction. A second footgun found
on the way: linking a bare `-Ttext=` probe without a full linker
script gave the program headers their own LOAD segment at the default
image base — which QEMU dutifully loaded over the reset-vector region.
Full linker script or nothing.

Measured under `qemu-system-riscv64 -machine virt` (OpenSBI v1.6):

```
Domain0 Next Address        : 0x0000000080200000
Domain0 Next Mode           : S-mode
 Hello from AuraLite OS kernel (riscv64)!
[kernel] AuraLite OS riscv64, RISCV_PLAN phase V0
[boot] boot hart: 0
[boot] DTB at phys 0x000000008fe00000
[boot] DTB magic OK (0xD00DFEED, big-endian read)
[kernel] V0 stub complete; shutting down (V1 adds the DTB -> boot_info walk)
```

The smoke test's 9 assertions pass, including `-smp 4` printing the
banner exactly once (the hart lottery parked the other three harts)
and the run ending by SBI shutdown rather than the timeout.
`build/kernelrv.elf` is 32K; the x86_64 and i386 suites are untouched.
The stub ends in `sbi_shutdown` rather than the planned `wfi` idle so
every smoke run exits in under a second — V2's timer work re-introduces
the idle loop when there is something to wake up for.

#### Test gate

- The stub banner appears on the SBI console under `-machine virt`;
  the smoke test's assertions include hartid and a non-NULL DTB
  pointer. The x86_64 and i386 suites are untouched (nothing shared is
  edited in V0 at all).

#### Deliverable

`patches/RV_V0_boot.patch`

---

### Phase V1 — `boot_info_t` from the Device Tree ✅ COMPLETE

**Objective:** the third producer of the one handoff struct: a
flattened-device-tree walk fills `boot_info_t`, and `kmain_rv` starts
consuming the same contract `kmain` and `kmain32` consume.

#### Tasks

- [x] `kernel/arch/riscv64/fdt.c`: a minimal FDT parser — header
      validation (magic `0xD00DFEED`, big-endian fields: **the one
      place byte order bites on this port**, called out in the code),
      `/memory` reg → `mmap[]`, `/chosen` initrd properties, UART and
      virtio node addresses recorded for V2/V7. No full libfdt; the
      four properties the kernel needs, bounds-checked.
- [x] The shim writes `hhdm_offset = 0xFFFFFFC000000000` (D3),
      `boot_from_uefi = 0`, magic last (a partially-filled struct must
      never carry a valid magic — same ordering Stage 2 uses).
- [x] Initrd: QEMU `-initrd` puts the USTAR archive in memory and its
      range in `/chosen`; the shim translates to
      `initrd_phys/initrd_size`. One archive, three kernels: rv64
      binaries go under `/binrv` beside `/bin32` (the I5 layout rule,
      third tenant — the *rule* is adopted here; the directory gains
      its first tenants in V5, when rv64 binaries exist to put in it).
- [x] `test_boot_info_width.c` grows the third compile:
      `--target=riscv64` must produce the same offsets (LP64 — it
      does; the assert makes "does" permanent).

#### Result

Delivered as specified. `fdt.c` is ~360 lines, single-pass,
bounds-checked against `totalsize`, with a per-depth
`#address-cells`/`#size-cells` stack (reg decodes with the *parent's*
cells — the spot most hand-rolled FDT walks get wrong). Beyond the
planned four properties the same pass collects `/cpus` (hart count +
hartids into `cpus[]`), `/reserved-memory` carve-outs and the spec's
memory-reservation block into `mmap[]` as `BOOT_MEM_RESERVED`, and
`/chosen bootargs` — each was one `streq` inside a walk that already
existed. The kernel image self-reports `[__kernel_start,
__kernel_end)` (new linker symbols) as `BOOT_MEM_KERNEL`; the initrd
range and the DTB itself are typed too, so V2's allocator inherits an
mmap with no untyped occupied RAM.

Measured on `-machine virt -m 256M` (OpenSBI v1.6):

```
[boot] handoff magic OK, path=SBI, boot_info filled from DTB
[mm]   HHDM offset: 0xffffffc000000000 (Sv39 direct map; the V3 contract, satp=0 today)
[mm]   0x0000000080000000 + 0x0000000010000000  usable
[mm]   0x0000000080200000 + 0x0000000000030010  kernel
[mm]   mmap entries: 5, usable RAM: 256 MiB
[hw]   uart: 0x0000000010000000
[hw]   plic: 0x000000000c000000
[hw]   virtio-mmio windows: 8
```

— the UART/PLIC/virtio numbers are exactly the plan's Fact 3 dump,
now discovered by the kernel instead of asserted by the plan. With
`-initrd build/initrd.tar -append ...`: the /chosen range translates
to `initrd_phys/initrd_size` byte-exact (8632320 bytes), bootargs
echo back. With `-smp 4`: `harts: 4`, boot hart from `a0`. The smoke
test grew from 9 to 21 assertions; the width contract compiles at
`--target=riscv64` (third width in `test_width_sweep.sh`, claim
V1-4 in the checker — now 13 claims). Errors are named
(`FDT_ERR_MAGIC/VERSION/BOUNDS/TRUNCATED`), printed, and end in a
clean shutdown: a silent boot was V0's failure mode and once was
enough.

#### Test gate

- Boot log: `handoff magic OK`, a non-zero mmap from the DTB, RAM
  total matching `-m`, initrd found when passed. The width contract
  compiles at all three targets; i686-without-`-malign-double` still
  refuses (the negative control keeps its teeth).

#### Deliverable

`patches/RV_V1_dtb.patch`

---

### Phase V2 — Traps, timer, PLIC ✅ COMPLETE

**Objective:** `stvec` catches everything with named diagnostics in the
R0 format; time advances; external interrupts route.

#### Tasks

- [x] `trapentry.S` + `trap.c`: full x1–x31 frame save to the per-hart
      kernel stack, `scause` decode — the 16 exception codes and the
      interrupt bit — printed R0-style (`cpu=hart0`, `sepc`, `stval`,
      register dump). Deliberate-fault self-test: an illegal
      instruction must be named and resumed past, exactly as i386's
      `int3` gate.
- [x] Timer: SBI `set_timer` + `sie.STIE`; a tick counter at 100 Hz
      (`[timer] PASS` when observed ticking, the I2 gate's shape).
      `sstc`/`stimecmp` probe recorded for later, not required.
- [x] PLIC: context enable/threshold/claim/complete for the boot
      hart's S-context; UART IRQ 10 wired as the first external line
      (consumed in V7; proven claimable here).
- [x] Jitter pool feeds from timer traps (the N0 fallback path):
      collection side delivered in V2 — rdtime deltas mixed and
      counted per tick. The consuming DRBG is shared kernel code that
      joins this build in V8; stubbing `rng.c`'s x86 probe moves
      there with it (the file cannot compile for rv64 before the
      shared-code phase anyway — it includes `arch/x86_64/cpu.h`).

#### Result

Delivered as specified, one scope move (rng, above) recorded rather
than hidden. Two facts earned during the phase:

- **The file-name collision:** `trap.S` and `trap.c` both produce
  `build/krv/.../trap.o` under the pattern rules — duplicate-symbol
  link errors. The assembly entry is `trapentry.S`; one object name,
  one owner.
- **The PLIC gate uses a real device interrupt, not a simulation:**
  the 16550's THRE line (IER bit 1) fires the moment it is enabled
  because the transmitter idles empty — a genuine
  PLIC-claim/handler/complete round-trip with no one typing. The
  handler acks via IIR and disables itself; V7's UART driver inherits
  a proven-live line. A level line left unacked would claim forever —
  which is also why `plic_dispatch` completes even handler-less
  claims.

Measured on `-machine virt` (full run under the smoke test):

```
[isr] Illegal Instruction at sepc=0x000000008020236a -- expected (self-test), resuming past
[isr]  PASS: illegal instruction named and resumed past
[timer] PASS: 5 ticks observed at 100 Hz
[rng]  jitter events collected: 5 (pool feeds from timer traps; DRBG consumes in V8)
[plic] S-context enabled, threshold 0, uart irq 10 wired
[plic] PASS: claim/complete round-trip, 1 completion(s), uart line fired 1 time(s)
```

The interrupt-enable order is airtight: stvec first, one timer shot
armed, then `sie` bits, then `sstatus.SIE` last — nothing can fire
half-configured. `fdt.c` grew `timebase-frequency` (10 MHz on virt —
the tick math's input) and the UART's `interrupts` property (line 10,
discovered not hardcoded). The smoke test: 21 → 29 assertions,
including "no unhandled trap in a full boot"; the checker: 13 → 19
claims. `wfi` idles the waits, so the full gate still ends by SBI
shutdown in ~1 s.

#### Test gate

- `[isr] PASS` (named + resumed illegal-instruction), `[timer] PASS`
  (ticks observed), PLIC claim/complete round-trip logged; no
  unhandled trap in a full boot.

#### Deliverable

`patches/RV_V2_traps.patch`

---

### Phase V3 — Memory: Sv39, PMM, heap — and W^X back ✅ COMPLETE

**Objective:** higher-half kernel behind Sv39, the standard PASS trio,
and the property i386 could not have: enforced W^X.

#### Tasks

- [x] `paging_rv.c`: three-level walk, map/unmap/probe; `satp` write +
      `sfence.vma`; boot sequence maps the kernel higher-half + the
      HHDM direct map with 2 MiB megapages, then drops the identity
      window (the I3 shape, one level deeper).
- [x] `kernelrv.ld` moves to the Sv39 higher half; the `.boot`
      physical-entry section pattern carries over from `kernel32.ld`.
- [x] PMM: `kernel/lib/bitmap.h`, third consumer, zero edits confirmed
      (the header's whole point); heap: the kheap32 design at LP64.
- [x] **W^X enforced and tested** for the kernel's own image: a store
      to `.text` and an execute-from-data must both fault — proven by
      resumable fault probes. (User PTEs from ELF `p_flags` move to
      V4 with U-mode itself — there is no user ELF to load yet; the
      enforcement machinery they will use is what this phase built
      and tested.)

#### Result

Delivered as specified. The boot shape: `boot.S` turns Sv39 on before
any C runs — the early root table is assembly-time DATA (gigapage
leaves are constants: identity @2, HHDM 256–259), so no code builds
tables before an allocator exists; then a literal-pool long jump to
`_start_high` up high. `paging_rv.c` later builds the FINAL tables
with an allocator and real permissions and drops the identity window.

Facts earned during the phase:

- **medany cannot address the low symbols from the higher half**:
  `auipc`'s ±2 GiB window does not span the HHDM gap, so
  `__kernel_start` etc. are unreachable as C symbols up high. They
  travel as data — `kernel_layout[8]`, a `.rodata` literal pool
  boot.S exports; fdt.c's self-report reads it too.
- **Mapping order is the algorithm**: `map_range` skips present
  entries and `walk` refuses to put a table under a leaf, so kernel
  sections map FIRST (4 KiB, real permissions), the split megapages'
  remainder second, the 4 GiB HHDM sweep (2 MiB RW megapages) last.
  Reversed, the sweep's leaves would block the section tables and the
  kernel would silently run unprotected.
- **Fault probes cannot resume by sepc += 4** — for execute-from-data
  the sepc IS the unexecutable address. `rv_setjmp` /
  `rv_longjmp_entry` (trapentry.S): the handler rewrites the frame so
  `sret` unwinds to the probe's setjmp with "the fault happened" as
  the return value. Access-fault and page-fault codes are both
  accepted per probe (PMPs and PTEs report the same sin differently).
- **DBCN wants physical addresses**: since V3 the console buffer's
  stack VA is higher-half, so `sbi_putc` subtracts the HHDM offset
  before handing the pointer to M-mode firmware.
- The heap self-test **failed its first run** (host harness with
  ASan reproduced it in seconds): first-fit had no "append a fresh
  block at the committed edge" path when the tail block was used —
  alloc-heavy phases OOM'd a 64 MiB window at 20 KB used. Fixed;
  the failing shape is now inside the passing gate.

Measured (full run, ~1 s to SBI shutdown):

```
[pmm]  frames tracked: 65536, free: 65357 (255 MiB)
[pmm]  PASS: 64 frames out and back, count restored
[vmm]  Sv39 final tables live: .text RX, .rodata R, data RW, HHDM RW; identity window dropped
[isr] Store/AMO Page Fault at sepc=0xffffffc0802061f2 -- expected (fault probe), unwinding
[vmm]  store to .text faulted (W^X write half)
[isr] Instruction Page Fault at sepc=0xffffffc080208040 -- expected (fault probe), unwinding
[vmm]  execute-from-data faulted (W^X execute half -- impossible to prove on i386)
[vmm]  identity window confirmed dropped (low load faults)
[heap] PASS: 64 cycles, no corruption, no leak
```

`sepc=0xffffffc08020xxxx` in every diagnostic is the link/boot
agreement proof (`eip=c01xxxxx`'s successor). The i386 status row's
❌ has its green sibling: same tree, same discipline, the arch with
the X bit enforces what the arch without one could only document.
Smoke: 29 → 36 assertions; claims: 19 → 25.

#### Test gate

- `[pmm] PASS`, `[vmm] PASS`, `[heap] PASS`; the vmm self-test
  additionally proves execute-from-data faults (new on this arch's
  gate, impossible on i386's). A fault frame's `sepc` in the higher
  half proves the link/boot agreement, as `eip=c01xxxxx` did.

#### Deliverable

`patches/RV_V3_mm.patch`

---

### Phase V4 — Threads, scheduler, U-mode, `ecall` ✅ COMPLETE

**Objective:** preemptive round-robin on the boot hart and a U-mode
program behind `sret`, with the trap stack lesson pre-paid.

#### Tasks

- [x] `context_rv.S`: callee-saved switch (`s0–s11`, `ra`, `sp` — the
      LP64 sibling of context32's set); TCBs and reaping in the
      thread32 shape.
- [x] Preemption from the timer trap, **after** trap-cause
      acknowledgement (the post-EOI placement, PLIC-flavoured:
      `sbi_set_timer` is what clears STIP, so the switch sits after
      the re-arm).
- [x] U-mode entry: `sstatus.SPP=0`, `sepc=entry`, `sret`;
      `sscratch` carries the per-image dedicated trap stack from day
      one — the I7 esp0 corruption is a design input here, not a bug
      to rediscover. trapentry.S does csrrw-swap-and-test: zero means
      S-trap (stay on the kernel sp), non-zero means U-trap (land on
      the trap stack, park the user sp in the x2 slot).
- [x] `ecall` dispatch per D4 into the existing syscall table;
      U-mode faults contained (`128+scause`), kernel survives — with
      the hand-assembled-image self-test and the privileged-op
      negative control both ported (the instruction is `csrr` from an
      S-CSR instead of `hlt`).

#### Result

Delivered as specified. The exit trampoline is V3's fault-probe pair
re-tenanted: `rv_setjmp`/`rv_longjmp_entry` IS user32.c's
saved_esp/saved_eip mechanism, so SYS_EXIT and the fault path share
one unwind. Four facts were bought with debugging sessions:

- **The sched gate deadlocked as first written.** Worker A exits the
  moment it has seen B's +3 and contributes nothing further; if B
  sampled its baseline near A's final value, B waits forever
  (measured: A DONE, B spinning). Each worker now banks a surplus
  after its own wait — the farewell tail in `sched_worker` is
  load-bearing and commented as such.
- **`sstatus.SUM` is SMAP's sibling and it is ON by default**: the
  kernel's own load from a `PTE_U` page traps. The write path does
  bounds-check → SUM on → copy to a kernel buffer → SUM off → print.
  First run measured the fault exactly where the plan's risk table
  said this ISA differs from x86.
- **`sbi_putc`'s DBCN buffer moved to .bss**: a stack local on the
  kheap trap stack (0xFFFFFFE0…) is NOT at HHDM+phys, so the V3
  "subtract the offset" trick fed OpenSBI a garbage physical address
  (M-mode fault_load, measured). Statics live where VA−HHDM is exact.
- **The unwind runs with SPIE=0.** `rv_longjmp_entry` executes two
  instructions with sp still holding the trapped USER stack pointer;
  a timer tick in that window built an S-mode trap frame on the user
  stack — store-fault recursion descending 288 bytes/iteration in
  `-d int`. `user_rv_leave` clears SPIE; `user_rv_run_image` re-opens
  interrupts once sp is a kernel stack again.

And one incident replayed on schedule: the hand-assembled image's
message landed at +0x44 with the pointer saying +0x48 — the console
printed `-U-OK` + 4 NULs, the i386 "ING3-OK" pad-byte incident
verbatim at the third width, caught by the same exact-string assert
that caught it there.

Measured (full run, ~3 s):

```
[sched] worker-a count 3581293, worker-b count 3671221 -- both advanced under forced preemption
[sched] PASS: two never-yielding workers both ran (preemption is real)
RING-U-OK
[user] exit(42) via ecall
[user] U-mode fault: scause=2 stval=0x0000000010002573 sepc=0x0000000040000000 -- terminating image (code 130)
[user] PASS: privileged op contained (code 130), kernel intact
[kernel] V4 complete; kernel reaches idle.
```

User text maps `PTE_U|R|X`, the user stack `PTE_U|R|W` — V3's W^X
machinery met its user PTEs one phase after it was built (`stval` in
the negative control is the `csrr` encoding itself: fetched, decoded,
refused). Smoke: 36 → 44 assertions; claims: 25 → 32.

#### Test gate

- `[sched] PASS` (two never-yielding workers), `RING-U-OK` written
  from U-mode via `ecall`, exit code round-trip, negative control
  contained, kernel reaches idle. The x86 pair untouched.

#### Deliverable

`patches/RV_V4_proc.patch`

---

### Phase V5 — Userspace: libc-rv, init, the shared shell ✅ COMPLETE

**Objective:** compiled-from-C U-mode programs from the shared initrd —
and the first *userspace source* shared across arches.

#### Tasks

- [x] `lib/libcrv/`: `crt0_rv.S` (inline `SYS_EXIT` ecall — the crt0
      independence rule from I5), `syscall_rv.S` (`a7`/`a0–a5`
      marshalling), `libcrv.h` mirroring `libc32.h`'s surface.
- [x] **Promote the shell**: `userspace/system/shell32/shell32.c` is
      pure portable C over the tiny libc surface — it moved to
      `userspace/system/smallsh/smallsh.c` and builds for BOTH i386
      and rv64 (each with its own crt0/linker script). One shell
      source, two bring-up arches; the i386 image keeps its behaviour
      (its smoke family is the regression gate).
- [x] ELF loader: `elfrvload` for `EM_RISCV`/`ET_EXEC`, refusing the
      other two arches' classes/machines — the three-way mutual
      refusal completing the pattern.
- [x] initrd: `/binrv/init` + `/binrv/smallsh`, stripped
      (`llvm-strip` — GNU strip does not speak EM_RISCV), in the one
      shared archive; the x86_64 boot still reaches its shell with
      the fatter tar (the I5 hard-won assert, re-armed and green).

#### Result

Delivered as specified. The promotion is the phase's centrepiece and
it came out cleaner than planned: smallsh.c differs from shell32.c by
a seam of FOUR defines (`AURA_LIBC`, `AURA_PUTS`, `AURA_UNAME`,
`AURA_RUN_EXAMPLE`) — the D4 one-table rule meant read/write/spawn/
getpid/exit are the same *numbers* at both widths, so the promotion
was a rename plus a header switch, not a port. The i386 shell smoke
(16 assertions incl. the x86_64 no-regression pair) runs green
against the shared source; that gate is this phase's negative
control, and it held.

Facts earned:

- **V4's SYS_RV_YIELD was 24; the table says 158.** A transcription
  slip that survived V4 because nothing dialled the number from a
  shared header — the moment libcrv.h existed, the mismatch was
  structural. Fixed in V5; the lesson is the D4 rule itself: numbers
  live in ONE place or they drift.
- **The loader's W^X is active, not passive**: p_flags map to real
  PTE bits (PF_X→R+X, PF_W→R+W), and a segment claiming W+X together
  is REFUSED — this loader will not build the PTE V3 promised never
  to build. The i386 loader states permissions; this one enforces
  them.
- `user_rv_run_elf` inherits user32_run_elf's nesting discipline
  verbatim: parent jmpbuf saved around the child, per-depth user
  stack pages stepping down from the window ceiling, per-depth trap
  stacks (the I7 lesson), mark/release unmapping. The `run
  binrv/init` from the shell exercises all of it at depth 1.
- The V4 flat-image self-tests still run before the V5 ELF path
  every boot — the write window widened to span both user worlds,
  with the page-probe loop vouching for presence.

Measured (the full interactive session over the SBI console):

```
initrv: kernel-pointer write refused (EFAULT) -- good
initrv: exiting 7
[elfrv] mapped 2 user page(s), entry 0x0000000030000000 (p_flags honoured: PF_X->RX, PF_W->RW)
auralite# uname
AuraLite OS riscv64 (Sv39 higher half, RISCV_PLAN V5)
auralite# run binrv/init
[user] running /binrv/init (entry 0x0000000008048000, 8712 bytes, depth 1)
exit code 7
auralite# exit
bye
```

New gate: `rv_shell_smoke.sh` (15 assertions — the i386 session
script with the arch swapped, which is the point: same source, same
transcript shape). Boot smoke: 44 → 45 assertions; claims: 32 → 38
(+ the i386 checker's I7 claim updated for the move — it FAILED the
moment the file moved, which is exactly the drift-detection working).

#### Test gate

- init runs U-mode from `/binrv`, EFAULT negative control from
  userspace, exits as built; the shell gate (`auralite# `, echo, run,
  nested spawn, exit) passes over the SBI/UART console — the same
  session script as `i386_shell_smoke.sh` with the arch swapped.
- The i386 shell smoke stays green against the renamed shared source
  (this is the phase's own negative control: a "promotion" that
  changes behaviour is a fork wearing a costume).

#### Deliverable

`patches/RV_V5_user.patch`

---

### Phase V6 — The inline-assembly sweep ✅ COMPLETE

**Objective:** ratchet 4 armed and burning: the 33 portable files with
x86 asm become arch-header consumers, batch by batch, with the
byte-identity control on every batch.

#### Tasks

- [x] `arch.h` grows `arch_irq_save/restore`, `arch_wait_for_interrupt`,
      `arch_cpu_relax` (D6) with x86_64, i386 and riscv64 backends;
      the `cli/sti/hlt/pause` sites migrate in counted batches.
- [x] `spinlock.c` → C11 atomics; the x86_64 kernel's `.text` compared
      before/after — it differed, and the diff was *read*, not waved
      through (findings below).
- [x] `check_width_sweep.py` ratchet 4: `__asm__`-bearing portable
      files, baseline 33 measured exactly as planned, self-test plants
      a violation and proves the count moves.
- [x] Port-I/O fenced for riscv: `arch.h`'s riscv branch declares
      `inb`..`outl` with `__attribute__((unavailable))` naming the V7
      virtio-mmio route — including arch.h stays legal (the irqflags
      block must work), the first port-I/O *use* is the hard error.
      Never a stub that silently does nothing (the xHCI lesson).

#### Result

Baseline confirmed at exactly the plan's number: **33** portable
files bearing real `__asm__` statements (the ratchet regex requires
the opening paren — a comment mentioning `__asm__` is not a hit).
First batch migrated 4: `spinlock.c` (C11 atomics), `kprintf.c`,
`time.c`, `scheduler.c` (irq-save pairs, sti;hlt idles, pause spins →
the arch_* four). Ratchet armed at **29**, selftest plants an
asm-bearing file and watches the count move.

**The byte-identity control fired, and reading the diff was the
phase's most instructive hour:**

- `kputs_locked`, `kprintf`, `sched_yield`, `kernel_block_current`:
  instruction-identical except relocated addresses and one shifted
  data symbol — the inline pushfq/cli/sti lowered to the same bytes
  through the header. The forwarding claim holds where it should.
- `spinlock_acquire`: same instruction *set*, different basic-block
  layout — the C11 spelling gives clang a visible CFG where the asm
  block was opaque, so it re-ordered the fast path (fall-through exit
  vs branch-to-exit) and dropped the frame push on it. Same lock
  algorithm: LOCK CMPXCHG, pause spin on a cached read, plain-store
  release. Accepted: a *reordered* spinlock is not a *changed*
  spinlock, and the atomics' documented lowering IS the old asm.
- `spinlock_acquire_irqsave`: the stack-protector canary frame
  vanished — the old version spilled RFLAGS to the stack through the
  `"=rm"` constraint, tripping -fstack-protector's array heuristic;
  the C11 version keeps it in a register. Strictly better code, same
  semantics.

Verified after the batch: x86_64 full boot green (22 PASS lines, the
shell reached), i386 boot32 smoke green, rv64 45-assert smoke green —
three kernels, one migration, zero regressions. The riscv kernel
still contains zero `__asm__` outside `kernel/arch/` (it always did;
ratchet 4 now guarantees it stays true as portable code starts
joining the rv64 build in V8).

Remaining 29 files are V8's companions: most are drivers whose whole
body is x86-only (port I/O ones now fenced), and the proc/*.c cluster
migrates when those files actually join the rv64 build — paying the
ratchet down in the commit that needs it, per the plan's batch rule.

#### Test gate

- Ratchet 4 registered and green; x86_64 `.text` byte-identical across
  the migration batches (or the diff explained in the commit — it is,
  above);
  i386 + x86_64 suites green; the riscv build contains zero `__asm__`
  outside `kernel/arch/`.

#### Deliverable

`patches/RV_V6_sweep.patch`

---

### Phase V7 — Drivers: virtio-mmio, blk, net, UART RX ✅ COMPLETE

**Objective:** the `virt` machine's real device set, through the
existing virtqueue logic — and the interactive gate on this arch.

#### Tasks

- [x] `kernel/arch/riscv64/virtio_mmio.c`: the transport —
      magic/version probe at the 8 DTB-provided windows, feature
      negotiation, queue setup via the mmio register layout (BOTH
      flavours: legacy version=1 contiguous-PFN vrings, which QEMU
      virt exposes by default, and modern version=2 split rings —
      probing beats folklore, the V0 DBCN lesson generalised). The
      virtqueue structures are **reused** from
      `drivers/virtio/virtio_common.h` — the same header the PCI
      drivers use; the PCI path keeps working on x86, gated by its
      own suite (D7). (Placed under kernel/arch/riscv64/, not
      drivers/: the x86_64 kernel's `find kernel drivers` would
      otherwise swallow it into that build — same reason the V5 phase
      excluded the riscv tree from KERNEL_SRCS.)
- [x] virtio-blk over mmio: the ata32-shaped self-test — with one
      honest adjustment: the rv64 boot has no MBR, so "known bytes"
      are a test pattern (`Aura` + 0x55AA) the smoke test writes to
      the disk image FRESH each run (a stale disk must never fake a
      pass); write/readback/restore on the last sector as specified.
- [x] virtio-net over mmio: the I8-shaped gate (DHCP lease on SLIRP,
      gateway ARP, payload-verified ICMP echo) — net32's protocol
      code lifted to `kernel/net/miniproto.c`, second consumer, same
      rule as the shell promotion. The lifted file PRINTS NOTHING
      (results via out-params, each caller keeps its own log
      strings) — that is what kept the i386 smoke asserts
      byte-identical across the refactor, verified:
      `[net] DHCP lease: 10.0.2.15 (gw 10.0.2.2)` still comes out of
      net32.c character for character.
- [x] UART: 16550 RX through PLIC IRQ 10 into a cons ring (kbd32's
      ring discipline, third console); `cons_rv_readline`'s blocking
      wait is `arch_wait_for_interrupt()` — wfi with SIE forced on;
      the I7 cleared-IF deadlock has an `sstatus.SIE` twin and the
      comment carries the lineage. The V6 access-method shim for a
      SHARED 16550 driver was inspected and declined: uart.c (x86) is
      40 lines of port I/O; a shim would be longer than both bodies —
      recorded as D-residue for when the x86 driver grows.

#### Result

Delivered with the deviations noted inline above (transport location,
blk known-bytes source, no UART shim — each with its reason). The
driver gates, measured on `-machine virt` with
`-global virtio-mmio.force-legacy=true -device virtio-blk-device
-device virtio-net-device -netdev user`:

```
[uart] 16550 RX armed: IRQ through the PLIC into the cons ring
[blk]  virtio-blk over mmio (legacy version 1): 8192 sectors (4 MiB), queue size 128
[blk]  PASS: known-bytes read + write/readback/restore on LBA 8191
[net]  virtio-net over mmio, MAC 52:54:00:12:34:56
[net]  DHCP lease: 10.0.2.15
[net]  PASS: lease + ARP + echo reply (payload verified)
[uart] rx bytes via PLIC irq: 63
```

The last line is the phase's receipt: every keystroke of the driven
shell session arrived through the PLIC interrupt path — a poll-fed
session would leave the counter at 0, and the smoke test greps for
`[1-9]`. Device absence stays a SKIP, not a FAIL: the boot smoke
(no `-device`) asserts the honest "no virtio-blk device" line and
runs green.

Cross-checks: `test_virtio_net` (x86 virtio-PCI, 7/7) green — the
transport split is invisible to the PCI path; `i386_parity_smoke`
(net32 → miniproto refactor) green with byte-identical log lines;
the full x86 integration filter run green. rv_shell_smoke: 15 → 23
assertions (blk/net/uart gates + the PLIC receipt); boot smoke: 45 →
46; claims: 43 → 49.

#### Test gate

- `[blk] PASS`, `[net] PASS` (lease/ARP/echo), and the driven shell
  session (`rv_shell_smoke.sh`) with keyboard-over-serial through the
  PLIC path; x86 suites green — especially the virtio-PCI cases,
  which must not notice the transport split.

#### Deliverable

`patches/RV_V7_drivers.patch`

---

### Phase V8 — Parity: storage, network, full crypto ✅ COMPLETE

**Objective:** the parity boot — every gate from every phase green in
one boot — plus the crypto milestone i386 could not reach.

#### Tasks

- [x] `rv_parity_smoke.sh`: the I8 shape — storage + network + the
      whole earlier gauntlet in a single boot, x86 pair attached.
- [x] **Full libatls at rv64**: `__int128` exists here (Fact 5), so
      the host gate runs the *complete* suite — X25519, Ed25519,
      P-256 included — cross-compiled riscv64-linux-gnu-gcc -static
      and EXECUTED under `qemu-riscv64` user-mode emulation, with the
      clang compile-only fallback when the cross toolchain is absent
      (the miss reported as a loud SKIP, never silently). The i386
      plan's §6 boundary entry has its counterpart: same sources,
      both truths recorded.
- [x] The initrd's third tenant audited: `mkinitrd.sh` reads
      `e_machine` out of every ELF in each `/bin*` tenant (62/3/243)
      and FAILS THE PACK on a cross-copied binary — naming the file,
      at build time, instead of a boot-loop refusal at runtime.
      Negative control exercised: an i386 binary planted in /binrv
      kills the pack with the file's name in the error.
- [x] Status-matrix rows drafted (below) for V9 to install in
      docs/status.md.

#### Result

Delivered as specified. The crypto gate measured:

```
[atls-rv64] OK test_atls_hash/aead/x25519/ed25519/ecdsa: vectors pass EXECUTED on rv64
[atls-rv64] PASS: the COMPLETE suite (hash/AEAD/X25519/Ed25519/ECDSA)
```

— all five RFC-vector suites, including the 51-bit-limb field
arithmetic and P-256 ECDSA that `-m32` structurally cannot compile,
executed on the target ISA (not just compiled: 64×64→128 carry
chains lower through mulhu on rv64, and a wrong-lowering bug would
produce plausible field elements — execution is the assertion).
Registered in `make test-unit` beside the m32 gate: the two truths
now sit in one target's output, three lines apart.

The parity boot: 21/21 assertions, one QEMU run — V0 banner through
V7 receipt (`rx bytes via PLIC irq`), `assert_no_grep FAIL` across
the entire log, and the x86_64 pair green with the three-tenant tar.

**Status-matrix draft for V9** (per-arch columns; the V9 phase moves
this into docs/status.md):

| Subsystem | x86_64 | i386 | riscv64 |
|---|---:|---:|---:|
| Boot path | ✅ BIOS+UEFI ISO | ✅ same ISO, refusal-gated | ✅ OpenSBI `-kernel` |
| Memory (PMM/VMM/heap) | ✅ | ✅ (no PAE ⇒ no NX) | ✅ Sv39 |
| W^X enforced | ✅ NX | ❌ honest (D3) | ✅ PTE X-bit, loader refuses W+X |
| Threads/sched | ✅ SMP | ✅ BSP-only | ✅ boot-hart only (D5) |
| User mode + syscalls | ✅ SYSCALL | ✅ int 0x80 | ✅ ecall (D4 numbers) |
| Userspace | ✅ full libc | 🚧 libc32 subset | 🚧 libcrv subset (V8 residue) |
| Shell | ✅ init shell | ✅ smallsh (shared) | ✅ smallsh (same source) |
| Storage | ✅ AHCI+virtio-PCI | ✅ ATA PIO | ✅ virtio-mmio blk |
| Network | ✅ e1000+virtio stack | ✅ e1000 miniproto | ✅ virtio-mmio + shared miniproto |
| Crypto vectors | ✅ host suite | 🧪 symmetric only (-m32 boundary) | ✅ COMPLETE suite executed |
| Console input | ✅ PS/2+serial ring | ✅ PS/2+serial ring | ✅ 16550/PLIC irq ring |

Residue, recorded not hidden: the full lib/libc port (errno/TLS/
stdio) and the VFS mount of the rv64 blk device are follow-on work —
the plan's §6 scoped them out of V8 ("the *bring-up* net/storage
proofs, not the full stacks"), and the matrix rows above say 🚧
where 🚧 is true.

#### Test gate

- The parity smoke green end to end; full-crypto gate green at rv64;
  both x86 suites green with the three-tenant initrd.

#### Deliverable

`patches/RV_V8_parity.patch`

---

### Phase V9 — CI matrix, docs, the claim check completed

**Objective:** three architectures on every push, documentation that
names all three, and the claim checker closing at full coverage.

#### Tasks

- [ ] `.github/workflows/integration.yml`: `riscv-parity` job —
      qemu-system-misc install, `make kernelrv`, artefact-presence
      assert (the KERNEL32-in-the-image lesson), the rv smoke family,
      logs on failure. Separate job, attributable red, as i386-parity
      argued.
- [ ] `docs/status.md`: the RISC-V section with the ❌-by-design rows
      stated (no rv32, no own M-mode firmware with D2's argument
      linked, no PCIe yet); `docs/architecture.md`: the third boot
      diagram; `docs/syscall_abi.md`: the `ecall` table beside the
      other two.
- [ ] `check_riscv_claims.py` grown to full phase coverage + the
      structural header/table checks; `README.md` boot-paths row.
- [ ] Rust stretch recorded honestly: target exists, `rustes`/`rsbr`
      are *possible* — a follow-up plan's opening fact, not this
      plan's promise.

#### Test gate

- Claim check at full coverage passes and self-fails against a
  doctored tree; all three arch jobs green from a clean clone.

#### Deliverable

`patches/RV_V9_ci.patch`

---

## 4. Order and rationale

```
V0 ── stub + claim check      (proves the path; checked from birth)
V1 ── DTB → boot_info         (the contract's third producer)
V2 ── traps/timer/PLIC        (V3 needs faults debuggable)
V3 ── Sv39 + W^X              (V4 needs address spaces)
V4 ── sched + U-mode + ecall  (V5 needs a way to run programs)
V5 ── libc-rv + shared shell  (the auralite# gate, third arch)
V6 ── inline-asm sweep        (needs V2–V5 to know which sites matter)
V7 ── virtio-mmio/blk/net     (needs V3 DMA memory + V2 PLIC)
V8 ── parity + full crypto    (needs V5 + V7)
V9 ── CI + docs + claims      (last, asserts what now exists)
```

The same compressed-bring-up shape as the i386 plan, minus one phase:
there is no I0-refusal analogue because there is no existing artefact
that *hangs* on RISC-V — nothing boots at all, so the first honest
deliverable is the stub, not a diagnostic. The claim checker moving to
V0 (D8) is the discipline the last plan proved out, promoted from
finish-line to starting-gun.

## 5. Risks

- **The V6 sweep touches running x86 code.** Migrating `spinlock.c` and
  the irq-gating sites changes files two shipping kernels execute. The
  byte-identity control and batch-sized commits are the mitigation; the
  first batch that fails byte-identity stops the line until the diff is
  understood.
- **FDT parsing is a new input surface.** Big-endian fields, untrusted
  lengths. The parser is deliberately four-properties-small,
  bounds-checked, and gets a host fuzz-shaped unit test with truncated/
  corrupt DTBs — the initrd.c allocation-failure lesson, applied at
  birth.
- **F/D register state in context switches.** rv64gc has 32 FP
  registers; the M1 FPU lesson (gltest corruption under SMP) says eager
  save/restore from day one on any thread that has touched FP, with
  `sstatus.FS` tracking. Costed into V4, not discovered in V8.
- **OpenSBI version drift.** The SBI spec is frozen but implementations
  vary (legacy putchar vs DBCN). V0 probes rather than assumes, and the
  smoke tests pin QEMU's bundled OpenSBI as the reference — same
  QEMU-first posture as everything else.
- **The virtio transport split regressing x86.** The PCI path has its
  own passing suite today; the factoring in V7 is gated on that suite
  staying green, which makes the split observable the moment it breaks
  something.
- **Real-hardware variance** (SoC UARTs that are almost-16550s, vendor
  SBI quirks): out of the gate's scope, experimental label, same as
  both x86 arches.

## 6. What this plan does not do

- **No rv32.** D1; a mismatched `misa` gets a polite refusal.
- **No own M-mode firmware / SBI implementation.** D2, argued there.
- **No PCIe on `virt`** until an mmio-less device forces it (D7).
- **No SMP** beyond parking secondary harts safely (D5); SBI HSM is
  the named exit ramp.
- **No virtio-gpu / graphical console.** The 16550 is the console;
  the compositor waits for a framebuffer story on this arch.
- **No vector extension (V), no hypervisor extension (H).** rv64gc
  only.
- **No Rust userspace in-plan.** The target exists (the honest
  difference from i386); porting `rustes`/`rsbr` is a recorded
  follow-up, not a phase.
- **No ACPI-on-RISC-V.** DTB is the platform description; the ACPI
  code stays x86.

## 7. A note on why this is worth doing at all

The i386 port proved the kernel's *width* portability and was honest
that it proved no more than that: same ISA, same devices, same firmware
culture. RISC-V is the orthogonal audit — same widths, *nothing else*
shared — and the tree is unusually well positioned for it: the handoff
struct is loader-agnostic by construction, the syscall table is
trap-agnostic by decision, the self-test discipline is ISA-agnostic by
habit, and the sweep machinery knows how to turn "portable in theory"
into a counter that only goes down. What RISC-V adds that neither x86
arch could: W^X back without apology, `__int128` crypto at full
strength, a Rust bare-metal target that actually exists, and a firmware
story — SBI — that is a frozen public specification rather than forty
years of compatibility sediment. If the portability claims survive
*this* audit, they are properties. If they do not, the plan finds out
one measured fact at a time, which is the only way this repository has
ever found anything out.
