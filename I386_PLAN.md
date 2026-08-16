# AuraLite OS — i386 (32-bit x86) Support Plan

## Status: IN PROGRESS 🚧 — I0–I5 complete, I6–I9 pending

| Phase | Result | Deliverable |
|-------|--------|-------------|
| I0 — an honest refusal on a 32-bit CPU | ✅ complete | `patches/I386_I0_lmcheck.patch` |
| I1 — the dual-kernel boot chain | ✅ complete | `patches/I386_I1_boot32.patch` |
| I2 — `kernel/arch/i386` CPU bring-up | ✅ complete (2 tasks re-scoped to I6, see phase result) | `patches/I386_I2_cpu.patch` |
| I3 — memory: non-PAE paging, PMM, heap | ✅ complete (1 deviation, see phase result) | `patches/I386_I3_mm.patch` |
| I4 — threads, scheduler, Ring 3, `int 0x80` | ✅ complete (scope note in phase result) | `patches/I386_I4_proc.patch` |
| I5 — 32-bit libc and userspace | ✅ complete (bring-up scope, see phase result) | `patches/I386_I5_user.patch` |
| I6 — the pointer-width sweep | pending | `patches/I386_I6_sweep.patch` |
| I7 — drivers on i386 | pending | `patches/I386_I7_drivers.patch` |
| I8 — filesystems, net, GUI parity | pending | `patches/I386_I8_parity.patch` |
| I9 — CI matrix, docs, the honest table | pending | `patches/I386_I9_ci.patch` |

This document answers:

> *What happens when someone boots `release/auralite.iso` on a machine whose
> CPU has no long mode — and what is the honest, incremental path from
> "a silent hang with no diagnostic" to "a working 32-bit AuraLite kernel"?*

It follows the structure of the existing plans (`FIXES_PLAN.md`,
`WIN32_PLAN.md`, `USB_PLAN.md`, `REALINTERNET_PLAN.md`): dependency-ordered
phases, a definition of done and a test gate for every phase, one `.patch`
per phase.

**Baseline:** commit `1b73450` (Documentation update).

Like `WIN32_PLAN.md`, this plan starts with a confession rather than a
feature list: the very first deliverable fixes a *defect* (a silent hang),
not a missing feature. A 32-bit kernel that boots is phases I1–I8; a 32-bit
*machine owner who knows why the screen is black* is phase I0, and it ships
first.

---

## 1. Where things actually stand

Measured against the tree and against QEMU, not assumed. Four facts shape
this plan.

### Fact 1 — On a 32-bit CPU, the image hangs silently. Today. Measured.

```
$ qemu-system-i386 -drive file=build/auralite.iso,format=raw,if=ide -serial stdio
...
[BL4] page tables built at 0x01000000
[BL4] entering long mode; jumping to kernel _start
                                  <- nothing, forever
```

`boot/bios/stage2/longmode.inc` executes `wrmsr` against `IA32_EFER` and
far-jumps into an L-bit code segment without ever asking the CPU whether it
implements either:

```
$ grep -rn cpuid boot/bios/ --include='*.inc' --include='*.asm' | wc -l
0
```

There is **no CPUID instruction anywhere in the boot chain.** On a CPU
without long mode the sequence is undefined at best (`#UD` on the EFER
write) and a triple fault at worst; under `qemu-system-i386` (default
`qemu32` vCPU, and equally under `-cpu 486` and `-cpu pentium3`) the
observed behaviour is a hang after the hand-off banner. Every 32-bit
machine that boots this image today presents as a black screen with a
promising serial log — the worst failure shape available, because the log
*claims* the hand-off happened.

### Fact 2 — The tree is 64-bit by construction, and the cost is countable

This is not a `#define`-flip port. The 64-bit assumptions are structural:

- **15 NASM files** in the boot chain, kernel and libc are `[bits 64]`
  (`boot.asm`, `isr_stubs.asm`, `syscall_entry.asm`, `context.asm`,
  `crt0.asm`, `syscall.asm`, …). Every one needs an i386 sibling.
- The kernel is linked with `-mcmodel=kernel` at `0xFFFFFFFF80100000`
  (top-2-GiB, `kernel.ld`) — meaningless on a 32-bit CPU.
- The HHDM constant `0xffff800000000000` appears at **12 sites** across
  `kernel/` and `boot/`, and `paging.c` walks a 4-level PML4 that i386
  does not have.
- `SYSCALL`/`SYSRET`, `swapgs`, `wrfsbase`-via-MSR, per-CPU `GS`-base —
  the entire syscall and cpu-local layer is long-mode-only.
- `grep -c 'uintptr_t\|(uint64_t)' kernel/` counts **877 sites** where
  pointers and 64-bit integers are mixed. Most are correct on both widths;
  each one has to be *read* to know. That sweep is I6, and it is the real
  cost centre of this plan (see Risks).

### Fact 3 — The toolchain already cross-compiles i386; zero new dependencies

Verified on this tree's build environment:

```
$ clang --target=i686-elf -ffreestanding -c t.c   # ELF 32-bit LSB, Intel i386 ✓
$ nasm -f elf32                                    # ✓
$ ld.lld -m elf_i386                               # ✓
$ which qemu-system-i386                           # ✓ (same qemu package)
```

