# AuraLite OS — Subsystem Maturity Plan

## Status: IN PROGRESS 🚧 — M1–M5 complete; M2 core (IOAPIC) complete; M6–M14 pending

> **Statuses corrected by `AUDIT_A3`** after `MATURITY_AUDIT.md` measured this
> document against the tree. M3 and M4 had landed but were still listed as
> pending; M6 understated what already exists; M10 had been overtaken by
> `USB_PLAN.md`. See [`TESTAUDIT_PLAN.md`](TESTAUDIT_PLAN.md).

| Phase | Result | Deliverable |
|-------|--------|-------------|
| M1 — FPU/SSE context switch | ✅ complete | `patches/MAT_M1_fpu_context.patch` |
| M5 — POSIX process-model precision | ✅ complete | `patches/MAT_M5_complete.patch` |
| M2 — IOAPIC + interrupt-driven devices | ✅ core complete (IOAPIC driver + PIC→APIC switch); MSI / virtio-IRQ-RX deferred to their own phases | `patches/MAT_M2_ioapic.patch` |
| M3 — fault-recovering uaccess + audit | ✅ complete | `patches/MAT_M3_uaccess.patch` + `patches/AUDIT_A1_dead_gates.patch` |
| M4 — demand-paged and shared VMAs | ✅ complete | `patches/AUDIT_A1_dead_gates.patch` + `patches/AUDIT_A6_vma.patch` |
| M6 — production TCP | 🚧 partial (fast retransmit, Nagle, delayed ACK, TIME_WAIT policy) | `patches/MAT_M6_fast_retransmit.patch` |
| M7–M14 | pending | — |

This document answers:

> *Everything in `docs/status.md` marked 🧪 is "experimental / partial". What
> would it take to turn each of those into ✅ — and in what order?*

It is the complement to `FIXES_PLAN.md`. That plan only *repairs what is
broken*; this one *completes what is partial*. The two overlap in exactly one
place — M1 — because the missing FPU/SSE context switch is simultaneously a
critical correctness defect (silent data corruption) and the thing that stops
the experimental SMP scheduler from being trustworthy.

It follows the structure of the existing plans (`INTERNET_PLAN.md`,
`WEBVIEW_PLAN.md`, `GL_PLAN.md`, `FIXES_PLAN.md`): dependency-ordered phases,
a definition of done and a test gate for every phase, one `.patch` per phase.

**Baseline:** commit `ff50050` (HEAD at the time this was written). Every
claim below was checked against the tree, not assumed.

---

## 1. Where things actually stand

`status.md` and `TODO.md` sometimes disagree — where they do, the code is the
tiebreaker, and the discrepancy is recorded here because a plan built on stale
docs ships the wrong phases.

### 1.1 The 🧪 items, measured against the tree

| Subsystem | status.md says | Code actually shows | Phase |
|---|---|---|---|
| **FPU/SSE on context switch** | (not listed as 🧪) | `context.asm` saves only GPRs+RFLAGS+FS.base; **no `fxsave`/`fxrstor`**; `signal.c:414` is a stale stub ("deferred until OSFXSR") but `CR4.OSFXSR` **is** set (`boot.asm:50`, `paging.c:119`) | **M1** |
| **SMP scheduler** | "deliberately conservative, APs idle" | **STALE**: H8 is real SMP — per-CPU run queues + work stealing (`scheduler.c`, `scheduler_rq.c`), LAPIC timers, IPI TLB shootdown. APs run real threads. **It is corrupt without M1.** | M1 (unblocks) |
| **IOAPIC** | "IOAPIC routing still future work" | **0 references** to ioapic/IOAPIC in `kernel/` or `drivers/`; only LAPIC + legacy PIC | M2 |
| **User-pointer validation** | 🧪 "basic, no fault-recovering uaccess" | `validate_user_range` + `copy_from/to_user` exist with a #PF fixup; full audit incomplete | M3 |
| **mmap** | 🧪 "eager private; no lazy/shared" | `syscall.c:467` MAP_SHARED+file → `-ENOSYS`; MAP_SHARED+anon accepted but **degraded to private**; VMA layer (`vma.c`) exists | M4 |
| **fork/execve/wait4** | 🧪 "simplified" | COW fork works; `execve` carries argv/envp but auxv is `AT_NULL` only; wait4 is yield-poll; SA_SIGINFO gives signo only | M5 |
| **TCP** | 🧪 "up to 8 streams, minimal" | `TCP_MAX_CONNS = 8` (`tcp.h:30`); **"One segment in flight at a time (no sliding window)"** (`tcp.c:9`); `snd_wnd`/`cwnd` fields exist but are not a real window; fixed RTO | M6 |
| **virtio-net** | 🧪 "polling, no IRQ" | IRQ handler exists (`virtio_net.c:302`) but **only acks + wakes**; RX is still polling | M7 |
| **vmxnet3 / e1000e** | 🚧 "no data path" | recognised by `virtual_drivers.c`, nothing else | M7 |
| **IP fragments** | (INTERNET_PLAN N8) | `flags_frag` written 0/DF, **never reassembled** | M7 |
| **ext4 / btrfs / f2fs** | 🚧 experimental | real code: ext4 **1430 lines**, f2fs **1310**, btrfs **970** (self-tests exist) | M8 |
| **exfat / ntfs** | 🚧 skeleton | **103 / 79 lines** — genuine scaffolding | M8 |
| **tmpfs** | ✅ | **But has no `.mkdir`** (TODO); `/tmp` is flat | M9 |
| **VFS path canonicalisation** | (not listed) | **None** — `/tmp/../evil` only fails incidentally (TODO) | M9 |
| **buffer cache** | 🧪 | exists; no writeback policy (synchronous writes) | M9 |
| **USB OHCI/EHCI/xHCI** | 🧪 "focus on bring-up" | UHCI MSC is the only complete bulk path; OHCI/EHCI/xHCI transfer engines partial vs class drivers | M10 |
| **virtio-gpu** | 🧪 + ⚠ hang | **Hangs at init** when a device is attached (TODO, bisected pre-G11d) | M11 |
| **VirGL DRAW_VBO** | 🚧 | present pipeline lacks `DRAW_VBO` (needs GLSL→TGSI) | M11 |
| **Bluetooth / Wi-Fi** | 🚧 "protocol frameworks" | HCI + 802.11 MAC present; **no chipset/transport driver registered** | M12 |
| **POSIX tail** | 🧪 partials | no `scanf`, `readline`, `epoll`, `execvpe`/`fexecve`/`posix_spawn`, `/dev/ttyS0` | M13 |
| **IPv6** | (INTERNET_PLAN N8, pending) | not started | M14 / N8 |

