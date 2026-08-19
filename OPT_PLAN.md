# AuraLite OS — General Performance Optimization Plan

## Status: IN PROGRESS 🚧 — O0–O4 landed; O5–O9 specified

| Phase | Result | Deliverable |
|-------|--------|-------------|
| O0 — the measuring rig | ✅ complete | `patches/OPT_O0_rig.patch` |
| O1 — word-wide string ops | ✅ complete | `patches/OPT_O1_stringops.patch` |
| O2 — fast-boot self-test knob | ✅ complete | `patches/OPT_O2_fastboot.patch` |
| O3 — buffered UART TX | ✅ complete | `patches/OPT_O3_uart_ring.patch` |
| O4 — compositor: composite the union, sleep when idle | ✅ complete | `patches/OPT_O4_compositor.patch` |
| O5 — precise TLB shootdown | ⬜ todo | — |
| O6 — size-class allocator front | ⬜ todo | — |
| O7 — block where the kernel yields | ⬜ todo | — |
| O8 — linker GC + LTO lane | ⬜ todo | — |
| O9 — CI wiring + claim check | ⬜ todo | — |

This document answers:

> *`FIXES_PLAN.md` repaired what was broken and `MATURITY_PLAN.md` completes
> what is partial. Neither asks the third question: of the things that are
> **correct and complete**, which ones are quietly slow — and what is the
> honest, measured path to making the OS faster without making it different?*

It follows the structure of the existing plans (`FIXES_PLAN.md`,
`MATURITY_PLAN.md`, `RISCV_PLAN.md`, `USB_PLAN.md`): dependency-ordered
phases, a definition of done and a test gate for every phase, one `.patch`
per phase (`patches/OPT_O<n>_*.patch`).

**Baseline:** commit `6bba33f` (HEAD at the time of writing). Every claim
below was checked against the tree or measured in QEMU on this machine, not
assumed. Baseline artefacts, for the before/after tables the phases must
fill in:

```
build/kernel.elf        2 437 296 bytes
build/initrd.tar        8 652 800 bytes
build/auralite.iso     51 380 224 bytes
boot → "shell active"        ~12 s   (test_boot_to_shell, this machine)
boot serial log             25 258 bytes to reach the shell
```

An optimization plan is the easiest kind of plan to write badly, because
"faster" is cheap to claim and expensive to verify. So one rule above all
the others, inherited from the way `MATURITY_AUDIT.md` treats claims:
**no phase in this plan may land without a number from before and a number
from after, produced by a tool that is itself committed in the phase.**
"Feels snappier" is not a deliverable.

---

## 1. How these were ranked

`FIXES_PLAN.md` ranked by danger. Danger is the wrong axis here — nothing
in this plan is broken. The axis that matters for an optimization is:

| Rank | Meaning |
|---|---|
| **Structural** | A cost paid on every operation of a whole subsystem (every `memcpy`, every frame, every shootdown) |
| **Latency** | A cost the user waits behind, visibly (boot time, input-to-photon) |
| **Waste** | CPU burned doing nothing (polling loops that could block) |
| **Footprint** | Bytes that cost load time and cache, not wall-clock directly |

Two things are deliberately **excluded**, because an optimization plan that
adds features is a feature plan wearing a disguise:

- **Missing subsystems** — demand paging is `MATURITY_PLAN.md` M4,
  interrupt-driven virtio-net RX is M7, TCP behaviour is M6 (done),
  scheduler priorities are a semantic change, not a tuning change. Not here.
- **Anything that changes observable behaviour** — every existing unit and
  integration case must pass unmodified. A phase that needs a test edited
  to go green is changing semantics and must say so loudly (only O2 does,
  for boot-time self-tests, and it keeps the old behaviour reachable).

### The list, ranked

| # | Cost, measured in the tree | Rank | Phase |
|---|---|---|---|
| 1 | `memcpy`/`memset` are byte-at-a-time loops; every frame flip, COW copy and spawn pays them | **Structural** | O1 |
| 2 | Boot spends whole seconds on unconditional self-tests (one of them literally waits 1 s) and on synchronous UART logging | **Latency** | O2, O3 |
| 3 | The compositor re-composites the entire back buffer for a 16×16 cursor rectangle, and busy-yields between frames | **Structural + Waste** | O4 |
| 4 | Every TLB shootdown IPI reloads CR3 on every CPU — a full flush for a one-page unmap | **Structural** | O5 |
| 5 | `kmalloc` is a first-fit linked-list walk; the slab layer exists but serves only 3 caches | **Structural** | O6 |
| 6 | `wait4` and `getrandom` yield-poll in loops although `wait_queue.c` exists precisely to avoid that | **Waste** | O7 |
| 7 | The kernel links every function it ever compiled; no section GC, no LTO | **Footprint** | O8 |

---

## 2. Where things actually stand

Measured against the tree. File and line numbers are from the baseline
commit; if they drift, the *claims* are what the phases answer to.

### Fact 1 — The kernel's `memcpy` moves one byte per iteration, and everything hot sits on top of it

`kernel/lib/string.c` says it itself:

```c
/* Compiled with -mno-sse, so these are plain scalar loops (no vectorisation). */
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) { *d++ = *s++; }
    return dst;
}
```

`-mno-sse` is the right call in a kernel that doesn't do lazy FPU for
kernel threads — but "no SSE" does not mean "one byte at a time".
`rep movsb` needs no SSE state at all, and on every microarchitecture QEMU
emulates (and every real one since Ivy Bridge, via ERMSB) it is the fastest
general copy available to kernel code. The current loop is the slowest.

Who pays, per the tree:

- `gfx_flip()` (`drivers/framebuffer/graphics.c:220`) — one `memcpy` per
  scanline of the whole screen, on every full frame.