Clang is a native cross-compiler and LLD links both widths; `REQUIRED_TOOLS`
does not grow. The one genuine toolchain gap is Rust: there is no
`i686-unknown-none` rustup target, so `rustes` and the `rsbr` bridge stay
x86_64-only until someone writes a custom target JSON (out of scope,
§6).

### Fact 4 — Some of the tree is already width-clean, by luck or by design

- `boot_info_t` (`boot/shared/boot_info.h`) is all fixed-width fields —
  the same struct works verbatim as a 32-bit hand-off contract, and
  `tools/gen_boot_offsets.c` regenerates offsets for whatever width
  compiles it.
- BIOS Stage 2 itself is 16/32-bit real-mode code end to end; FAT32,
  E820, A20, ACPI, the ELF *staging* all run fine on a 386-class CPU
  (measured: the i386 hang happens *after* all of them succeed).
- User programs are linked at low addresses (`0x40000000`-region: the
  build log shows user `.bss` at `0x40015e10`), which already fits under
  a 3 GiB/1 GiB split.
- The PMM bitmap, the slab layer, the VFS, FAT32/ext2, TCP/TLS and the
  compositor are freestanding C over `stdint.h` types — portable unless
  proven otherwise by I6's sweep.

---

## 2. Decisions

### D1. "i386 support" means **i686-class (P6) minimum** for the kernel — and a diagnostic for everything older

The 32-bit kernel assumes CPUID, CMOV, CMPXCHG8B: Pentium Pro (1995) and
later. Genuinely supporting an 80386 means fighting missing atomics for a
machine with 4 MiB of RAM; that is a different hobby. **But the refusal
path (I0) must work all the way down to a 386**, because the machine that
needs the message most is the one that can run the least — so the
long-mode check starts with the EFLAGS.ID toggle test (no CPUID on
i486-and-earlier is itself a "no").

### D2. Two kernels, one image, one boot chain — Stage 2 picks by CPUID

`make iso` keeps producing **one** hybrid image. The FAT32 partition
carries `KERNEL.ELF` (x86_64) *and* `KERNEL32.ELF` (i386); BIOS Stage 2
runs the long-mode check and loads whichever the CPU can execute. No
second ISO, no boot menu, no user decision — the CPU already made it.

UEFI: 32-bit UEFI firmware (`BOOTIA32.EFI`) is rare in practice and adds a
second PE toolchain target for a market of nearly zero machines. **Refused,
documented** (§6): a 32-bit machine boots via BIOS/CSM or not at all.

### D3. Non-PAE 2-level paging, 3 GiB/1 GiB split — and the honest NX consequence

The i386 kernel lives at `0xC0100000` virtual, direct-maps the first
896 MiB at `0xC0000000` (`hhdm_offset = 0xC0000000` in `boot_info_t` —
the field is already 64-bit-wide and the kernel already reads it rather
than assuming), and uses plain 2-level 4-KiB paging. No PAE, no highmem:
supported RAM is capped at 512 MiB, which is what every QEMU
configuration in this repository already uses.

The consequence to state plainly rather than bury: **without PAE there is
no NX bit, so the i386 kernel loses W^X for user pages.** `elfperm`'s
execute-from-data assertion becomes arch-conditional, and `docs/status.md`
gets an honest per-arch row instead of a silent green tick.

### D4. Syscall ABI: `int 0x80`, AuraLite's own numbers — **not** Linux-i386 numbers

The syscall table (`SYS_READ 0`, `SYS_WRITE 1`, … 103 numbers in
`lib/libc/include/unistd.h`) is shared verbatim between both
architectures; only the *trap mechanism* differs. i386 dispatches via
`int 0x80` (no `SYSCALL` in 32-bit mode; `SYSENTER` is an optimisation for
later, not a baseline). Register mapping: `eax` = number,
`ebx, ecx, edx, esi, edi, ebp` = args — Linux's convention, because it is
well-documented and every register allocator understands it. Adopting
Linux's *numbers* as well would fork the libc table in two and buy
nothing: AuraLite's numbers already mostly match Linux-x86_64, not
Linux-i386, and one table that is true everywhere beats two that drift.

### D5. BSP-only on i386, at first

The H8 SMP scheduler (per-CPU run queues, work stealing, IPI shootdown)
stays x86_64-only. The i386 kernel boots BSP-only with the PIT at 100 Hz,
exactly like this kernel did for its first ten phases. A 32-bit machine
with multiple CPUs will schedule on one until someone cares; that is a
performance gap, not a correctness gap, and it is recorded in the status
matrix rather than hidden.

### D6. One tree, an arch split, and typed widths instead of an `#ifdef` forest

`kernel/arch/x86_64/` gets a sibling `kernel/arch/i386/` with the same
file names and the same contracts (`gdt_init()`, `idt_set_gate()`,
`paging_map()`, …). Portable code includes `kernel/arch/arch.h` which
forwards to the configured arch. Two typing rules make the I6 sweep
mechanical rather than judgemental:

- **Physical addresses stay `uint64_t` on both arches** (`paddr_t`).
  E1000/AHCI descriptor formats are 64-bit on the wire regardless of the
  CPU, and DMA above 4 GiB simply never happens on a 512 MiB machine.
- **Virtual addresses are `uintptr_t` and nothing else.** Every
  `(uint64_t)ptr` cast in kernel code is either deleted (was a virtual
  address) or retyped `paddr_t` (was physical). The sweep is done when
  `-Wshorten-64-to-32`-class warnings are clean on the i386 build with
  `-Werror`.