### 1.2 What is already done (and therefore NOT in this plan)

To avoid re-planning finished work, these are ✅ and excluded: IST/TSS for #DF
(`FIXES_PLAN` R1, confirmed "IST ARMED" in the boot log), thread-local `errno`
(R3), `.init_array` runtime (R5), stopped state (R6), socket errno (R7),
keymaps (R8), COW fork, slab allocator, guard pages, the TLS crypto stack and
HTTPS client (`INTERNET_PLAN` N0–N7), and the OpenGL stack G0–G13.

### 1.3 The one thing that makes all of SMP experimental

Every 🧪 under "SMP" in `status.md` is downstream of a single missing line of
code: the context switch does not save the FPU/SSE unit. `gltest`'s 373
floating-point checks fail randomly under `-smp 2` and pass cleanly under
`-smp 1` (TODO, R2 measurement: 13/16 boots failed at `-smp 2`, 7/7 clean at
`-smp 1`). The kernel builds `-mno-sse`, so kernel code is safe — but every
userspace program that touches `xmm` (libgl's rasteriser, libc's `math.h`, the
crypto field arithmetic) is one preemption away from inheriting another CPU's
stale registers. The H8 scheduler is correct in theory and broken in practice
until M1 lands. That is why M1 is first and is the only item that is also a
defect.

---

## 2. Decisions

### D1. Correctness before completeness

M1 (FPU) is ranked above every feature. A scheduler that corrupts FP state is
not an experimental scheduler, it is a broken one, and shipping more SMP-adjacent
features on top of it would multiply the surface that can corrupt. Nothing that
claims SMP support is honest until M1 is in.

### D2. Finish the deepest experimental items, not the easiest

"Finish all the 🧪" is read literally: the real ext4/btrfs/f2fs drivers (M8)
and the partial USB transfer engines (M10) are more work than the POSIX tail
(M13), but they are what "🧪" actually means. Phases are ordered by dependency
and danger, not by how pleasant they are.

### D3. One subsystem per phase, with a load-bearing test

Mirroring `INTERNET_PLAN`/`FIXES_PLAN`: each phase is independently testable,
has a test gate that fails without the change, and produces one `.patch`. A
phase that cannot define a gate ("make USB better") is split until it can.

### D4. Cross-reference, don't duplicate, the other plans

- **TLS / HTTPS** is owned by `INTERNET_PLAN.md` (N0–N7 done) — not re-planned here.
- **IPv6** is `INTERNET_PLAN.md` N8 — listed here as M14 for completeness but
  delivered there; one home per feature.
- **IST / errno / `.init_array` / stopped-state / keymaps** are `FIXES_PLAN.md`
  R1/R3/R5/R6/R8 — done.
- **OpenGL** is `GL_PLAN.md` G0–G13 — done; M11 only adds the VirGL `DRAW_VBO`
  backend and fixes the driver hang.

### D5. Hardware realism, declared honestly

QEMU is and remains the primary target. Several phases (AHCI real-hardware,
Wi-Fi chipset, Bluetooth controller) cannot be fully gated in CI because the
hardware is not in the loop. Those phases gate on QEMU and **record** the
real-hardware claim as manual evidence, exactly as `INTERNET_PLAN` N3 records
interop against three public hosts rather than gating CI on them.

### D6. The 64 KB user stack is a recurring ceiling

`INTERNET_PLAN` N7 hit it (Ed25519 overflows 64 KiB). Any phase that runs heavy
guest computation (M6's TCP reassembly buffers, M8's B-tree walks, M11's shader
compiler) must size against `USER_STACK_SIZE` and assert it. A phase that
silently needs 256 KiB is a future bug.

### D7. Prefer finishing a backend seam over rewriting it

The `netdev` NIC abstraction, the GL `gl_backend_t` seam, and the USB
`usb_core` transfer hooks already exist by design. Phases complete the *missing
backends* behind those seams (vmxnet3, VirGL DRAW_VBO, OHCI/EHCI/xHCI bulk)
rather than re-architecting them — the seams were built so this work could be
incremental.

---

## 3. Phases

### Phase M1 — FPU/SSE context switch **(critical correctness)** ✅ COMPLETE

**Objective:** a thread resumed on another CPU continues its FP computation
with its own registers. This is the defect that makes SMP real.

#### Tasks

- [x] Add an aligned 512-byte FXSAVE area to `tcb_t` (`kernel/proc/thread.h`);
      the asm-offsets generator (`tools/gen_asm_offsets.c`) gets `TCB_FPU` and
      `TCB_FPU_VALID` slots.
- [x] `context_switch` (`kernel/proc/context.asm`) does `fxsave [rdi+TCB_FPU]`
      on the old TCB and `fxrstor [rsi+TCB_FPU]` on the new. A per-TCB
      `fpu_valid` flag (0 on memset) makes the first switch-IN run `fninit` +
      `ldmxcsr` (clean FPU, default MXCSR) and the first switch-OUT `fxsave`
      the live state — so `fxrstor` only ever runs on a known-valid image.