- COW fault copies and `paging_clone_user_space()` — a page per fault.
- `spawn` reads the whole ELF image through a 1 MiB bounce buffer
  (`kernel/proc/process.c:771`).
- Every implicit struct copy the compiler emits, because these symbols are
  what `-ffreestanding` code links against.

The same byte loops are duplicated in `lib/libc/src` for user space, where
SSE **is** available and even `rep movsb` would be a large win.

### Fact 2 — Boot pays for its own honesty, every single time

The boot path runs its self-tests unconditionally. Three of them are not
free, and one is spectacular:

- `drivers/timer/pit.c:164` — *"self-test: measuring 1-second delay..."* —
  the timer self-test **spins for a real wall-clock second on every boot**.
- The heap self-test runs 10 000 alloc/free cycles; the PMM self-test
  allocates and frees 1 000 frames; the RNG self-test generates and
  analyses 16 KiB. Cheap individually, not collectively.
- All of it is narrated: the serial log is 25 258 bytes by the time the
  shell starts, and every byte goes through `uart_putchar()`
  (`drivers/uart/uart.c:18`), which **busy-waits on LSR.THRE per byte**
  under `kprintf`'s global `print_lock`. At 115 200 baud that is ~87 µs of
  spinning per character — over **2 s of boot** spent waiting on a UART,
  with a lock held that every other CPU's `kprintf` contends on.

The self-tests are a load-bearing part of this project's culture — every
boot is a smoke test, and the integration suite greps for their PASS lines.
The point is not to delete them. The point is that a *user* boot and a *CI*
boot have different jobs, and only CI's job requires the 1-second wait.

### Fact 3 — The dirty-rect compositor composites everything and clips only the flip

H1 (`HARDENING_PLAN.md`) landed dirty rectangles, and the idle path is
genuinely free now (`gui.c:2310`: *"Idle frame — no work needed"*). But
look at what the partial path actually does (`kernel/gui/gui.c:1652`):

```c
static void compositor_render_dirty(void) {
    /* Re-composite the entire back buffer. */
    draw_desktop();
    draw_icons();
    ...
```

The full desktop, every icon and every window is redrawn into the back
buffer; the dirty union clips only the final `gfx_flip_rect()`. Moving the
mouse one pixel re-renders the whole scene and then copies 16×16 pixels of
it. The flip was the cheap half; the composite is where the bandwidth is —
through the byte-loop `memcpy` of Fact 1, twice (window→back, back→front).

And between frames, the compositor thread does not sleep
(`kernel/gui/gui.c:2339`):

```c
uint64_t target = timer_get_ticks() + 1; /* ~100 Hz / 100 FPS */
while (timer_get_ticks() < target) {
    sched_yield();
}
```

That is a yield-spin at 100 Hz forever, GUI activity or not — scheduler
churn that `sched_get_idle_ticks()` will happily quantify.

### Fact 4 — A one-page unmap flushes every TLB on every CPU

`kernel/arch/x86_64/tlb_shootdown.c`, in its entirety:

```c
void ipi_tlb_shootdown_handler(void) {
    /* Full TLB flush: reload CR3. */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    lapic_eoi();
}
```

No address is carried in the request, so the handler cannot `invlpg`; it
nukes the world. Every `munmap`, every `mprotect`, every COW resolution
that needs remote invalidation costs every other CPU its entire TLB —
including all kernel translations, on a kernel that runs `-smp 4` in its
own default `make run`. The H8 scheduler made cross-CPU activity normal;
the shootdown path still behaves as if it were rare.

### Fact 5 — `kmalloc` walks a list; the slab allocator watches from the bench

`kernel/mm/heap.c:1` — *"generic first-fit heap allocator with boundary-tag
coalescing"*. First-fit over an intrusive free list is O(free blocks) per
allocation, and the free list is LIFO (*"Insert at head: O(1). Order does
not matter for first-fit correctness"* — heap.c:48), which is exactly the
ordering that maximises fragmentation-driven walk length over time.

Meanwhile `kernel/mm/slab.c` exists, is initialised, and serves precisely
three caches (`tcb_cache`, `ofd_cache`, `vnode_cache`). Every network
buffer, GUI event, path buffer and DER blob goes to the first-fit walk.

### Fact 6 — The kernel owns a real wait_queue and then polls anyway

`kernel/proc/wait_queue.c` backs pipes, futexes, `select()` and
`nanosleep()` — the infrastructure is done and tested. And yet:

- `do_waitpid()` ends its loop with `sched_yield()`
  (`kernel/proc/thread.c:429`) — the shell burns its quantum re-scanning
  the zombie list for as long as a child runs. `docs/status.md` admits it:
  *"wait4 — yield-polling"*.
- `getrandom()` blocks-by-yielding too (`kernel/arch/x86_64/syscall.c:2293`,
  *"poll with yields"*).
- The compositor's frame pacing is Fact 3's yield-spin.

Three call sites, one missing pattern: an event (`child exited`,
`pool seeded`, `frame due`) that `wait_queue_wake` already knows how to
deliver.

### Fact 7 — Nobody has ever asked the linker to throw anything away

`LDFLAGS := -nostdlib -static -T kernel.ld -z max-page-size=4096` — no
`--gc-sections`, and `CFLAGS` has no `-ffunction-sections`/`-fdata-sections`.
The 2.4 MB `kernel.elf` carries every function of every subsystem ever
linked in, reachable or not. LTO has never been evaluated. This is the
lowest-stakes item in the plan and it is ranked accordingly (last real
phase), but it is also the only one that shrinks the artefacts CI copies
around on every run.

---

## 3. Decisions

Numbered so later phases can cite them instead of re-arguing.

**D1 — Measure first, in-tree, or it didn't happen.** Every phase's gate
includes a before/after number produced by tooling committed in O0. The
numbers go into this document's phase table when the phase lands (the
`RISCV_PLAN.md` convention: the plan is also the report).