### D7. The refusal ships first, alone, before any porting

I0 is one included file and a smoke test, deliverable in a day, and it
converts every 32-bit boot from "silent hang after a log line that claims
success" into a two-line diagnostic on both the serial port and the VGA
text screen. Even if phases I1–I9 are never built, I0 is worth shipping —
the same reasoning `FIXES_PLAN.md` R0 used: make the failure visible
before making it good.

---

## 3. Phases

### Phase I0 — An honest refusal on a 32-bit CPU ✅ COMPLETE

**Objective:** no CPU ever executes the long-mode sequence without being
asked first; a CPU that answers "no" gets a diagnostic, not a hang.

#### Tasks

- [x] `boot/bios/stage2/lmcheck.inc` (new): `check_long_mode` —
      EFLAGS.ID toggle test (386/486 have no CPUID at all), then CPUID
      `0x80000000` extended-leaf presence, then `0x80000001 EDX.LM`
      (bit 29). CF=1 on any "no". Works in real mode on a 386.
- [x] Stage 2 calls it after the unreal-mode self-test and **before** any
      long-mode commitment; on failure it prints `[BL10]`-tagged
      diagnostics to **both** COM1 and the VGA text console (INT 10h
      teletype — the machine this fires on may have no serial cable
      attached) and halts.
- [x] A `vga_puts` teletype helper in Stage 2 — the first user-facing
      output path in the boot chain that does not assume a serial port.
- [x] `tests/integration/i386_refusal_smoke.sh` (new): boots the real ISO
      under `qemu-system-i386` and asserts the refusal text appears and
      the `entering long mode` line does **not**; then boots the same
      bytes under `qemu-system-x86_64` and asserts the normal 64-bit boot
      is untouched.

#### Result

Delivered as specified. Measured under `qemu-system-i386` (qemu32, `-cpu
486`, `-cpu pentium3` — all three refusal shapes):

```
[BL3] unreal mode OK
[BL10] this CPU has no long mode (x86_64); AuraLite needs a 64-bit CPU
[BL10] halting -- see I386_PLAN.md for the 32-bit kernel roadmap
```

and under `qemu-system-x86_64` the same bytes print `[BL10] CPU supports
long mode` and boot to the kernel banner unchanged. The smoke test's
8 assertions pass; Stage 2 grew 512 bytes (4608 → 5120 of 64512).

#### Test gate

- `qemu-system-i386` + `build/auralite.iso`: serial log contains
  `[BL10] this CPU has no long mode` and does not contain
  `entering long mode`.
- `qemu-system-x86_64` + the same image: `Hello from AuraLite OS kernel!`
  still appears; no refusal text.
- Negative control: reverting the Stage 2 hunk turns the i386 case back
  into the silent hang, which is how the test is known to test anything.

#### Deliverable

`patches/I386_I0_lmcheck.patch`

---

### Phase I1 — The dual-kernel boot chain ✅ COMPLETE

**Objective:** the same image carries `KERNEL32.ELF`, and a 32-bit CPU
boots into 32-bit code that proves the hand-off — before a single line of
the real kernel is ported.

The stub-first shape is deliberate and copied from BL3 ("stage2 alive"
banner before any loading existed): prove the *chain*, then grow the
*payload*. The stub is ~60 lines and prints on COM1; everything it
validates — ELF32 staging, protected-mode entry, `boot_info_t` in ESI,
the magic check — is exactly what the real i386 kernel needs in I2.

#### Tasks

- [x] `boot/bios/stage2/elf32.inc` (new): ELF32 sibling of `elf.inc` —
      class check, `e_entry`/`e_phoff`/phdr walk at the 32-bit offsets,
      PT_LOAD copy to `p_paddr` through unreal `FS`.
- [x] `boot/bios/stage2/pmode32.inc` (new): flat 32-bit GDT
      (code `0x00CF9A…`, data `0x00CF92…`), `CR0.PE`, far jump, segment
      reload, `ESP` at `0x9F000`, **`ESI` = `boot_info_t` phys** (the
      32-bit hand-off contract; RDI is the 64-bit one), jump to
      `[elf32_entry_addr]`. Never returns.
- [x] Stage 2: I0's refusal becomes a *fallback* — the verdict is
      recorded in `lm_absent` at check time; after `fat_init` the chain
      branches: long mode → `KERNEL.ELF` (unchanged 64-bit path), no
      long mode → `KERNEL32.ELF` → `elf32_load` → `enter_prot32`. The
      refusal diagnostic remains for the one case that still deserves
      it: no long mode **and** no `KERNEL32.ELF` on the partition.
- [x] `kernel/arch/i386/stub/` (new): `boot32.asm` (stack, push ESI, call
      C), `main32.c` (COM1 init by port I/O, banner, `boot_info_t` magic
      verdict, `hlt` loop), `kernel32.ld` (ELF32, linked at physical
      `0x00100000`, VMA = LMA — no paging yet).
- [x] `Makefile`: `kernel32` target (`--target=i686-elf`, `nasm -f
      elf32`, `ld.lld -m elf_i386`); `iso-dual` builds it;
      `tools/mkisoimage_dual.sh` adds `/KERNEL32.ELF` to the FAT root.