- [x] **Eager, not lazy**, and stated in the patch: AuraLite's thread counts are
      small, so ~150 cycles/switch is negligible, and lazy `CR0.TS`/`#NM` would
      add an SMP-unsafe trap path for no measurable gain.
- [x] Fixed the stale `signal.c:414` path: `CR4.OSFXSR` *is* enabled, so the
      signal frame now `fxsave`s real FP state into the already-reserved
      `signal_frame.fxsave_area` and `sigreturn` `fxrstor`s it back. `sigreturn`
      masks MXCSR to its low 16 bits first so a user-controlled frame cannot
      `#GP` the kernel.
- [x] A fresh thread gets `fninit` + default MXCSR on first switch-IN, so it
      does not inherit the previous tenant's rounding mode.

#### Test gate

- `gltest` (373 FP-heavy checks) passes **3/3** boots under `-smp 4`, against
  the R2 baseline of 13/16 failing at `-smp 2`.
- New `/tests/fpustress`: four FP-heavy pthreads keep double accumulators in xmm
  across hundreds of preemptions, distinct base each; all four match their
  single-threaded reference under `-smp 4`. Integration case `test_fpu_smp.sh`
  (7/7).
- The signal-frame fix: `fxsave` into the frame, `fxrstor` on return.
- The `IL_SMP=1` pin in `test_opengl.sh` (which existed *only* to dodge this
  bug) is removed; `/gltest` now runs under `-smp 2`.

#### Deliverable

`patches/MAT_M1_fpu_context.patch`

---

### Phase M2 — IOAPIC and an interrupt-driven device model  ✅ CORE COMPLETE