**D2 — QEMU/TCG numbers gate nothing hard.** TCG timing is noisy and
host-dependent; CI runners doubly so. Perf assertions in integration cases
are **ratchets against absurdity** (e.g. "boot-to-shell under 60 s", "idle
frame does zero composite work") — not microbenchmark thresholds. The
microbenchmark numbers are recorded and human-reviewed, the same way
`MATURITY_AUDIT.md` reviews claims.

**D3 — Behaviour-preserving by construction.** The full unit + integration
suites are a *precondition* of every gate, unmodified. The single
behavioural change in the plan (O2's fast-boot default) must keep the old
behaviour selectable and CI must select it, so the self-test greps keep
their teeth.

**D4 — The panic path stays synchronous.** O3 makes logging buffered;
`panic`/`kernel_halt` and early boot (before the IRQ layer) must keep the
direct busy-wait UART path. A faster boot that eats the last words of a
dying kernel is a bad trade, and this decision is the fence around it.

**D5 — x86_64 first, residue recorded, parity not promised.** The i386,
rv64 and a64 kernels share some of these costs (byte-loop string ops,
polling waits). Each phase states what transfers and leaves a matrix line,
the way `RISCV_PLAN.md` V8 does — but this plan's gates run on the x86_64
tree only. Cross-arch parity is its own follow-up when the numbers justify
it.

---

## 4. The phases

Dependency order: the rig first (O0), then the copy engine everything else
is measured through (O1), then latency (O2–O3), bandwidth (O4), SMP costs
(O5), allocator (O6), waste (O7), footprint (O8), and the wiring/docs
close-out (O9).

---

### O0 — The measuring rig

**Status: ✅ landed** (`patches/OPT_O0_rig.patch`). What O0's first boot
measured is recorded at the end of this section, including one fact
nobody knew was in the tree.

**Objective:** make every later claim in this plan checkable by `make`.

Nothing in the tree today can answer "how long did boot take" or "how many
bytes did the compositor move" without a stopwatch and faith. This phase
builds the shared instruments; it optimizes nothing.

Tasks:

- [x] `kernel/lib/perfstat.c`: a small fixed table of named monotonic
      counters (`perfstat_add(id, n)`), lock-free per-CPU accumulation,
      exposed read-only through `/proc/perf` (procfs already exists).
      Counters wired in this phase: `boot_ticks_to_shell`,
      `compositor_frames_full`, `compositor_frames_partial`,
      `compositor_pixels_composited`, `compositor_pixels_flipped`,
      `tlb_shootdowns_full`, `kmalloc_walk_steps`, `uart_tx_sync_bytes`.
      Cost when idle: zero — an unread counter is an `add`.
- [x] Boot timestamping: `kmain` records `timer_get_ticks()` at the
      "shell active" line; the value lands in `/proc/perf` and is printed
      once, so serial-log tooling can grep it.
- [x] `userspace/tests/membench`: user-side copy/set microbench (sizes
      64 B – 1 MiB, aligned/misaligned), prints a fixed-format table.
      Runs from the shell like every other test binary.
- [x] `tests/integration/cases/test_perf_smoke.sh`: boots, reads
      `/proc/perf` and the membench table, asserts only D2-grade ratchets
      (boot under 60 s; idle full-recomposite count bounded to one-shot
      events), and archives the numbers into
      `build/integration-logs/perf_smoke.log` for the phase tables here.
- [x] Register the case in `run_all.sh` (the registry check will insist
      anyway).

**What O0 measured on its first boot** (QEMU/TCG, this machine; §6 has
the table):

- `kmalloc_walk_steps` = **665 394 free-list nodes visited in one boot**
  — Fact 5 now has a number, and it is a bigger number than expected.
- `uart_tx_sync_bytes` = 24 778 by the shell prompt — Fact 2's ~2 s of
  115200-baud spin, counted byte by byte.
- Composited vs flipped pixels, UEFI boot: **64 512 000 composited,
  4 546 560 flipped — a 14.2× gap.** Fact 3's "the composite is where
  the bandwidth is" is now a ratio, and it is O4's budget.
- **A fact nobody had written down: the BIOS boot path has no pixels.**
  The BIOS Stage 2 sets no VBE mode (there is no `vbe.inc` in
  `boot/bios/stage2/` — check), so on the SeaBIOS path
  `boot_fb_t.bpp != 32`, `gfx_init()` returns early, `gfx_get_width()`
  is 0, and the entire GUI pipeline runs *dimensionless*: the compositor
  ticks, windows exist, and not one pixel moves.  Every GUI integration
  case that boots BIOS (all of them) exercises window/event logic only.
  The counters made this visible on their first read — `frames_full 2,
  pixels_composited 0` is not a counter bug, it is the tree.  Recorded
  for O4: the compositor gate must boot UEFI (OVMF) to measure real
  pixels, and "BIOS boots get a framebuffer" is a bootloader follow-up
  outside this plan's scope.
- membench baseline (BIOS guest, byte-loop libc): aligned `memcpy` is
  **6–11 MB/s at every size** under TCG — the byte loop is so dominant
  that size and alignment barely matter.  (One outlier: the JIT
  occasionally traces the misaligned 1 MiB loop to ~76 MB/s; recorded,
  not relied on — D2.)

**Definition of done:** the counters exist, cost nothing measurable when
unread, and the smoke case records a complete baseline table on this
machine, committed into §6 of this document.

**Test gate:** `test_perf_smoke` green; full suites green (D3);
`tools/check_test_registry.py` green.

**Deliverable:** `patches/OPT_O0_rig.patch`

---

### O1 — Word-wide string ops (`rep movsq` in the kernel, real routines in libc)

**Status: ✅ landed** (`patches/OPT_O1_stringops.patch`).  Measured
results and one TCG lesson at the end of this section.

**Objective:** retire the byte loops of Fact 1 in both worlds.

Tasks:

- [x] Kernel: the fast backend lives in
      **`kernel/arch/x86_64/string_fast.c`** — NOT in
      `kernel/lib/string.c` as first drafted, because the V6 asm ratchet
      holds portable files at zero inline assembly and it is right to.
      `memcpy`/`memset` are `rep movsq` bulk + `rep movsb` tail with the
      sub-64 B scalar path; `kernel/lib/string.c` keeps the portable
      bodies under `#ifndef ARCH_X86_64` (host tests compile exactly
      those; rv64/a64 inherit them when they adopt the shared tree).
      `memmove` keeps DF untouched: forward = memcpy's rep path
      (overlap-safe in that direction by definition), backward = 8-byte
      tail-first chunks — not `std; rep movsb` because backward rep gets
      no fast-string path on any microarchitecture, even though the
      entry stubs do `cld` (isr_stubs.asm:73, checked).
- [x] `memcmp`/`strlen` word-wide in portable C (both kernel and libc):
      8-byte `__builtin_memcpy` loads, has-zero-byte trick for strlen
      (aligned reads only — cannot cross a page), byte-scan of the first
      differing word for memcmp.  No `rep cmpsb` — micro-coded
      byte-at-a-time everywhere.
- [x] `lib/libc/src`: same shapes for user space (`libc.c` memcpy/memset,
      `string_extra.c` memmove — whose forward path is a self-contained
      8-byte loop, not a memcpy call, because
      `tools/extract_libc_impls.py` extracts memmove standalone for the
      host stdio tests and an unresolved symbol there is a build error).
- [x] The i386 and rv64/a64 trees keep their loops; residue line per D5
      (`rep movsq` transfers to i386 as `rep movsd`; rv64/a64 want the
      8-byte loops `kernel/lib/string.c` now carries).

**What O1 measured** (membench, QEMU/TCG, BIOS guest; §6 table):

- **The first draft used `rep movsb` for everything and membench refused
  to move: 11 MB/s before, 11 MB/s after.**  TCG emulates rep-string
  ops one *iteration* at a time, so a byte-element rep IS a byte loop
  there — while the 8-byte-loop memmove in the same boot jumped
  144 → 1242 MB/s.  The bulk became `rep movsq` (8 bytes/iteration —
  and on real hardware ERMSB covers movsq too, so nothing is lost
  where it matters).  D1 worked exactly as designed: a "faster" that
  the rig could not see did not get to call itself faster.
- After the movsq rework: memcpy 4 KiB–1 MiB **11 → 73–82 MB/s (~7×)**,
  memcpy 64 B 6 → 18–20 (call overhead bound), memset 1 MiB
  **342 → 1687 MB/s**, memmove 64 KiB **144 → 1236 MB/s**.
- Boot-to-shell: 505 → 497 ticks — statistically nothing, as expected:
  boot is wait-dominated (O2/O3's business), not copy-dominated.

**Definition of done:** membench shows the improvement and the crossover
choice; no suite regression. ✓ (gltest/selftest/boot cases green,
`test-unit` EXIT 0.)

**Test gate:** `tests/unit/test_string_ops.c` — the fast backend and the
word-wide portable routines across alignment (0–8) × size (0–1024,
straddling the 64 B crossover) × overlap (both directions, distances
1–9) matrices against a byte-loop reference, with guard canaries around
every destination: **3052 checks, 0 failed**, registered in
`UNIT_TESTS`. ✓

**Deliverable:** `patches/OPT_O1_stringops.patch`

---

### O2 — Boot latency: make the 1-second self-test opt-in without blinding CI

**Status: ✅ landed** (`patches/OPT_O2_fastboot.patch`).  Two corrections
to this section's own spec and two measured surprises below.

**Objective:** cut multi-second, fixed costs out of the default boot while
keeping every self-test reachable and CI-enforced (D3).

**Correction 1 — the knob's channel.**  The spec above said "from the
kernel command line the loaders already pass".  Measured: **no cmdline
plumbing exists anywhere in `boot/`** — neither the BIOS Stage 2 nor
`BOOTX64.EFI` passes one, and `boot_info_t` has no field for it.  The
landed channel is QEMU **fw_cfg** (`-fw_cfg
name=opt/auralite.selftest,string=full|fast|off`, read by
`kernel/arch/x86_64/fwcfg.c` — port I/O stays in the arch tree, the I6
ratchet is why) with the **build default** as the fallback
(`make SELFTEST=full|fast|off`, the FIX_R8 KEYMAP precedent — and all
real hardware gets).  A QEMU-shaped knob for a QEMU-primary project is
the honest fit; a bootloader cmdline is real follow-up work that belongs
to a bootloader plan.

Tasks:

- [x] `kernel/lib/selftest.{c,h}`: mode state + `selftest_scale(full,
      fast)`; `full` = historical values and byte-identical output,
      `fast` = same invariants at reduced sizes, `off` = skip printing
      `SKIPPED (selftest=off)`.  Default `fast` via `make SELFTEST=`.
- [x] `kernel/arch/x86_64/fwcfg.c`: fw_cfg signature check, FILE_DIR
      walk (big-endian fields composed byte-by-byte), reads
      `opt/auralite.selftest`.  Absent/garbage → build default stands.
- [x] Scaled: PIT verification window 1 s → 100 ms (±5% band → ±20%,
      ±1-tick quantisation at ~10 ticks; the divisor programming is
      untouched), PMM 1000 → 100 frames, heap 10 000 → 500 cycles
      (including its O(N²) uniqueness scan), RNG analysis 16 KiB → 2 KiB.
      **Seeding is never skipped** — the knob trades boot-time
      statistics, not entropy.
- [x] `tests/integration/lib/lib.sh`: every CI boot passes
      `-fw_cfg ...,string=full` (override: `IL_SELFTEST`).  All existing
      self-test greps hold **unmodified** — the D3 tripwire held.
- [x] `tests/integration/cases/test_selftest_modes.sh` (registered): all
      three modes boot; full asserts the historical lines (1-second
      delay, 1000 frames, 10000 cycles, 16 KiB), fast asserts the scaled
      lines still PASS, off asserts four loud SKIPPEDs and a live shell;
      plus the D1 gate: fast beats full by ≥ 80 ticks.
- [x] `test_perf_smoke.sh` records both modes' boot ticks every run.

**What O2 measured:**

- Boot-to-shell, BIOS guest: **full 499 ticks (~5.0 s) → fast 400 ticks
  (~4.0 s)** — almost exactly the 1-second PIT window; the heap/PMM/RNG
  gauntlets are noise under TCG next to it.  The remaining ~4 s of the
  fast boot is not self-tests at all: it is the boot *demo sequence*
  (`r3d_demo(30)` renders 30 frames of 3D demo, `wm_demo`,
  `gui_self_test` windows) plus device-init waits — recorded here as the
  boot-latency residue, because deleting demos is a behaviour decision,
  not an optimization.
- **Surprise 1 (statistics):** the RNG byte-frequency band (±50%) was
  calibrated at λ=64 and pierced immediately at fast's λ=8 — a healthy
  boot printed `FAIL (byte 0x04 count 14, expected ~8)`, exactly the
  Poisson math predicts (~6% per bucket × 256 buckets).  Fast mode now
  keeps only a 4× upper bound (stuck generators miss it by orders;
  counter generators are the bit-runs test's catch).  Full keeps the
  historical band, byte-identical.
- **Surprise 2 (cascade):** the faster boot reaches `rng_init()` before
  the interrupt-jitter pool has 128 estimated bits (60 at that point),
  so fast boots exercise the *late opportunistic seeding* path
  (`rng_jitter_event`/`rng_available`) that slow boots never used —
  seeding completes ~1 s later and the self-test runs there.  A latent
  code path became load-bearing because the boot got faster; it held.

**Definition of done:** default boot drops by ≥ 1 s ✓ (99 ticks);
`selftest=full` output byte-identical ✓ (every existing grep unmodified).

**Test gate:** `test_selftest_modes` 30/30; `test_boot_to_shell` 17/17
under the lib's `full` pin; `test_perf_smoke` 18/18 + both-modes record;
`make test-unit` EXIT 0. ✓

**Deliverable:** `patches/OPT_O2_fastboot.patch`

---

### O3 — Buffered, interrupt-driven UART TX (panic path exempt, D4)

**Status: ✅ landed** (`patches/OPT_O3_uart_ring.patch`).  Measured
results — including the one this phase deliberately does NOT get to
claim — at the end of the section.

**Objective:** stop paying 87 µs of spin per logged byte under a global
lock (Fact 2), without losing a single byte of any log CI reads.

Tasks:

- [x] TX ring (16 KiB static) in `drivers/uart/uart.c`; the index core
      is pure C in `drivers/uart/uart_ring.h` (free-running uint32
      counters, power-of-two mask) so the host unit test can try the
      wrap/full/empty off-by-ones without a UART in the room.
      `uart_putchar` enqueues, then drains OPPORTUNISTICALLY — up to one
      FIFO burst (16 bytes) whenever LSR.THRE is already set, never
      spinning; the THRE interrupt (IRQ 4) carries the rest and is
      enabled only while the ring is non-empty.
- [x] Ring-full policy: drain synchronously until there is room — never
      drop (D3: the suite's greps stand on byte-fidelity).  Spill bytes
      count into `uart_tx_sync_bytes`.
- [x] `uart_flush()`: latch back to synchronous mode, BOUNDED lock
      acquire (a dead lock-holder must not silence the dying words),
      drain, disable THRE.  Called from `kernel_halt()` — which the
      fatal `#DF`/panic paths already funnel through (D4).
- [x] Early boot synchronous via the mode latch; kmain arms the ring
      right after the IDT/PIC are live.  The IRQ-4 handler is registered
      through a thunk in kernel.c: uart.c deliberately includes no
      x86_64 headers (the I6 include ratchet held at 69), and IRQ 4
      survives the IOAPIC takeover via the identity-mapped GSI 4 entry
      (`ioapic.c` programs all ISA GSIs, measured before wiring).
- [x] `perfstat`: `uart_tx_ring_bytes` joins `uart_tx_sync_bytes`;
      the smoke test asserts the ring carries the majority.

**What O3 measured:**

- Counter proof of adoption: **ring 24 106 bytes vs sync 748 bytes** at
  the prompt — the sync remainder is exactly the pre-ring boot banner
  (everything printed before the IDT is live), i.e. the ring carries
  ~97% of the log.
- Byte-fidelity held with zero grep edits: `test_boot_to_shell` 17/17,
  `test_selftest_modes` 30/30, and both death-path cases
  (`test_panic_diag`, `test_ist_double_fault`) green — the flush gets
  the last words out through `kernel_halt()`.
- **The claim this phase does not get to make: boot did not get
  faster under QEMU** (full 499 → 501 ticks, noise).  QEMU's chardev
  accepts serial bytes at effectively infinite baud — LSR.THRE is
  almost always already set, so the old busy-wait was nearly free
  *there*.  The 87 µs/byte cost is real-hardware-shaped (115 200 baud
  wire), and per D1/D2 the plan records the counter proof and leaves
  the wall-clock claim to a real-hardware follow-up.  What QEMU does
  keep: `kprintf`'s global print_lock no longer holds all CPUs hostage
  to the wire — the locked window is now an enqueue, which matters exactly
  when SMP contention does.

**Definition of done:** serial logs byte-identical (the suites are the
proof) ✓; `uart_tx_sync_bytes` ≈ pre-ring banner + spill only ✓ (748);
boot-time component honestly recorded as QEMU-invisible ✓.

**Test gate:** `tests/unit/test_uart_ring.c` — 75 checks over
empty/full boundaries, 1000 interleaved wrap laps, and the 2^32 counter
crossing; full suites green (`test-unit` EXIT 0, boot/panic/modes cases
above). ✓

**Deliverable:** `patches/OPT_O3_uart_ring.patch`

---

### O4 — Compositor: composite what is dirty, sleep when nothing is

**Status: ✅ landed** (`patches/OPT_O4_compositor.patch`).  The 14.2×
headroom O0 measured is collected below — 10.8× of it on the steady
state, with the remainder named as overdraw.

**Objective:** finish what H1 started (Fact 3): the dirty union should
bound the *work*, not just the flip — and the frame loop should block,
not yield-spin.

Tasks:

- [x] The clip lives in the gfx layer, not threaded through forty draw
      calls: `gfx_clip_set()/gfx_clip_clear()` in
      `drivers/framebuffer/graphics.c`, enforced by `gfx_putpixel` and
      the `gfx_fill_rect` bulk path (every other primitive — lines,
      text, circles, gradients, window blits — funnels through those
      two).  `compositor_render_dirty()` computes the union FIRST, arms
      the clip, re-runs the scene, flips the union, clears the clip.
      Windows (shadow included) that miss the union are rejected before
      their per-pixel loops run.
- [x] `compositor_pixels_composited` is now the gfx layer's REAL
      post-clip store count (overdraw included) — a plain non-atomic
      accumulator drained once per frame, because the back buffer has
      exactly one writer.  The O0 full-screen approximation is gone.
- [x] The 100 Hz yield-spin is gone.  The compositor blocks on a
      wait_queue; pokes come from the keyboard/mouse IRQ enqueue points
      (at the ring-push, not the handler tail — early returns after
      enqueue would have skipped a tail poke), from one chokepoint in
      the GUI syscall layer (`syscall_gui_call`/`_theme` wrappers, not
      forty case arms), and from a 1 Hz PIT line for the clock and
      notification expiry.  Pacing inside the drain loop is
      `timer_sleep_ms(10)` — a BLOCKING sleep, so the pacing gap is
      idle time the scheduler can give away.
- [x] **`wait_queue.c` is IRQ-safe now** (all five lock sites take the
      queue lock irqsave).  Before, waking from IRQ context was a latent
      deadlock — an IRQ landing on a CPU that held `wq->lock` would spin
      on its own lock forever.  The GUI pokes needed the guarantee; O7's
      wait4/getrandom conversions inherit it.
- [x] The `full_dirty` escape hatch is untouched (overflow, theme
      change, resize still force a full redraw).
- [x] Residue (unchanged from the spec): the framebuffer's cacheability
      (PAT/WC) is real-hardware work and stays deferred — TCG cannot
      measure it (D1).

**What O4 measured** (UEFI/OVMF 1280×800, `test_gui_dirty_uefi.sh` +
manual before/after boots of the O3 and O4 trees):

- Steady state (the 1 Hz taskbar-clock frame): **1 024 000 px composited
  per frame before — the entire screen, every second — vs 94 805 px
  after**, a **10.8×** reduction.  The flip was already union-clipped
  (40 960 px = exactly the 1280×32 taskbar strip); now the composite is
  too.
- The remaining 2.3× composited:flipped gap on a clock frame is
  *overdraw* (desktop gradient + taskbar fill + text repainting the same
  strip), named and accepted — eliminating overdraw is a z-culling
  compositor, a different machine than this plan builds.
- Idle CPU (at the shell, UEFI, `-smp 2`): `/proc/loadavg` busy
  **42.30 → 34.37** — the compositor's yield-spin contribution is gone;
  the remainder belongs to the USB/HID polling threads (O7's sweep
  territory, recorded there).
- Full redraws while idle: **0** over the watch window (was: the
  escape-hatch events only — the bound held at ≤ 1).

**Definition of done:** cursor/clock frames composite ≤ the union
(counter-checked: 948 090 px over a ~5 s idle window, threshold 3 M,
regression signature 8.2 M) ✓; GUI thread idle churn gone (loadavg
delta) ✓; existing GUI cases green unmodified ✓.

**Test gate:** `test_gui_dirty_uefi.sh` (new, registered; loud-skips
without OVMF, the bl6 convention) 8/8 — GOP present, composite bounded,
clock still paints, full redraws one-shot; `test_gui` 5/5,
`test_gui_bad_pointers` 2/2, `test_boot_to_shell` 17/17,
`test_perf_smoke` 19/19, `make test-unit` EXIT 0, width-sweep ratchets
359/359 (the first draft of the pixel accounting added two uint64_t
casts and was reworked — the ratchet's third catch in this plan). ✓

**Deliverable:** `patches/OPT_O4_compositor.patch`

---

### O5 — Precise TLB shootdown

**Objective:** stop reloading CR3 on every CPU for every unmap (Fact 4).

Tasks:

- [ ] Carry a payload with the IPI: a small per-CPU mailbox
      (`{cr3, va_start, page_count, generation}`) written before
      `lapic_send_ipi`; the handler `invlpg`s up to
      `TLB_INVLPG_MAX` (start at 32, measure) pages and falls back to the
      CR3 reload above that — the current behaviour remains the safe
      ceiling, not a removed path.
- [ ] Filter the target set: an address-space CPU mask maintained at
      context switch (`paging_switch`/scheduler already know the CR3 they
      load). CPUs not running the affected address space and not holding
      its translations (they reloaded CR3 since — the generation counter
      answers this) are skipped entirely. Kernel-range flushes still
      broadcast.
- [ ] `perfstat`: `tlb_shootdowns_full` vs `tlb_shootdowns_ranged` vs
      `tlb_ipis_skipped`.
- [ ] PCID/`invpcid` recorded as residue: QEMU TCG supports it, the win is
      real-hardware-shaped, and it doubles the state space — deferred, per
      the same reasoning as O4's WC mapping. The mailbox layout leaves room
      for a PCID field so the follow-up is additive.

**Definition of done:** under the existing `mmapshare`/`mmapfile`/COW
integration cases at `-smp 4`, full flushes drop to (kernel-range +
overflow) only; no new stale-TLB faults (the spurious-fault detector at
`paging.c:559` is the tripwire, and it already logs).

**Test gate:** `test_mmap*`, `test_fpu_smp`, `test_selftest` at `-smp 4`
green across 10 consecutive runs (the flakiness-hunting convention from
FIX_R2); counter assertions in `test_perf_smoke`; a host unit test for the
mask/generation logic as plain C (`tests/unit/test_tlb_mask.c`).

**Deliverable:** `patches/OPT_O5_tlb.patch`

---

### O6 — Allocator: size classes in front of first-fit

**Objective:** make the common `kmalloc` O(1) (Fact 5) without rewriting
the heap or destabilising anything that holds pointers into it.

Tasks:

- [ ] Segregated free lists for size classes ≤ 4 KiB (16, 32, 64, …, 4096)
      layered **in front of** the existing first-fit region: a class miss
      refills from the first-fit heap in slabs of N objects; frees return
      to the class list. Larger allocations fall through to first-fit
      unchanged. Boundary-tag coalescing keeps working underneath because
      class refills are ordinary heap blocks.
- [ ] `kmalloc_walk_steps` counter before/after — the walk length is the
      claim, so it is the measurement.
- [ ] Extend the existing heap self-test (O2's short form included) with
      class-boundary and refill/drain cases; keep the 10 000-cycle
      gauntlet as the `selftest=full` form.
- [ ] Explicit non-goal: no new callers move to `slab.c` in this phase —
      one allocator change at a time (the same discipline the tree applied
      by shipping slab with exactly three caches).

**Definition of done:** `kmalloc_walk_steps` per boot drops by an order of
magnitude; alloc/free microbench (membench grows a kernel-driven mode via
a debug syscall guarded to `selftest=full` boots) shows the O(1) path; heap
self-tests green in both forms; zero suite regressions.

**Test gate:** `tests/unit/test_sizeclass.c` (host build of the class
logic, the refill/drain/coalesce interactions, and adversarial
alloc/free interleavings); full suites (D3); numbers into §6.

**Deliverable:** `patches/OPT_O6_alloc.patch`

---

### O7 — Block where the kernel currently yields

**Objective:** delete the yield-polls of Fact 6 using the wait_queue that
already exists for exactly this.

Tasks:

- [ ] `wait4`: a per-parent (or single global, first) child-exit
      wait_queue; `thread_exit_with_code`/`zombie_enqueue` wakes it;
      `do_waitpid` blocks with the existing SA_RESTART-compatible
      interruptible-sleep pattern `select()` already uses. `WNOHANG`
      unchanged.
- [ ] `getrandom`: a seeded-event wait_queue woken once by
      `rng`'s seeding path; the 30 s give-up becomes a timed wait.
- [ ] The compositor is already converted in O4; this phase is the sweep
      that greps `sched_yield()` loops in `kernel/` and either converts
      them or records each survivor with a one-line justification in this
      section (the `RISCV_PLAN.md` V6 sweep shape: a counted residue,
      driven to a number, published).
- [ ] Idle-tick accounting (`sched_get_idle_ticks`) is the system-level
      measurement: a shell `wait`ing on a sleeping child should idle the
      CPU, not run it.

**Definition of done:** while a child sleeps 2 s and the parent waits,
idle ticks accumulate at ≈ wall-clock rate (they measurably do not today);
`stoptest`/`proctest`/`test_stopped` green unmodified — SIGSTOP/WUNTRACED
interactions are the risk surface and their cases are the fence.

**Test gate:** full suites, with `test_stopped` and `test_posix_p10`
called out for 10 consecutive runs (signal/wait interaction is where
blocking conversions historically break); idle-ratio assertion in
`test_perf_smoke`.

**Deliverable:** `patches/OPT_O7_blocking.patch`

---

### O8 — Footprint: let the linker collect garbage

**Objective:** stop shipping unreachable code (Fact 7); measure, don't
assume, what LTO buys on top.

Tasks:

- [ ] `-ffunction-sections -fdata-sections` in kernel CFLAGS,
      `--gc-sections` in LDFLAGS; audit `kernel.ld` for sections that are
      reached only from assembly or from tables (ISR stubs, the syscall
      table, `.init_array`, multiboot-style headers) and pin them with
      `KEEP()` — this audit *is* the phase's work, and each `KEEP` gets a
      comment saying who reaches it.
- [ ] The same for the user linker script and libc archives (user ELFs
      shrink initrd, which shrinks the ISO and every CI copy of it).
- [ ] ThinLTO behind `make LTO=1`, off by default: a full suite lane must
      pass with it on before it can ever become default (recorded as a
      follow-up decision, not made here).
- [ ] Sizes recorded per artefact in §6 (kernel.elf, each user ELF class,
      initrd.tar, ISO).

**Definition of done:** artefact size table filled; boot and full suites
green with GC on (default) and with `LTO=1` (one CI lane); no symbol
needed at runtime was collected (the suites are the proof — this is why
O8 lands late, when the perf smoke also watches counters that would notice
a dead subsystem).

**Test gate:** full suites in both configurations; `SHA256SUMS` regenerated;
a negative control in the spirit of the POSIX drift check: remove one
`KEEP()` on the ISR stubs, assert the build **fails or the boot smoke goes
red loudly**, restore it — proving the pins are load-bearing.

**Deliverable:** `patches/OPT_O8_gc_lto.patch`

---

### O9 — CI wiring, docs sync, and the claim check

**Objective:** close the loop so this document cannot drift from the tree
(the `AUDIT_A7` lesson: a plan that says PLANNED while its work is shipped,
or COMPLETE while it isn't, is worse than no plan).

Tasks:

- [ ] `test_perf_smoke` registered and running in the CI integration job;
      its recorded numbers archived as a build artefact.
- [ ] `tools/check_opt_claims.py` (the `check_fixes_claims.py` shape): ties
      each phase's checkbox state to the existence of its patch, its test
      gate's registration, and its §6 table row — run in CI.
- [ ] `docs/status.md` gains a Performance section with the counters and
      the current numbers; `README.md`'s feature list is *not* touched
      (speed is not a feature bullet, it is a table).
- [ ] `CHANGELOG.md` entries per landed phase, as usual.
- [ ] The D5 residue matrix (i386/rv64/a64 transfers) finalised in §7.

**Definition of done:** CI fails if this document and the tree disagree
about what has landed.

**Test gate:** the claim checker itself, green in CI, plus one planted
violation (mark O1 complete with no patch present) caught in a local run —
the negative control convention.

**Deliverable:** `patches/OPT_O9_ci.patch`

---

## 5. What this plan deliberately does not do

Named so nobody mistakes absence for oversight:

- **Scheduler priorities, deadlines, tickless idle.** Semantic changes to
  scheduling are feature work; the round-robin contract is load-bearing
  for every timing-sensitive test in the tree. If O4+O7 leave the GUI
  latency story unsatisfying, *that measurement* is the opening argument
  of a scheduler plan, not a footnote here.
- **Demand paging / page cache unification.** `MATURITY_PLAN.md` M4/M8
  territory; touching it from a perf plan would fork ownership.
- **virtio-net IRQ RX, network throughput tuning.** M7 and M6's ledger.
- **GPU acceleration.** virtio-gpu/virgl exists in-tree and is its own
  world; this plan only makes the *software* path honest.
- **User-space SIMD** (libgl rasteriser vectorisation, SSE memcpy beyond
  `rep movsb`). Real wins, but they belong to a userspace/SDK plan — the
  kernel plan stops at the kernel/libc seam.
- **Real-hardware-only wins** (PAT/WC framebuffer, PCID, ERMSB tuning):
  specified where they arose (O4, O5), deferred until they can be measured
  where they matter (D1). Pretending TCG numbers validate them would be
  theatre.

## 6. The numbers (filled per landed phase — D1)

Baseline column measured by O0's `test_perf_smoke` on this machine
(QEMU/TCG, BIOS boot unless marked UEFI; log:
`build/integration-logs/perf_smoke.log`).

| Metric | Baseline (`6bba33f` + O0) | After O1 | O2 | O3 | O4 | O5 | O6 | O7 | O8 |
|---|---|---|---|---|---|---|---|---|---|
| boot → shell (PIT ticks, BIOS) | 505 (~5.1 s) | 497 | full 499 / **fast 400** | | | | | | |
| boot → shell (PIT ticks, UEFI/OVMF) | 1345 (~13.6 s) | — | | | | | | | |
| membench memcpy-a 64 KiB (MB/s) | 11 | **82** | | | | | | | |
| membench memcpy-a 1 MiB (MB/s) | 10 | **79** | | | | | | | |
| membench memset-a 1 MiB (MB/s) | 342 | **1687** | | | | | | | |
| membench memmove-o 64 KiB (MB/s) | 144 | **1236** | | | | | | | |
| uart_tx_sync_bytes at prompt | 24 778 | 24 774 | ~24 800 | **748** (+ ring 24 106) | | | | | | |
| kmalloc_walk_steps per boot | 665 394 | 665 394 | | | | | | | |
| compositor px composited / flipped (UEFI, ~60 s up) | 64 512 000 / 4 546 560 (14.2×) | — | | | clock frame: **1 024 000 → 94 805 px (10.8×)**; idle loadavg 42.3 → 34.4 | | | | |
| tlb_shootdowns_full per boot (`-smp 2`) | 8 | 8 | | | | | | | |
| idle ticks during 2 s `wait` | *O7 fills* | | | | | | | | |
| kernel.elf bytes | 2 437 296 | 2 443 920 | | | | | | | |
| auralite.iso bytes | 51 380 224 | 51 380 224 | | | | | | | |

## 7. Cross-arch residue (D5)

| Item | i386 | rv64 | a64 |
|---|---|---|---|
| O1 string ops | `rep movsb` transfers verbatim | 8-byte loops needed | 8-byte loops needed (or `dc zva` for memset) |
| O2 fast-boot knob | transfers (same cmdline path) | different self-test set | different self-test set |
| O3 UART ring | same 16550 — transfers | same 16550 model via MMIO — transfers with the access shim | PL011 — same shape, different registers |
| O5 shootdown | single-CPU today — n/a | SBI `rfence` already ranged — ahead of x86 here | TLBI by VA exists — ahead of x86 here |
| O7 blocking | smallsh has no wait4 — n/a | n/a | n/a |

The rv64/a64 lines on O5 are worth reading twice: the RISC-V and ARM ISAs
hand the kernel ranged remote invalidation as a primitive. The x86_64 tree
is the one that has to build it — which is exactly why it is a phase.