- [x] `tests/integration/i386_boot32_smoke.sh` (new): the full matrix —
      i386 boots the stub with `magic OK`; x86_64 boots the real kernel
      from the same bytes; and an image with `KERNEL32.ELF` deleted
      (`mdel` on a copy) still produces I0's refusal rather than a hang.

#### Result

Delivered as specified. Measured under `qemu-system-i386` on the real
`make iso` image:

```
[BL10] taking the 32-bit path: KERNEL32.ELF
[BL4] FAT32 BPB parsed
[BL4] kernel.elf located
[BL10] ELF32 PT_LOAD segments copied to phys
[BL10] entering protected mode; jumping to kernel32 _start
[kernel32] AuraLite i386 stub alive
[kernel32] boot_info handoff (ESI) magic OK
[kernel32] mmap entries: 0x00000006
[kernel32] initrd size: 0x00836800 bytes
```

The smoke test's 13 assertions pass, including the `mdel` negative
control (refusal, no stub, no hang) and the x86_64 no-regression case;
`bl4_boot_smoke.sh` and `bl7_dual_smoke.sh` stay green.

One real bug was found by the stub's own canary and is worth recording,
because it is the I6 sweep's thesis in miniature: the first build printed
`mmap entries: 0`. The i386 System V psABI aligns `uint64_t` to 4 bytes,
the AMD64 one to 8, so the same `boot_info.h` compiled to different
`mmap[]` offsets at the two widths and the stub read the map 8 bytes
early. Fixed with `-malign-double` in `CFLAGS32` (both ABIs then agree on
every offset), asserted forever after by the smoke test's "mmap count
non-zero" line. A hand-off struct shared across pointer widths is a
contract only when a test enforces it.

#### Test gate

- `qemu-system-i386`: `[BL10] entering protected mode` →
  `[kernel32] AuraLite i386 stub alive` → `boot_info handoff (ESI) magic OK`.
- `qemu-system-x86_64`, same image: the 64-bit kernel boots exactly as
  before (`bl4_boot_smoke.sh` and `test_boot_to_shell` stay green).
- The `mdel` negative control: refusal text, no hang, no stub banner.

#### Deliverable

`patches/I386_I1_boot32.patch`

---

### Phase I2 — `kernel/arch/i386` CPU bring-up ✅ COMPLETE

**Objective:** the stub grows into a real `kmain32` running the portable
kernel core: console, GDT/IDT/PIC/PIT, exceptions with named diagnostics.

#### Tasks

- [x] `kernel/arch/i386/{boot32.asm,gdt.c,idt.c,isr32.c,isr_stubs32.asm,irq32.c}`
      — same contracts as the x86_64 siblings; 32-bit IDT gates, `iret`
      frames, error-code push parity handled per vector exactly as
      `isr_stubs.asm` does today (vectors 8, 10–14, 17, 21 get the CPU's
      error code, everyone else a pushed zero).
- [x] Exceptions print the same `[diag]` register-dump format R0
      established — 32-bit register names, CPU number 0, CR2 on #PF.
- [ ] ~~`kernel/arch/arch.h`~~ — **re-scoped to I6** (see result note).
- [ ] ~~`kmain32` compiles `kernel/kernel.c`'s init path~~ — **re-scoped
      to I6** (see result note).

#### Test gate

- i386 boot reaches `IDT installed` + `PIC remapped` + a PIT tick counter
  on serial; a deliberate `int3` in a boot self-test prints a named
  exception frame instead of rebooting.
- `ARCH=x86_64` (default) kernel binary is unchanged — trivially true
  this phase, since `arch.h` moved to I6 and no shared file was touched.

#### Result

Delivered with one honest re-scope. The stub is deleted;
`kernel/arch/i386/` now holds the real bring-up kernel: flat GDT with
Ring 3 descriptors and a **32-bit TSS** (8-byte descriptor, `SS0`/`ESP0`
wired — on i386 the TSS is the ring-transition mechanism, so it exists
from day one even though Ring 3 arrives in I4), 256 NASM-generated gate
stubs with per-vector error-code parity, the 8259A remap, the PIT at
100 Hz, and two boot self-tests in the kmain contract:

```
[boot] GDT loaded (kernel + user segments + 32-bit TSS)
[boot] IDT installed: 256 gates
[boot] PIC remapped (IRQs -> vectors 32-47), all masked
[kernel] interrupts enabled, exception handling online.
[diag] deliberate #BP self-test:
  cpu=0  vector=3 (Breakpoint)  err=00000000
  eip=00100b0b cs=00000008 eflags=00000206
[isr] PASS: deliberate #BP named, dumped, resumed
[timer] PASS: PIT ticking (4 ticks observed)
```

`i386_cpu_smoke.sh`: 16 assertions green, including "the I1 stub banner
is gone" and the standing x86_64 no-regression case.

**The re-scope, stated rather than hidden:** the original task list had
this phase both introduce `arch.h` *and* compile `kernel/kernel.c`'s
init path. Attempting that ordering was wrong in a way that only became
visible with the code open: `kernel.c`'s first hundred lines pull
`boot_info.c` → HHDM arithmetic → `paging.h` — I3's and I6's work,
respectively. Compiling shared portable files before the width sweep
exists means either doing the sweep piecemeal *inside* I2 (scope creep
with no gate of its own) or peppering `#ifdef`s (which D6 forbids). So
I2 ships arch-local siblings with identical *contracts* (`gdt_init()`,
`idt_set_gate()`, the `[diag]` format), and the *adoption* of shared
code through `arch.h` lands in I6 where its negative control
(byte-identical x86_64 kernel) already lives. `kprintf32` carries the
same reasoning in its header: it dies when the shared `kprintf`
becomes width-clean. The alternative — claiming the checkbox by
compiling a gutted `kernel.c` — is exactly the drift `AUDIT_A7` exists
to catch.