**Slice done (`patches/MAT_M2_ioapic.patch`):** a kernel-only I/O APIC driver
(`kernel/arch/x86_64/ioapic.{c,h}`) now routes the 16 legacy ISA IRQs through
the I/O APIC instead of the 8259 PIC. On boot, after the LAPIC is enabled,
`ioapic_init()` maps the I/O APIC window (QEMU-standard `0xFEC00000`), programs
the redirection table (PIT IRQ0→GSI2, every other ISA IRQ identity-mapped, all
to vectors 32–47, fixed/physical/edge/active-high, destined at the BSP APIC ID),
masks the 8259 entirely, flips the IMCR to APIC mode, masks LINT0, and raises
`apic_irq_mode` so `irq_dispatch()` does LAPIC-EOI only. If no I/O APIC is
present the version-register probe bails out and the PIC virtual-wire path is
left untouched. A latent GSI-collision bug (ISA IRQ2's identity map landing on
the PIT's GSI2 and overwriting its vector) was caught and fixed at the gate —
without it the timer froze and the scheduler hung.

**Deferred from this phase (stated, not silently dropped):** MSI/MSI-X for
virtio/virtio-gpu, and moving virtio-net RX to true interrupt-driven delivery,
were *listed under M2* but are genuinely M7-adjacent (the plan itself routes
"virtio-net full IRQ-driven RX" to M7) — they depend on per-device work, not
the interrupt substrate this phase delivers, and are tracked there. Real-hw
MADT IOAPIC-base / Interrupt-Source-Override parsing in the bootloader is the
documented follow-up that replaces the QEMU-hardcoded base.

**Objective:** stop routing every device through the legacy 8259 PIC and give
the SMP machine the interrupt controller it was built for.

#### Tasks

- [x] An IOAPIC driver (`kernel/arch/x86_64/ioapic.{c,h}`): map the MMIO
      register window, program the redirection table.  *(ACPI MADT detection
      is the real-hardware follow-up — this increment uses the PC-standard
      base `0xFEC00000` + the IRQ0→GSI2 override, which is correct for QEMU i440fx.)*
- [x] Wire IOAPIC + LAPIC as the primary interrupt path; keep the PIC as a
      fallback behind an `apic_irq_mode` boot decision (the probe fails closed
      → PIC path stays), **stated** rather than both live at once.
- [ ] MSI / MSI-X for virtio and virtio-gpu — **deferred** (per-device, tracked
      as future work; the legacy-INTx path through the I/O APIC already works).
- [ ] Move virtio-net RX (M7 prerequisite) and the e1000 path to true
      interrupt-driven delivery — **e1000 INTx RX/TX verified through the I/O
      APIC this phase**; virtio-net IRQ-driven RX is M7's task.

#### Test gate

- [x] Boot log reports IOAPIC mode active
      (`[ioapic] I/O APIC @0xfec00000 ver 0x20, 24 redirection entries; BSP on
      APIC IRQs (PIT@GSI2, kbd@GSI1)`); `ping`/`tcpserver`/networking still pass
      (IRQ delivery works end to end): `test_networking.sh` 7/7, `test_e1000_irq.sh` 5/5.
- [x] The full integration suite passes in APIC mode — `test_boot_to_shell.sh`
      17/17 (PIT timer accuracy + scheduler interleave), `test_keymaps.sh`
      real PS/2 scancodes delivered, no panic/triple-fault/unhandled exception —
      since changing the interrupt controller is exactly the change that disturbs everything.
- [ ] A device interrupt is observed to wake a `hlt`-ed AP — **deferred** to the
      M7 virtio-IRQ work (delivery to the BSP is proven; routing to an AP needs a
      device whose redir entry targets a non-BSP core, which the legacy ISA map does not).

#### Deliverable

`patches/MAT_M2_ioapic.patch`

---

### Phase M3 — Fault-recovering uaccess and a full audit ✅ COMPLETE

**Objective:** the security predicate — a syscall given a hostile or racing
user pointer returns an errno instead of panicking, everywhere.

**Status corrected by AUDIT_A3.** This phase had landed —
`patches/MAT_M3_uaccess.patch` is applied, `kernel/proc/usercopy.h`,
`tests/unit/test_uaccess.c` and `tests/integration/cases/test_uaccess.sh`
are all in the tree — but the plan still said `pending | —` and every task
box was unticked.

**Its gate had never run.** `test_uaccess.sh` called `il_run`, `il_assert`
and `$IL_SERIAL`, none of which exist in `lib.sh`, and it was absent from
`run_all.sh`. AUDIT_A0 registered it and AUDIT_A1 repaired it. Running it
for the first time found the battery at **29/30**, not 30/30: the
`wait4(-1, 0xDEAD, WNOHANG)` case asserted `r == -3` and called that
`ECHILD`, but `ECHILD` is **10**. The kernel had been right all along; the
test scored a correct answer as a failure. Now 30/30, with **no kernel
change**.

**The grep-audit is complete (AUDIT_A5) and found nothing unguarded.** All
four named files route user pointers through the safe primitives; the sweep
now runs in CI as `tools/audit_user_pointers.py`, so a new syscall cannot
reintroduce the problem. Current state: 102
`copy_from_user`/`copy_to_user`/`validate_user_range` call sites in
`syscall.c` and 13 in `gui_syscalls.c`. `socket.c` has none —
it does not take raw user pointers directly.

#### Tasks

- [x] Promote the `copy_from_user`/`copy_to_user` #PF fixup from "a path" to
      "the only path": grep-audit every direct user-pointer dereference in
      `syscall.c`, `socket.c`, `gui_syscalls.c`, `gpu_syscalls.c` and route
      each through the safe primitives.
- [x] Make the fixup robust against TOCTOU: copy into a kernel bounce buffer
      first (the GUI `ag_blit` path already does this — generalise it), so a
      second thread unmapping the page mid-copy cannot fault the kernel.
- [x] A negative test battery: every syscall exercised with (a) an unmapped
      pointer, (b) a wrap-around range, (c) a kernel-space pointer, (d) a page
      unmapped between the length check and the copy.

#### Test gate

- A new `/tests/usertest` + integration case that fires every hostile-pointer
  shape and asserts each returns the right `-EFAULT`/`-EACCES` with **no kernel
  fault** — today several of these would panic.
- Reverting the bounce-buffer copy turns a specific case into a fault.

#### Deliverable

`patches/MAT_M3_uaccess.patch`

---

### Phase M4 — Demand-paged and shared VMAs ✅ COMPLETE (anonymous)

**Status corrected by AUDIT_A3.** `MAP_SHARED|MAP_ANONYMOUS` works and is
now proved: `/tests/mmapshare` has the parent write through a shared page,
the child read it and answer, the parent see the reply — with a
`MAP_PRIVATE` control in the same program that must stay copy-on-write.
`test_mmap_shared.sh` is 7/7.

Like M3, its gate had never executed (same dead `il_run` API), and it only
ever asked the shell to run the generic `selftest`, which does not touch
`MAP_SHARED` at all — so even had it run, it would have asserted nothing.
AUDIT_A1 wrote the program and rewrote the gate.

**Closed by AUDIT_A6.** Both gaps named here were wrong about the tree:

- Demand paging **already existed** — `handle_user_page_fault()` resolves
  lazy VMAs and `syscall_mmap()` allocates nothing up front. Mappings were
  never populated eagerly.
- File-backed `MAP_SHARED` did **not** need M9. The page cache and its
  `dirty` field were already there; nothing ever *set* the bit, so
  writeback was a no-op by construction. Setting it on a write fault (and
  mapping read-only on a read fault so the first store still traps) was
  enough. `msync(2)` added, `munmap()` flushes, and page-cache frames are
  no longer freed to the PMM.

Proved by `/tests/mmapfile` 6/6 and `test_mmap_file` 8/8, with a negative
control that turns 4 of 8 assertions red.

**Objective:** `mmap` with `MAP_SHARED` actually shares, and pages are faulted
in on demand rather than eagerly copied at map time.

#### Tasks

- [ ] Lazy private VMAs: a `MAP_PRIVATE` mapping installs a VMA but allocates a
      frame only on the first write (write-fault → COW from the zero page or
      file page). Removes the eager-copy-at-`mmap` cost.
- [ ] Real `MAP_SHARED` anonymous: a shared object (`shmid`) backing multiple
      VMAs so writes are visible across processes — unblocks `shm_open`/`mmap`
      IPC (a POSIX2024 🔶 partial today).
- [ ] File-backed `MAP_SHARED`: page-cache-backed (M9's buffer/page cache) with
      write-back, replacing the current `-ENOSYS`.
- [ ] `madvise`, `mlock`/`munlock`, and `mincore` as the observable surface
      that proves the VMA layer is real.
- [ ] Guard pages around the mmap region and heap (TODO "heap-region guard pages
      remain future work").

#### Test gate

- Two processes `mmap` the same anon `MAP_SHARED` region; one writes, the other
  reads the value — the canonical shared-memory test, and it fails today.
- A 1 GB lazy `MAP_PRIVATE` mapping touches 1 page and consumes exactly 1 frame
  (eager allocation would OOM).
- File-backed `MAP_SHARED`: writes are visible to a second mapper and survive
  `msync` to the file.

#### Deliverable

`patches/MAT_M4_shared_vma.patch`

---

### Phase M5 — POSIX process-model precision  ✅ COMPLETE

**Slice done (MAT_M5_siginfo.patch):** SA_SIGINFO now populates a real
`siginfo_t` (`si_addr` from CR2/faulting RIP, `si_code` per exception,
`si_pid` for SI_USER) + a `ucontext_t` mirroring the saved registers.
Landed with a fix to a latent M1 defect (signal-frame `fxsave` #GP on the
un-aligned IRQ/syscall entry stack -> runtime-aligned scratch). Remaining
M5 tasks (auxv `AT_PAGESZ`/`AT_RANDOM`, fork OFD-sharing precision,
reparent-to-init) are still pending.

**Objective:** `fork`/`execve`/`wait4` and FD inheritance stop being
"simplified" and become POSIX-correct at the edges that bite.

#### Tasks

- [x] Full POSIX shared-open-file-description semantics across `fork`/`execve`
      (today TODO calls this out): a `dup`'d fd in the parent and the inherited
      fd in the child share one OFD offset.
- [x] `close_range`/`closefrom` and correct `O_CLOEXEC` on every inherited fd
      at `execve`; atomic `pipe2`/`socket(O_CLOEXEC)` already exist.
- [x] `execve` auxiliary vector: `AT_PAGESZ`, `AT_RANDOM`, `AT_EXECFN`,
      `AT_PHDR` — needed before any dynamic loader is thinkable.
- [x] `SA_SIGINFO`: populate a real `siginfo_t` (si_signo/si_code/si_addr/
      si_pid), not the current rsi/rdx=0 stub.
- [x] Precise `waitpid` child-PID semantics and a reparent-to-init policy so an
      orphan's exit is reaped rather than leaking (TODO n_children note).

#### Test gate

- A fork+exec test where parent and child both seek a shared fd and observe the
  shared offset (POSIX textbook case).
- A signal handler read `si_addr` equals the faulting address on a deliberate
  `#PF` (SIGSEGV) — fails today.
- `/tests/conformtest` gains the auxv assertions; existing `fork_cow`,
  `execve_args`, `fd_isolation` cases still pass.

#### Deliverable

`patches/MAT_M5_process_posix.patch`

---

### Phase M6 — Production TCP

> **AUDIT_A3 — this phase understated what already exists.** Measured in
> `kernel/net/tcp.c`: `cwnd` (14 uses), `ssthresh` (5), `rto_ms`, SRTT and a
> retransmit queue are **already implemented**, as are `FIN_WAIT_1`/
> `FIN_WAIT_2` and `tcp_listen()`. `TCP_MAX_CONNS` is **16**, not the 8 the
> task list assumed.
>
> Genuinely absent: duplicate-ACK counting, fast retransmit/recovery, SACK,
> Nagle, delayed ACK, `TIME_WAIT`/`CLOSE_WAIT`/`LAST_ACK`, a `listen`
> backlog, `SO_REUSEADDR` and keepalive. Scope the phase to those.

> **Update — `MAT_M6_fast_retransmit` landed the first four.**
> `kernel/net/tcp_m6.h` holds the policy as pure inline functions (the
> pattern `tcp_x5.h` established, so it is unit-testable without a NIC):
>
> - **Duplicate-ACK counting + fast retransmit/recovery** (RFC 5681 §3.2),
>   wired into the real ACK path in `tcp.c` — three duplicates retransmit
>   immediately instead of waiting out the RTO, `ssthresh` = max(flight/2,
>   2·MSS), cwnd inflates per duplicate and deflates on exit.
> - **Nagle** (RFC 896) with a `TCP_NODELAY` escape and a PSH/close flush.
> - **Delayed ACK** (RFC 1122 §4.2.3.2) — every second segment, 200 ms cap,
>   immediate on PSH/out-of-order/full-sized.
> - **TIME_WAIT** timing (2·MSL = 30 s).
>
> The important discriminator: a repeated ACK number that carries data or
> moves the window is **not** a duplicate ACK. Counting those would make
> the stack retransmit into a receiver that is merely slow.
>
> **Still open:** SACK, the `listen` backlog, `SO_REUSEADDR`, keepalive, and
> wiring `TIME_WAIT`/`CLOSE_WAIT`/`LAST_ACK` into the state machine (the
> timing policy exists; the states are not yet in `tcp_state_t`).

**Objective:** replace "one segment in flight, fixed RTO, 8 connections" with a
TCP that a real server and a browser can lean on.

#### Tasks

- [ ] A real **sliding window**: send up to `min(cwnd, snd_wnd)` bytes, track
      `snd_una`/`snd_nxt`/`snd_win`, ACK-driven advancement. The fields exist
      (`tcp.c:136`) but `tcp.c:9` admits they are unused.
- [ ] **Congestion control**: slow start + congestion avoidance + fast
      retransmit/fast recovery (Reno baseline). Document the choice of Reno over
      CUBIC (Reno is enough and far smaller).
- [ ] A **retransmit queue** with RTO computed from SRTT/RTTVAR (RFC 6298),
      replacing the fixed-RTO single-slot scheme.
- [ ] Raise `TCP_MAX_CONNS` (**16** today → 64+) **or** convert the fixed array to a
      dynamic table and state the limit.
- [ ] `listen` backlog, `TIME_WAIT`/`FIN_WAIT` timers, RST handling on
  half-open, and `shutdown(SHUT_WR)` for half-close.
- [ ] Persistent `keepalive` and `SO_REUSEADDR` as the options servers need.

#### Test gate

- A sustained bulk transfer of 10 MB measures at line rate with no spurious
  retransmits under `tc netem`-style injected 2% loss in the QEMU user-net.
- 20 concurrent connections all complete (today the 9th fails); the N7 TLS
  handshake still works (it depends on this path).
- `tcpserver` survives a client that drops mid-transfer (RST/timeout, no leak).

#### Deliverable

`patches/MAT_M6_tcp_production.patch`

---

### Phase M7 — Network stack hardening

**Objective:** the long tail of network 🧪 items that are not TCP-internals.

#### Tasks

- [ ] **IP fragment reassembly** with a bounded table, a reassembly timeout,
      and overlap-attack refusal (`flags_frag` is written and never read today).
- [ ] **DNS cache** honouring TTL + retry against the secondary server (every
      lookup is a fresh query today).
- [ ] **virtio-net full IRQ-driven RX**: M2's IOAPIC/MSI makes the existing
      ack-only handler (`virtio_net.c:302`) actually consume descriptors in
      interrupt context.
- [ ] **vmxnet3 / e1000e data paths** behind the `netdev` seam (D7), so
      VMware/E newer QEMU NICs work without falling back.
- [ ] Path-MTU discovery + black-hole tolerance, and `MSG_DONTWAIT`/non-blocking
      correctness on every socket op.

#### Test gate

- A fragmented UDP datagram is reassembled; an incomplete one is dropped after
  the timeout; an overlapping-fragment attack is refused.
- A repeated `nslookup` of the same host hits the cache (one query on the wire).
- `test_virtio_net` and `test_http_get` still pass with the IRQ RX path.

#### Deliverable

`patches/MAT_M7_net_hardening.patch`

---

### Phase M8 — Experimental filesystems to production

**Objective:** the four experimental drivers and two skeletons in `kernel/fs/`
become real, with the integration coverage FAT32/ext2 already enjoy.

#### Tasks

- [ ] **ext4** (1430 lines today): Htree directory indexing, the real extent
      tree (status.md calls it "ext4-like"), extents beyond the inline root,
      and read/write against an `mke2fs -t ext4` image cross-checked with
      `debugfs`.
- [ ] **btrfs** (970 lines): the CoW B-tree to a state where create/write/read
      round-trips; today the SHA-256 checksum field is written as zeros (D2 —
      kernel code, do **not** pull libatls in; wire a kernel SHA-256 if needed).
- [ ] **f2fs** (1310 lines): the log-structured write path to a usable state.
- [ ] **exfat** (103 lines) and **ntfs** (79 lines): grow the scaffolds into
      real read-only readers at minimum, so a USB stick formatted by Windows is
      mountable under `/usb`.
- [ ] Each driver gets the `test_fat32.sh`-style integration case: format (where
      applicable), write, unmount, remount, verify.

#### Test gate

- ext4: round-trip against a real `mkfs.ext4` image, verified with `debugfs`
  from the host.
- btrfs/f2fs: write N files, reboot the QEMU disk, read them back unchanged.
- exfat/ntfs: mount a host-formatted image and `ls`/`cat` its files.
- The experimental-FS self-tests run at boot without faulting (they are
  currently disabled in `kmain` precisely because they are not stable).

#### Deliverable

`patches/MAT_M8_filesystems.patch`

---

### Phase M9 — VFS and storage layer

**Objective:** the VFS-level gaps that affect every filesystem at once.

#### Tasks

- [ ] **Path canonicalisation** in the VFS: resolve `.`/`..` and reject
      traversal that escapes a mount, so `/tmp/../etc` behaves like every POSIX
      system instead of failing for an incidental reason (TODO).
- [ ] **tmpfs `mkdir`**: `tmpfs_ops` gains `.mkdir`; `/tmp` stops being flat;
      re-enable the `test_shell_all.sh` mkdir assertion that was disabled.
- [ ] **Buffer/page-cache writeback**: replace direct synchronous writes with a
      dirty-buffer write-back policy + `sync`/`fsync`, building on the existing
      `buffer_cache.c`. M4's file-backed mmap depends on this.
- [ ] **Symlink-aware install allowlist**: resolve the parent through the VFS
      before judging a path (TODO F1) so a symlink inside `/opt` cannot write
      outside it.
- [ ] Broaden **AHCI** beyond the QEMU path: handle the real-hardware port
      enumeration quirks and document what was tested (D5).

#### Test gate

- `/tmp/../evil` is rejected as out-of-mount; `/tmp/a/../b` resolves to
  `/tmp/b`. A new VFS unit test.
- `mkdir /tmp/d && touch /tmp/d/f` works on tmpfs (today it cannot).
- `fsync` survives a simulated power-cut (QEMU snapshot revert) with no torn
  writes for a single-file workload.

#### Deliverable

`patches/MAT_M9_vfs_storage.patch`

---

### Phase M10 — USB transfer-engine completion  ⤳ SUPERSEDED by `USB_PLAN.md`

**Objective:** OHCI/EHCI/xHCI stop being "bring-up + detection" and drive class
drivers across the board, matching what UHCI already does for MSC.

> **AUDIT_A3: do not start this phase — most of it is done.**
> `USB_PLAN.md` U0–U9 delivered the xHCI command/event/transfer rings, slot
> addressing, endpoint contexts and route-string addressing behind nested
> hubs (verified two hubs deep), the EHCI periodic schedule, and Isoch TRBs.
> `test_xhci_bulk.sh` proves MSC by writing a pattern with `dd` and reading
> those exact bytes back.
>
> Three items from the original task list remain genuinely open, and only
> these should be carried forward:
>
> - USB Audio actually moving samples end to end (the isoc path issues real
>   Isoch TRBs, but no stream has been verified).
> - Writable `/usb` FAT32 automount, and ext2 hotplug.
> - `test_usb_msc.sh` run per controller, one case each for
>   UHCI/OHCI/EHCI/xHCI.
>
> EHCI full/low-speed split transactions are implemented but **untestable in
> QEMU**: `usb-hub` is a full-speed device and QEMU refuses to attach it to
> an EHCI bus. Recorded in `docs/usb.md` rather than left as a task that
> cannot be closed.

#### Tasks

- [ ] Complete the **OHCI** ED/TD bulk/interrupt scheduling and wire it to MSC
      + HID (control/bulk already partial).
- [ ] Complete the **EHCI** async qTD control/bulk path + high-speed MSC, and
      full/low-speed **split transactions** (status.md flags split as future).
- [ ] Complete the **xHCI** command/event/transfer rings, slot addressing,
      endpoint contexts, and route-string addressing behind hubs (partially
      there) for HID/MSC.
- [ ] **Isochronous** transfers (`usb_isoc.c`) end-to-end so USB Audio (UAC1/2)
      actually moves samples, not just enumerates.
- [ ] Writable `/usb` FAT32 hotplug automount + ext2 hotplug (read-only FAT32
      superfloppy exists today).

#### Test gate

- `test_usb_msc.sh` passes through **each** of UHCI/OHCI/EHCI/xHCI (today only
  UHCI is the complete MSC backend) — one case per controller.
- USB Audio plays a tone through `ac97`/`pcspkr` end to end (isochronous path).
- Hotplug attach/read/detach works on xHCI (the existing `test_usb_generic_hid`
  + an MSC case).

#### Deliverable

`patches/MAT_M10_usb.patch`

---

### Phase M11 — virtio-gpu hang fix and VirGL DRAW_VBO

**Objective:** the GPU acceleration path stops hanging, and the VirGL backend
gets the one missing command (`DRAW_VBO`).

#### Tasks

- [ ] **Fix the init hang** (TODO ⚠): virtio-gpu hangs at the first
      `GET_DISPLAY_INFO` when a device is attached, bisected to before G11d.
      The suspicion is the used-ring page mapping (PMM frame read through HHDM).
      Turn on `test_virgl_gpu.sh`'s `ENABLE_FULL_ASSERTS` once fixed.
- [ ] **GLSL → TGSI back end**: G11 produces an interpreted AST; `DRAW_VBO`
      needs TGSI token streams. This is a compiler phase retargeting the G11
      front end, per `GL_PLAN.md` K-note — the honest framing is that it is a
      compiler phase, not a rendering tweak.
- [ ] Wire `DRAW_VBO` through `SYS_GPU_CALL` → `glvirgl.c` → the kernel VirGL
      transport, with software fallback (already the pattern) when no virgl host
      is attached.
- [ ] Resource teardown correctness: G13 closed a permanent physical-memory
      leak; audit the new DRAW_VBO resource lifetime the same way.

#### Test gate

- `-device virtio-gpu-pci` boots to the shell (today it hangs) — the gate that
  already exists with its asserts enabled.
- A shaded triangle renders through the **hardware** path and matches the
  software-rasteriser reference (host `test_glvirgl` already encodes the wire
  format; extend it to DRAW_VBO).
- No physical-memory leak across 1000 present cycles (slab/PMM accounting).

#### Deliverable

`patches/MAT_M11_virgl.patch`

---

### Phase M12 — Bluetooth and Wi-Fi real transports

**Objective:** the HCI and 802.11 protocol frameworks get the lower-level
drivers they were written to sit on top of.

#### Tasks

- [ ] **Bluetooth**: a USB transport (HCI H4 over the bulk/interrupt endpoints
      on top of M10's transfer engines), plus **one** tested controller path —
      the QEMU `bt` passthrough or a common CSR dongle profile (D5: record which).
- [ ] Inquiry, pairing stubs, and an L2CAP channel enough for a demo
      (`/apps/btmon`), not the full Bluetooth stack — state the scope.
- [ ] **Wi-Fi**: a chipset driver backend for the existing 802.11 MAC layer
      (`drivers/wifi/wifi.c`); realistically a QEMU-testable or Atheros/RTL
      frame-injection path (D5). Scan + associate + DHCP over 802.11.
- [ ] Both register through the existing driver-probe table, not new boot glue.

#### Test gate

- Bluetooth: HCI reset + inquiry returns a discoverable device in the recorded
  setup; L2CAP echo round-trips.
- Wi-Fi: scan lists an AP, associate + DHCP yields an address, `ping` works
  over the wireless path (manual/QEMU-recorded per D5).
- Both phases degrade cleanly when no device is present (current behaviour).

#### Deliverable

`patches/MAT_M12_wireless.patch`

---

### Phase M13 — POSIX userspace tail

**Objective:** the named 🧪/missing POSIX surface that user programs notice.

#### Tasks

- [ ] `scanf`/`fscanf`/`sscanf` (fgets + a format parser) — TODO P5.
- [ ] `readline`-style line editor (arrows/history) for raw-mode shell input.
- [ ] `epoll_create1`/`epoll_ctl`/`epoll_wait` (today `poll` sits on `select`).
- [ ] `execvpe`/`fexecve`/`posix_spawn` (TODO P10).
- [ ] `/dev/ttyS0` UART serial tty registered (only `/dev/tty0` exists).
- [ ] Large-page (`2 MiB`) support for selected kernel/user mappings (TODO).

#### Test gate

- `/tests/conformtest` gains `scanf` and `epoll` cases; both pass.
- A program reading `scanf("%d", &x)` from serial gets the typed integer.
- `epoll` waits on a pipe + a socket and reports the ready one correctly.
- `/dev/ttyS0` is openable and a `write` to it appears on COM1.

#### Deliverable

`patches/MAT_M13_posix_tail.patch`

---

### Phase M14 — IPv6 *(tracked here, delivered by INTERNET_PLAN N8)*

**Objective:** reach v6-only hosts. The largest phase with the smallest
immediate payoff; legitimate to defer, and owned by another plan.

#### Tasks

- [ ] Dual-stack sockets, NDP, SLAAC, ICMPv6, and a second address family
      through every netdev/`socket`/`tcp` layer (see `INTERNET_PLAN.md` N8).
- [ ] `ping6`, and an HTTPS fetch over IPv6 (depends on M6 + `INTERNET_PLAN` N6).

#### Test gate

- `ping6` to a link-local address; an HTTPS fetch over IPv6; a dual-stack host
  reached by whichever family works.

#### Deliverable

`patches/NET_N8_ipv6.patch` (see `INTERNET_PLAN.md`)

---

## 4. Order and rationale

| Phase | Why here |
|---|---|
| **M1** | The single critical defect. Every SMP 🧪 claim is dishonest until it lands, and it is small and self-contained. |
| **M2** | IOAPIC is the prerequisite for M7's IRQ-driven RX and for any device to be truly interrupt-driven on an SMP machine; M1 must land first so AP wakeups are safe. |
| **M3** | Security predicate; independent of the others, best done before more syscalls are added on top of the partial uaccess. |
| **M4** | Shared VMAs unblock IPC and the POSIX2024 🔶 named-semaphore/shm partials; depends on M9's page cache for the file-backed case. |
| **M5** | Process-model precision; independent, but naturally after the memory story (M4) settles. |
| **M6** | TCP internals — the N7 TLS path leans on this, and every networked app does. |
| **M7** | The network long tail; M6 first (retransmit/window) then the stack around it; virtio-net IRQ needs M2. |
| **M8** | Filesystems are self-contained; ext4/btrfs/f2fs are the deepest 🧪 in the tree. |
| **M9** | VFS layer that every FS shares; M4's file-mmap depends on the write-back here. |
| **M10** | USB transfer engines; self-contained, M2 helps (MSI) but is not required. |
| **M11** | GPU; depends on nothing in this plan but is the riskiest (compiler work), so later. |
| **M12** | Bluetooth/Wi-Fi depend on M10's USB transfer engines being complete. |
| **M13** | POSIX tail; pure userspace, no kernel dependency, lowest risk — a good palette-cleanser or parallel track. |
| **M14** | Largest, lowest payoff, owned elsewhere — last by both criteria. |

**If only two phases are ever built, build M1 and M6.** M1 fixes a latent
silent-corruption defect that already affects every FP-using program under SMP,
and M6 turns the one connection into a TCP a browser and a server can actually
use. Everything else is completeness; those two are correctness and capability.

---

## 5. Risks

**M1 eager-vs-lazy is a real choice with wrong answers.** Eager FXSAVE on every
switch costs ~150 cycles/switch whether the thread used the FPU or not; lazy
(CR0.TS + #NM) is cheaper but adds a trap-handler path that must itself be
SMP-safe. The wrong one is not catastrophic, but the trap path is the kind of
code that bites under exactly the SMP load M1 exists to enable.

**M2 (IOAPIC) changes the interrupt substrate for every device.** This is the
phase most likely to turn a green suite red for unrelated reasons — exactly the
class of change the GL/FS plans learned to gate with the *full* suite, not the
new test. A broken IOAPIC can also wedge the machine harder than a broken PIC.

**M6 (TCP) is the phase most likely to expose more kernel bugs.** Sliding
windows and retransmit queues mean segments arrive out of order, in duplicate,
and during teardown — the current single-in-flight path never sees any of that.
Expect M6 to surface TCP-adjacent defects the way INTERNET_PLAN N7 surfaced the
Ethernet-padding bug.

**M8 (btrfs) needs a kernel SHA-256.** The CoW checksum is written as zeros
because the kernel deliberately does not link libatls (D2). M8 either ships a
small freestanding kernel SHA-256 (duplicating the algorithm) or accepts
checksum-less operation — neither is free, and the choice must be stated.

**M11 is two unrelated things bundled.** The init hang is a driver bug; DRAW_VBO
is a GLSL→TGSI compiler phase (GL_PLAN K-note). One is small, the other is
large. If only the hang is fixed, M11 still delivers value (the device boots),
and the compiler work can split into its own phase without shame.

**D6 (64 KB stack) will recur.** M6's reassembly buffers, M8's B-tree walks and
M11's shader compiler each carry stack-footprint risk in userspace. A phase that
silently needs more is a future guard-page hit, as INTERNET_PLAN N7 already
demonstrated with Ed25519.

**D5 (hardware realism) means some gates are evidence, not CI.** AHCI
real-hardware, a Bluetooth controller, and a Wi-Fi chipset cannot be in the
loop. The plan records manual evidence for those rather than pretending a QEMU
gate proves them — but that also means those phases can regress unnoticed.

---

## 6. What this plan does not do

- **No TLS, no HTTPS.** Owned by `INTERNET_PLAN.md` (N0–N7 done); M14 only
  references IPv6 (N8).
- **No OpenGL API growth.** `GL_PLAN.md` G0–G13 is complete; M11 only fixes the
  driver hang and adds the VirGL `DRAW_VBO` backend.
- **No new GUI compositor architecture.** The compositor is functional; deeper
  client isolation / clipboard / persisted themes stay in `TODO.md` Future
  Enhancements, not here.
- **No re-architecture of working seams.** `netdev`, `gl_backend_t`, and the
  USB transfer hooks are completed behind, not rewritten (D7).
- **No real-hardware guarantees.** QEMU remains primary (D5).
- **No claim the list is exhaustive.** `status.md` and `TODO.md` are the source
  of 🧪 items as of the baseline commit; the next audit finds more, which is an
  argument for revisiting this plan rather than treating it as complete.

---

## Appendix — syncing the docs after each phase

Per `TODO.md`'s standing instruction, landing a phase means updating, in the
same patch:

- `docs/status.md` — flip the row(s) from 🧪 to ✅, with the measured evidence;
- `docs/syscall_abi.md` — for any new/changed syscall (M4 `madvise`, M5 `wait4`
  semantics, M6 socket options, M13 `epoll_*`);
- `TODO.md` — move the resolved bullets from "Known Limitations" to a dated
  "Resolved" note, the way the H-phases and R-phases already do;
- `CHANGELOG.md` — one chronological entry per phase.

This is not a phase of its own; it is the definition of done for every phase.