#### Deliverable

`patches/I386_I2_cpu.patch`

---

### Phase I3 — Memory: non-PAE paging, PMM, heap ✅ COMPLETE

**Objective:** `pmm`/`vmm`/`kheap` self-tests pass on i386 with 2-level
paging and the `0xC0000000` direct map.

#### Tasks

- [x] Paging enabled before `kmain32` — **in the kernel's own `.boot`
      section, not in Stage 2** (deviation from the original task text,
      argued below): `boot32.asm` builds a PSE page directory (identity
      [0, 896 MiB) + the same frames at `0xC0000000`), sets `CR4.PSE`,
      `CR0.PG|WP`, and jumps higher-half. Stage 2's 32-bit path stays
      paging-free and instead gains the `check_i686` floor test (PSE,
      CX8, CMOV — a 486/586 now gets the honest refusal, not a #UD on
      `mov cr4`) and writes `hhdm_offset = 0xC0000000` into
      `boot_info_t`, which `kmain32` validates rather than assumes.
- [x] `kernel/arch/i386/paging32.c`: map/unmap/probe over PDE/PTE;
      `PAGE32_FLAG_NO_EXEC` accepted and **recorded as unenforceable**
      (D3) — the boot log itself prints the consequence, and the smoke
      test asserts the line exists.
- [x] PMM: `pmm32.c` reuses `kernel/lib/bitmap.h` — the identical,
      host-tested header the x86_64 PMM compiles. E820 walks in
      `uint64_t`; regions above the horizon are **skipped, not
      truncated** (D6 discipline, first instalment).
- [x] kheap: first-fit with split/coalesce over an on-demand committed
      64 MiB window, same design and PASS contract as `kheap.c`.

#### Test gate

- i386 serial log: `[pmm] PASS`, `[vmm] PASS`, `[heap] PASS` — the same
  self-tests, same output contract as the x86_64 boot. ✔
- `i386_mm_smoke.sh` additionally proves execution is *actually*
  higher-half (the #BP frame's `eip=c01xxxxx` — a banner can lie, a
  fault frame cannot) and that the identity window is gone. ✔
- The host PDE/PTE-encoding unit test is **deferred to I6** with the
  other host-build work; the in-VM self-test covers the encoding
  end-to-end meanwhile (map → write → alias-read через direct map →
  probe → unmap → probe-dark).

#### Result

Delivered; 16 smoke assertions green, including the x86_64 pair (the
64-bit HHDM line specifically, since Stage 2 now writes the field on one
path).

**Deviation, argued:** the original task had Stage 2 build the page
tables, mirroring `paging.inc`. Implementation showed the mirror is
false: the 64-bit kernel *cannot* run a single instruction unpaged (long
mode requires paging), so its loader must build tables; a 32-bit kernel
runs fine unpaged, and the natural owner of a page directory that lives
in the kernel's own `.boot` section is the kernel. Moving the work into
`boot32.asm` kept Stage 2 526 lines instead of ~700, kept the loader
contract identical for both kernels ("flat memory, boot_info in a
register"), and made the identity-window drop a kernel-internal detail.
The plan text was wrong about *where*; the gate (what must be true at
`kmain32`) was right and is what the test asserts.

**Two bugs the gates caught, worth the record:**
1. The planned heap base `0xF0000000` sits *inside* the direct map
   (`0xC0000000 + 896 MiB = 0xF8000000`); `paging32_map` correctly
   refused to split a PSE page and `[vmm] FAIL` stopped the boot at
   first try. Heap moved to `0xF8000000`, probe to `0xFC000000`. The
   plan's own §2 layout table carried the bug — measured is better than
   planned.
2. A header-edit rebuild gap: `kheap32.o` stayed compiled against the
   old `KHEAP32_BASE` and failed to commit its first page. The `k32`
   pattern rule now depends on all i386 headers (comment in the
   Makefile records the incident).

#### Deliverable

`patches/I386_I3_mm.patch`

---

### Phase I4 — Threads, scheduler, Ring 3, `int 0x80` ✅ COMPLETE

**Objective:** preemptive round-robin and a Ring 3 process on i386.

#### Tasks

- [x] `context32.asm` (callee-saved-only switch, the cdecl sibling of
      `context.asm`'s SysV-minimal set), `user_entry32.asm` (`iretd` to
      Ring 3 with the 5-dword inter-privilege frame), TSS `esp0`
      refreshed on **every** context switch — the i386 TSS is the
      ring-transition mechanism, and refreshing unconditionally means
      Ring 3 cannot forget.
- [x] `int 0x80` gate, DPL=3 on exactly that vector; register
      marshalling per D4 (EAX number, EBX/ECX/EDX/ESI/EDI args,
      AuraLite's own numbers — `SYS_WRITE=1`, `SYS_GETPID=39`,
      `SYS_EXIT=60`, `SYS_SCHED_YIELD=158`). User pointers are
      **range-checked against the user window** at this phase; the
      `#PF`-fixup `copy_from_user` port moves to I5 with the libc that
      needs it (scope note below).
- [x] Preemption wired PIT → `sched32_tick` → post-EOI
      `sched32_maybe_preempt` (the placement is the phase-6 lesson:
      switching before EOI freezes IRQ0 for every other thread).
      Reaping from the idle thread. BSP-only (D5).
- [x] Ring 3 fault containment: an exception with CPL=3 in the saved
      CS terminates the user image with `128+vector` and the kernel
      survives — the x86_64 dispatcher's SIGSEGV path, minus signals,
      which arrive with the libc in I5.
- [ ] ELF32 user loader — **moved to I5**, with the scope note below.

#### Test gate

- i386: `[sched] PASS` interleave self-test ✔; a Ring 3 program runs,
  makes `write()`/`getpid()`/`exit()` via `int 0x80`, its output and
  exit code observed in the kernel log ✔; **negative control**: a
  privileged `hlt` from Ring 3 is contained via #GP (code 141), never
  executed, and the boot continues ✔.
- x86_64 suite untouched and green ✔.

#### Result

Delivered; `i386_proc_smoke.sh` 17 assertions green.

```
[sched] round-robin online (BSP-only, 8 slots, 16 KiB kernel stacks)
[sched] worker counts: 3064567 / 570368
[sched] PASS: 2 workers preempted, both progressed
[boot] int 0x80 gate armed (DPL=3), AuraLite syscall numbers (plan D4)
RING3-OK
[user] exit(42) via int 0x80
[user] Ring 3 fault: vector=13 err=00000000 eip=40000000 -- terminating image (code 141)
[user] PASS: Ring 3 write/getpid/exit + #GP containment
```

**Scope note, argued:** the gate's original wording wanted a
"`/bin/hello`-class ELF32 binary". What shipped is a hand-assembled
Ring 3 image (the bytes are in `user32.c`, short enough to read),
because an ELF32 *file* needs a VFS/initrd read path the i386 kernel
does not have until I5's libc/init work — building a throwaway one-phase
tar reader to satisfy the sentence would be scaffolding, not progress.
What the gate is *for* — proving the privilege boundary, the DPL=3
gate, the TSS esp0 path, the register marshalling and fault
containment — is exactly what the hand-built image proves. The ELF32
loader task moves to I5 where its real consumer (init) lives.

Two incidents worth the record: (1) the interleave test's first cut had
both workers *yield* in their loops — which tests cooperation, not
preemption; the shipped version hlt-waits in the boot thread and the
workers never yield, so their progress can only come from the PIT path.
(2) The hand-assembled program's padding was one byte short and the
console printed `ING3-OK`; the smoke test asserts the exact string for
precisely this class of bug.

#### Deliverable

`patches/I386_I4_proc.patch`

---

### Phase I5 — 32-bit libc and userspace ✅ COMPLETE (bring-up scope)

**Objective (as delivered):** a real, compiled-from-C init runs Ring 3
from the shared initrd through an ELF32 loader and an `int 0x80` libc.
The original objective ("the init shell and the core `/bin` set") is
**split**: the shell needs a keyboard driver (I7) and the `/bin` set
needs the full libc port (I6) — both re-scoped forward with the
reasoning below, not silently dropped.

#### Tasks

- [x] `lib/libc32/`: `crt0_32.asm` (call main, trap SYS_EXIT inline —
      a crt0 must not depend on a library that is not there),
      `syscall32.asm` (`int 0x80` wrapper, D4 register convention,
      callee-saved EBX/ESI/EDI preserved), `libc32.h` (write/getpid/
      exit/sched_yield + string helpers). `setjmp`/TLS-`errno` move to
      I6 with the archive-libc port they belong to.
- [x] `lib/libc32/user32.ld`: user layout at `0x08048000` inside the
      loader window, headers not loaded, W^X-shaped PHDRS (even though
      i386 cannot enforce the distinction — D3 — the layout keeps
      binaries honest for a PAE-capable future).
- [x] `kernel/arch/i386/initrd32.c`: USTAR reader over the **shared**
      `initrd.tar` — one archive for both kernels, i386 binaries under
      `/bin32`, each loader refusing the other's ELF class.
- [x] `kernel/arch/i386/elf32load.c` (the task moved from I4):
      class/machine/type validation in `elf.c`'s order, bounds-checked
      phdr walk, segments confined to `[0x08000000, 0x40000000)`,
      pages zeroed before mapping (no data leaks into user space).
- [x] `userspace/system/init32/init32.c` + Makefile `user32` target;
      `initrd.tar` gains `/bin32/init32` (stripped, like every other
      shipped binary).
- [ ] ~~shell + core `/bin` set~~ — **I7** (keyboard) and **I6** (libc).
- [ ] ~~`/tests/selftest` gauntlet~~ — **I6** (it is a libc test suite;
      porting it before the libc is porting its skips).

#### Test gate

- i386: the initrd mounts; `/bin32/init32` — ELF32 compiled from C —
  loads, runs Ring 3, writes its banner, round-trips `sched_yield`,
  and **runs the EFAULT negative control from userspace** (write()
  with a kernel pointer must come back refused); `exit(7)` observed by
  the kernel. ✔ (19 assertions in `i386_user_smoke.sh`.)
- x86_64 suite untouched — checked harder this phase because
  `initrd.tar` itself changed: the 64-bit boot must still reach its
  init shell with the fatter archive. ✔

#### Result

```
[initrd] USTAR at phys 01800000, 8420 KiB, 86 files
[elf32] mapped 2 user page(s), entry 08048000
[user] running /bin32/init32 (entry 08048000, 8588 bytes)
AuraLite i386 init: userspace is alive
init32: sched_yield returned
init32: kernel-pointer write refused (EFAULT) -- good
[user] exit(7) via int 0x80
[init] PASS: init32 ran and exited 7 as built
```

**A refusal that paid for itself:** the first `init32` link used a bare
`-Ttext 0x08048000`, and lld emitted a headers-only `PT_LOAD` at its
default `0x00400000` image base — outside the loader's window.
`elf32load_map` refused the binary. The loader was right and the link
was wrong; `user32.ld` is the fix, and the incident is the best
available evidence that the window check is worth having.

**The scope split, argued:** "boots to `auralite#`" as I5's gate would
require either porting the keyboard driver out of order (I7's phase,
with its own gate) or faking a prompt over serial with no reader behind
it. The delivered gate — compiled C in Ring 3, through the real initrd,
with a userspace-driven negative control — is the part of the original
sentence that was *about this phase*. The `auralite#` gate moves to I7
where the hardware it needs lives.

#### Deliverable

`patches/I386_I5_user.patch`

---

### Phase I6 — The pointer-width sweep

**Objective:** the 877 counted `uintptr_t`/`(uint64_t)` sites are each
read once, classified per D6 (`paddr_t` / `uintptr_t` / delete), and the
i386 kernel build is `-Werror`-clean on truncation warnings.

This phase is deliberately *separate* and *late-ordered*: I2–I5 touch the
sites their subsystems need and no more, so the sweep closes the residue
rather than blocking the port. It is also the phase most likely to find
real x86_64 bugs (a virtual address stored in `uint32_t` is a bug on
*both* widths; the sweep just makes it visible).

#### Tasks

- [ ] `paddr_t` introduced in `kernel/lib/` and adopted by `pmm`, `vmm`,
      DMA-facing drivers.
- [ ] `tools/check_width_sweep.py` (new): greps for the banned cast
      patterns and fails CI when the count rises — the same
      drift-prevention shape as `check_test_registry.py` and
      `check_fixes_claims.py`.
- [ ] Every site changed in a subsystem with a host unit test keeps that
      test green on both a 64-bit and a `-m32` host build where the
      harness already supports it (`test_pmm`, `test_bitmap`,
      `test_fat32`, `test_elf` are the known-compilable set).

#### Test gate

- `ARCH=i386` kernel build passes with truncation warnings promoted to
  errors; `tools/check_width_sweep.py` is registered in `make test-unit`
  and fails on regression (with a negative control run, per house rule).

#### Deliverable

`patches/I386_I6_sweep.patch`

---

### Phase I7 — Drivers on i386

**Objective:** the QEMU-standard device set works: e1000, AHCI, PS/2,
framebuffer console.

#### Tasks

- [ ] VBE framebuffer: Stage 2's 32-bit path sets a linear-framebuffer
      mode via VBE 2.0 real-mode calls before `CR0.PE` (the 64-bit path
      gets its framebuffer from the loader too — same contract, same
      `boot_fb_t`).
- [ ] e1000/AHCI descriptor rings: already `uint64_t` on the wire;
      audit bounce conditions (none expected below 4 GiB), enable IRQ
      paths through the 8259 PIC as on early x86_64 phases.
- [ ] PS/2 keyboard/mouse, PIT, UART: port-I/O only — expected to be
      recompile-clean; the task is proving it, not writing it.
- [ ] USB/xHCI, Bluetooth, Wi-Fi, virtio: explicitly deferred; rows in
      the status matrix say so per-arch.

#### Test gate

- i386 QEMU: DHCP acquires a lease and `ping` gets an echo reply
  (`test_e1000_irq`-equivalent under `qemu-system-i386`); AHCI
  read/write self-test passes; the framebuffer console shows the boot
  log (VNC screenshot assert, as `test_graphics.sh` does today).

#### Deliverable

`patches/I386_I7_drivers.patch`

---

### Phase I8 — Filesystems, net, GUI parity

**Objective:** the portable upper layers — VFS, FAT32/ext2, TCP/TLS, the
compositor — run on i386, and the integration suite runs a defined subset
green under `qemu-system-i386`.

#### Tasks

- [ ] `tests/integration/run_all.sh` gains an `IL_ARCH=i386` mode with an
      explicit **supported-case manifest** (the D5/D3/§6 exclusions:
      no SMP cases, no NX cases, no w32/Rust/xHCI cases) — a curated
      list, not a filter, so a skipped case is a decision on record.
- [ ] TLS/crypto: `libatls` is portable C; the win is running its full
      unit suite compiled `-m32` on the host, which catches width bugs
      without a VM in the loop.
- [ ] GUI: the compositor is integer 2D over the framebuffer — expected
      portable; `gltest`'s software GL uses doubles and will be slow on
      x87. Correctness gate yes, FPS gate no.

#### Test gate

- The i386 manifest of integration cases passes end to end in CI-like
  conditions (`bash tests/integration/run_all.sh` with `IL_ARCH=i386`),
  and the manifest file itself is checked against `cases/` by
  `check_test_registry.py` so it cannot silently rot.

#### Deliverable

`patches/I386_I8_parity.patch`

---

### Phase I9 — CI matrix, docs, and the honest table

**Objective:** both architectures build and smoke-test on every push, and
the documentation stops implying x86_64-only facts are universal.

#### Tasks

- [ ] `.github/workflows`: an `ARCH=i386` job building `kernel32` + the
      i386 initrd + the dual image, running `i386_refusal_smoke.sh`,
      `i386_boot32_smoke.sh` and the I8 manifest.
- [ ] `docs/status.md` gains an arch column (or per-arch rows) for every
      feature the plan touched; `README.md` Quickstart documents the
      single-image dual-arch behaviour and the D1 floor (i686).
- [ ] `docs/architecture.md`: the i386 boot flow diagram beside the
      existing one; `docs/syscall_abi.md`: the `int 0x80` register
      contract beside the SYSCALL one.
- [ ] `MATURITY_AUDIT.md`-style claim check: a script asserts this plan's
      phase table agrees with the tree (the `AUDIT_A7` lesson — a plan
      document that can drift, will).

#### Test gate

- CI is green on both arch jobs from a clean clone; the claim-check
  script fails when a phase row and the tree disagree (verified by a
  deliberate one-line negative control during review).

#### Deliverable

`patches/I386_I9_ci.patch`

---

## 4. Order and rationale

```
I0 ── the refusal            (independent; ships alone, day one)
I1 ── dual-kernel chain      (needs I0's check; proves the hand-off)
I2 ── CPU bring-up           (needs I1's entry; introduces arch split)
I3 ── memory                 (needs I2's exceptions to debug #PF)
I4 ── proc + int 0x80        (needs I3's address spaces)
I5 ── libc + userspace       (needs I4's Ring 3)
I6 ── width sweep            (closes the residue I2–I5 left; CI-guarded)
I7 ── drivers                (needs I3 DMA-safe memory; parallel to I5)
I8 ── parity + test manifest (needs I5 + I7)
I9 ── CI + docs              (last, because it asserts what now exists)
```

The dependency chain is the same shape as the original 14-phase bring-up,
compressed — because it *is* the original bring-up, replayed on a smaller
CPU with the portable code already written. The two early phases are the
cheap ones and carry all the user-visible value for owners of 64-bit
machines (nothing changes) and 32-bit machines (a hang becomes a
diagnostic, then a boot).

## 5. Risks

- **The I6 sweep is the cost centre, and it is O(sites-read), not
  O(sites-changed).** 877 counted sites; estimate a low-hundreds change
  count. The mitigation is the two mechanical typing rules (D6) and the
  CI counter that stops regression. The unknown is how many latent
  x86_64 bugs it surfaces; each becomes a `FIXES_PLAN`-style entry rather
  than a silent edit.
- **No NX on i386 (D3) weakens a property the tree is proud of.** The
  mitigation is honesty in the status matrix plus keeping user/kernel
  split enforcement (U/S bit) intact — not pretending segmentation tricks
  restore it.
- **x87-only floating point.** The userspace GL stack and anything
  compiled with SSE assumptions must build with `-mno-sse -mfpmath=387`
  on i386; performance will be poor and `gltest` timing-sensitive asserts
  may need arch-aware budgets.
- **Rust has no `i686-unknown-none` target.** `rustes`/`rsbr` are
  excluded on i386 (§6). If that ever matters, a custom target JSON +
  `-Zbuild-std` is the researched path — deliberately not on this plan.
- **Real 32-bit hardware variance.** QEMU's `qemu32`/`pentium3` models
  are the supported gate, matching the repository's existing "QEMU is
  the primary target" reality check. Real-hardware reports get the same
  experimental labelling x86_64 has.
- **Regression risk to x86_64 is concentrated in I2's `arch.h`
  refactor.** That commit carries its own gate: the default-arch kernel
  binary must be byte-identical before and after.

## 6. What this plan does not do

- **No PAE, no highmem, no >512 MiB RAM on i386.** 2-level paging only
  (D3).
- **No NX emulation.** The loss is documented, not papered over.
- **No 32-bit UEFI (`BOOTIA32.EFI`).** BIOS/CSM only on i386 (D2).
- **No SMP on i386** (D5). BSP-only; the H8 scheduler stays x86_64.
- **No Win32-on-i386.** The w32 personality is PE32+ (64-bit) by design;
  PE32 support is a separate plan if it is ever worth one.
- **No Rust userspace on i386** until a maintained bare-metal 32-bit
  target exists (Risk 4).
- **No 386/486/586 kernel support.** i686 floor (D1); older CPUs get the
  I0 diagnostic, which is more than they get today.
- **No performance parity promises.** Correctness gates run on i386;
  FPS/throughput numbers stay x86_64-only until measured honestly.

## 7. A note on why this is worth doing at all

The 64-bit kernel will never need a 32-bit sibling to serve its users —
QEMU on a modern host is the audience. The reasons are the same ones that
justified the custom bootloader over Limine: the boot chain is *this
project's own code*, and a boot chain that silently wedges on an entire
CPU family has a defect regardless of how rare the family is (I0 fixes
that in a day). And the port itself is the cheapest available audit of a
claim the documentation already makes — that the kernel core is portable
freestanding C. A claim that has never been compiled for a second width
is a hope, not a property. I6's counter turns it into a property that CI
defends.
