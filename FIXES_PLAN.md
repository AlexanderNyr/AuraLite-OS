# AuraLite OS — Critical Defect Repair Plan

## Status: PLANNED 📋 (phases R0–R8)

This document is different from the others. `GL_PLAN.md`, `FSLAYOUT_PLAN.md`,
`SDK_PLAN.md`, `WEBVIEW_PLAN.md` and `INTERNET_PLAN.md` all *add* something.
This one only repairs what is already broken.

Everything here is a defect that exists in the tree today, found by reading
the source or by watching a test fail — not a missing feature, and not a
design preference.

It follows the same structure: dependency-ordered phases, a definition of done
and a test gate for every phase, one `.patch` per phase.

**Baseline:** commit `7b80d69`.

---

## 1. How these were ranked

A defect list is only useful if it is ordered by *danger*, not by how pleasant
each item is to fix. The ranking used here:

| Rank | Meaning |
|---|---|
| **Critical** | Silent data corruption, or a crash with no diagnostic |
| **Serious** | Wrong behaviour a user will hit, with a confusing symptom |
| **Latent** | Correct today only by luck; the guard is an accident |
| **Cosmetic** | Real, but the consequence is inconvenience |

Two things were deliberately *excluded*, because a repair plan that also adds
features is a feature plan wearing a disguise:

- **Missing features** — `epoll`, `scanf`, `readline`, `execvpe`. Absent, not
  broken.
- **Anything needing a new subsystem** — TLS, IPv6, proportional fonts. Those
  have their own plans.

### The list, ranked

| # | Defect | Rank | Phase |
|---|---|---|---|
| 1 | Kernel fault on a bad stack → triple fault, no diagnostic | **Critical** | R1 |
| 2 | Stack-protector trip under `-smp 2`, ~1 run in 3 | **Critical** | R2 |
| 3 | `errno` is one global shared by every thread | **Critical** | R3 |
| 4 | `initrd_init()` NULL-derefs on allocation failure | Latent | R4 |
| 5 | `gfx_fill_rect()` NULL-derefs when the back buffer failed | Latent | R4 |
| 6 | `.init_array` never runs; constructors silently skipped | Serious | R5 |
| 7 | `SIGSTOP`/`SIGTSTP` terminate instead of stopping | Serious | R6 |
| 8 | Socket syscalls return bare `-1`, losing the cause | Serious | R7 |
| 9 | Keyboard layout hardcoded US, no way to change it | Cosmetic | R8 |

---

## 2. Why each of these is real

Every claim below was checked in the source at the baseline commit.

### 1. The IST is allocated and never used — **critical**

`kernel/arch/x86_64/tss.c` allocates a per-CPU IST1 stack, and panics on OOM
doing so. `tss_entries[cpu].ist1_low/high` are filled in. Then:

```c
/* kernel/arch/x86_64/idt.c:22 */
idt[n].ist = 0;    /* no IST for now */
```

**No vector ever selects it.** The memory is reserved and unreachable.

The consequence is not theoretical. A fault taken while RSP is invalid — a
kernel stack overflow, or a #DF — cannot push an exception frame, so the CPU
escalates to a triple fault and the machine resets *with no output at all*.
Every kernel-stack bug in this OS currently presents as a spontaneous reboot.

### 2. The stack protector trips under `-smp 2` — **critical**

Observed twice during recent work: `test_selftest` printed

```
[security] STACK CORRUPTION DETECTED in kernel
```

and halted, then passed on three consecutive re-runs. It is **not recorded in
`TODO.md`** — two integration cases set `IL_SMP=1` to avoid the area, which
documents the workaround rather than the fault.

`__stack_chk_fail()` firing means either a genuine kernel stack overflow or a
corrupted canary. Both are memory corruption. The current SMP story ("APs idle,
BSP schedules") means this should be *impossible*, which makes it more
interesting rather than less: something is running on an AP, or shared state is
being mutated without a lock.

This is the one item on the list whose cause is genuinely unknown, and the plan
treats it accordingly — R2 is an investigation phase with a diagnosis as its
deliverable, not a fix committed in advance.

### 3. `errno` is a single global, shared by real threads — **critical**

```c
/* libc/src/libc.c */
static int __errno_storage = 0;
int *__errno_location(void) { return &__errno_storage; }
```

The comment says a TLS-backed cell "arrives in P9". P9 shipped: `pthread_create`
issues a real `SYS_CLONE` with `CLONE_VM|CLONE_THREAD|CLONE_SETTLS`, and the
kernel installs an FS base with `wrfsbase`. **Threads are real and share one
`errno`.**

Two threads failing syscalls concurrently overwrite each other's error code, so
a caller reads a plausible errno belonging to a different thread — the worst
kind of bug, because the value is wrong rather than obviously absent.

It has not bitten because nothing in the tree calls `pthread_create` outside
the unit tests. That is luck, and the SDK now invites third parties to write
threaded programs.

**The fix is small**, which is why it is worth doing now: `%fs`-based TLS
already works, and `pthread_tcb` already exists.

### 4 & 5. Two unchecked allocations — **latent**

`initrd_init()` (`kernel/fs/initrd.c`) allocates the vnode pool and `memset()`s
it on the next line with no NULL test. `gfx_fill_rect()`
(`drivers/framebuffer/graphics.c`) writes through `back_fb` without the
`!back_fb` guard that `gfx_putpixel()`, `gfx_clear()`, `gfx_flip()` and
`gfx_flip_rect()` all have.

Neither has fired: the initrd is parsed while the heap is empty, and the back
buffer is allocated early. Both are one line to fix, and both are the kind of
thing that only fires on a machine with unusual memory — where debugging is
hardest.

### 6. `.init_array` is never executed — **serious**

`__libc_start_main()` calls `main()` directly. A function marked
`__attribute__((constructor))` is compiled, linked into `.init_array`, and
**silently never runs**. `userspace/apps/gui-usb/gusb.c` has one.

This is the worst failure shape available: the code is present, the programmer
has every reason to believe it runs, and nothing reports otherwise.

Either the runtime walks `.init_array`, or the linker script rejects the
section. Doing neither — the current state — is the only unacceptable option.

### 7. `SIGSTOP`/`SIGTSTP` terminate — **serious**

```c
case DFL_STOP: /* job control lands in P6; treat as terminate for now */
case DFL_TERM:
    terminate_by_signal(signo);
```

Ctrl+Z **kills** the foreground program. The shell has `jobs`, `fg` and `bg`,
and `test_jobcontrol` passes 14 assertions — none of which suspends anything.
A user pressing Ctrl+Z loses their work.

### 8. Socket syscalls return bare `-1` — **serious**

`SYS_SOCKET*` and `SYS_NET_*` return `-1` for every failure, so `connect()`
cannot distinguish "no route", "refused" and "bad descriptor". This is the same
class of bug as the `EPERM`-becomes-`ENOENT` one fixed during SDK phase S3: an
error that reaches the caller as the wrong error is worse than one that
arrives as an opaque failure, because it sends the reader somewhere else.

### 9. Keyboard layout hardcoded US — **cosmetic, but user-visible**

Two fixed 128-entry tables in `drivers/keyboard/keyboard.c`, no keymap
abstraction, no runtime selection, no dead keys. A non-US keyboard produces
wrong characters outside the shared ASCII subset, with no workaround short of
editing the tables.

Ranked cosmetic because nothing corrupts — but it is the defect a user meets
first, and this repository's owner does not have a US keyboard.

---

## 3. Decisions

### D1. Fix causes, not symptoms

R2 in particular: the temptation with an intermittent SMP fault is to raise a
stack size or add a lock until it stops reproducing. That converts a crash into
a silent corruption. R2's deliverable is a **diagnosis**; the fix follows from
it and may land in a later phase.

### D2. Every fix gets a test that fails without it

Not "the suite still passes" — a specific test that goes red when the fix is
reverted. This is how the recent GL and filesystem work verified itself, and
several of these defects survived precisely because nothing asserted on them.

### D3. Ordered by danger, with one exception

R0 comes first and fixes nothing: it makes the critical failures *visible*.
Debugging a triple fault without output is guesswork, and R1's own test gate
needs a way to observe a fault that currently produces silence.

### D4. `.init_array` is fixed by running it, not by banning it

Both options are defensible. Running it is chosen because the SDK ships headers
that let a third party write `__attribute__((constructor))`, and a toolchain
that accepts the attribute and ignores it is a trap. If the runtime cost is
ever a problem, the linker script can reject the section later — that is a
reversible decision, and silence is not.

### D5. Keyboard layouts: a table, not a framework

A keymap format, a loader and a switching UI is a subsystem. R8 ships a
compile-time selectable table plus one non-US layout as evidence the
abstraction is real. Runtime switching can follow if anyone wants it.

---

## 4. Phases

### Phase R0 — Make failures visible

**Objective:** the diagnostics the rest of the plan needs.

#### Tasks

- [x] A panic path that survives a bad stack: print the register state,
      the faulting RIP and a stack trace to the serial port **before**
      touching anything that might fault again.
- [x] Emit the CPU number in every panic and exception message — R2's
      investigation is impossible without knowing which core faulted.
- [x] A boot-time self-check that reports whether IST is armed, so R1's
      change is visible in the log rather than only in the source.

#### Test gate

- A deliberately triggered kernel fault produces a diagnostic on the serial
  console rather than silence.
- The messages name the CPU.

#### Deliverable

`patches/FIX_R0_diagnostics.patch`

---

### Phase R1 — Arm the IST **(critical)**

**Objective:** a kernel fault on a bad stack produces a diagnostic instead of
a reset.

#### Tasks

- [x] `idt_set_gate()` gains an IST index parameter, or a companion that sets
      one; the hardcoded `ist = 0` goes.
- [x] Vector 8 (#DF) on IST1. Consider vector 2 (NMI) and 18 (#MC), and
      **state the choice** rather than doing all three by reflex — each IST
      slot is a separate stack that must be sized and never re-entered.
- [x] A guard page below the IST stack, so overflowing *it* is also caught.
- [x] Make the double-fault handler print and halt, never return.

#### Test gate

- A test that deliberately overflows the kernel stack produces a **double-fault
  diagnostic** naming the condition — where today the machine resets silently.
- Reverting the IST wiring turns that test back into a reset, which is how the
  test is known to test anything.
- The full suite still passes: arming an IST changes the stack a fault runs on,
  which is exactly the sort of change that disturbs unrelated things.

#### Deliverable

`patches/FIX_R1_ist.patch`

---

### Phase R2 — Diagnose the SMP stack-protector trip **(critical)**

**Objective:** know *why* before changing anything.

This phase may end without a fix. That is an acceptable outcome; an
unexplained intermittent memory corruption that has been *characterised* is
worth more than one that has been made harder to reproduce.

#### Tasks

- [ ] Reproduce it deliberately: run the affected cases under `-smp 2` in a
      loop and record the failure rate. Anecdotally ~1 in 3; measure it.
- [ ] Determine which CPU trips it (needs R0) and which stack is involved.
- [ ] Establish whether it is a genuine overflow or a corrupted canary — these
      have different causes and the message does not distinguish them.
- [ ] Audit what actually runs on an AP: the claim is "APs idle", and the
      symptom suggests otherwise.
- [ ] Check the per-CPU TSS/RSP0 programming, which SDK-era work touched.

#### Test gate

- A written diagnosis in the patch and in `TODO.md`: what corrupts what, on
  which CPU, and why.
- If a fix follows: a loop of 50 runs under `-smp 2` with no trip, against a
  measured baseline failure rate from before.
- If no fix follows: the reproduction recipe and the ruled-out hypotheses are
  recorded, exactly as the virtio-gpu hang was.

#### Deliverable

`patches/FIX_R2_smp_diagnosis.patch`

---

### Phase R3 — Thread-local `errno` **(critical)**

**Objective:** one `errno` per thread.

#### Tasks

- [ ] Add an `errno` cell to `struct pthread_tcb`.
- [ ] `__errno_location()` returns `&tcb_self()->errno_cell` when FS base is
      installed, and the existing global otherwise — the main thread of a
      non-threaded program must keep working before `pthread` is ever linked.
- [ ] Verify the fallback is correct at the point where `%fs` is not yet set:
      this runs before `main()`, and getting it wrong turns every program into
      a fault at startup.

#### Test gate

- Two threads each provoke a different errno in a loop; neither observes the
  other's. **This test fails with the current single global** — the point of
  D2.
- A single-threaded program's `errno` still works, including before `main()`.
- Every existing test still passes; `errno` is touched by nearly everything.

#### Deliverable

`patches/FIX_R3_tls_errno.patch`

---

### Phase R4 — The two unchecked allocations **(latent)**

**Objective:** two one-line fixes, and a check that there are not more.

#### Tasks

- [ ] `initrd_init()`: check the `kmalloc`, and fail the mount with a
      diagnostic rather than dereferencing NULL.
- [ ] `gfx_fill_rect()`: add the `!back_fb` guard its four siblings have.
- [ ] Sweep every `kmalloc`/`slab_alloc` in `kernel/` and `drivers/` for the
      same shape, and record the result — including "none others found", which
      is a useful thing to know.

#### Test gate

- A host unit test that calls `initrd_init()` with a failing allocator and
  asserts it reports failure instead of faulting.
- The sweep's findings are in the patch description.

#### Deliverable

`patches/FIX_R4_null_checks.patch`

---

### Phase R5 — Run `.init_array` **(serious)**

**Objective:** a constructor that is linked in actually runs.

#### Tasks

- [ ] `user.ld` keeps `.init_array`/`.fini_array` with `__init_array_start`
      and `__init_array_end` symbols.
- [ ] `__libc_start_main()` walks the array before calling `main()`, and
      `.fini_array` in reverse after it returns.
- [ ] Confirm ordering: constructors run before `main`, destructors after,
      destructors in reverse order.

#### Test gate

- `gusb`'s existing constructor prints its line — it does not today.
- A test program with three constructors and three destructors observes the
  correct order.
- Reverting the runtime walk makes those tests fail.

#### Deliverable

`patches/FIX_R5_init_array.patch`

---

### Phase R6 — A stopped state **(serious)**

**Objective:** Ctrl+Z suspends instead of killing.

#### Tasks

- [ ] A `THREAD_STOPPED` state the scheduler skips.
- [ ] `DFL_STOP` enters it instead of falling through to `terminate_by_signal`.
- [ ] `SIGCONT` resumes, and is delivered even to a stopped thread — a stopped
      process that cannot receive the signal that unstops it is worse than the
      current behaviour.
- [ ] `waitpid(WUNTRACED)` reports the stop; `WIFSTOPPED`/`WSTOPSIG` work.
- [ ] The shell's `fg`/`bg` drive it.

#### Test gate

- Ctrl+Z suspends a foreground program; `jobs` lists it stopped; `fg` resumes
  it and it continues from where it was.
- `waitpid` with `WUNTRACED` returns a stopped status.
- A stopped process consumes no CPU — asserted, since a "stopped" process that
  still runs is the likely wrong implementation.

#### Deliverable

`patches/FIX_R6_stopped_state.patch`

---

### Phase R7 — Real errno from socket syscalls **(serious)**

**Objective:** `connect()` says *why* it failed.

#### Tasks

- [ ] `kernel/net/socket.c` and the `SYS_NET_*` handlers return specific
      negative errnos: `ECONNREFUSED`, `EHOSTUNREACH`, `ETIMEDOUT`, `EBADF`,
      `EAFNOSUPPORT`, `EMFILE`.
- [ ] **Do not route them through `vfs_errno()`** — `EPERM` is 1, so `-EPERM`
      is indistinguishable from the generic `-1` sentinel. That trap is
      documented at `vfs_errno()` after it cost a debugging cycle in SDK phase
      S3; this phase must not walk into it.
- [ ] `perror()` output for a failed connection names the cause.

#### Test gate

- Connecting to a closed port yields `ECONNREFUSED`, not `-1`.
- Connecting to an unroutable address yields `EHOSTUNREACH` rather than
  hanging or reporting refusal.
- Operating on a closed socket yields `EBADF`.
- Each is asserted on the *specific* errno, so a wrong-but-nonzero value fails.

#### Deliverable

`patches/FIX_R7_socket_errno.patch`

---

### Phase R8 — Selectable keyboard layouts **(cosmetic, user-visible)**

**Objective:** a non-US keyboard produces the right characters.

#### Tasks

- [ ] A `struct keymap { const char lo[128], hi[128]; const char *name; }`,
      with the current tables becoming `keymap_us`.
- [ ] One additional layout as evidence the abstraction works — the plan
      suggests a Lithuanian or a UK map, chosen because someone can verify it.
- [ ] Compile-time selection, with a `kbd` shell command to switch at runtime
      if it falls out cheaply.
- [ ] AltGr, since most non-US layouts need it for characters US keyboards
      reach directly.

#### Test gate

- Each shipped layout maps a scancode set to its expected characters, tested on
  the host against the table.
- Switching layouts changes what the shell receives, asserted in QEMU.

#### Deliverable

`patches/FIX_R8_keymaps.patch`

---

## 5. Order and rationale

| Phase | Why here |
|---|---|
| R0 | R1 and R2 both need to observe faults that are currently silent |
| R1 | Critical, self-contained, and makes every later kernel bug diagnosable |
| R2 | Critical and unknown; benefits from R0 and R1 landing first |
| R3 | Critical, small, and the SDK invites the code that will trip it |
| R4 | Two one-line fixes; grouped so a trivial patch is not three patches |
| R5 | Serious and self-contained |
| R6 | Serious; the largest of the repairs, touching scheduler and shell |
| R7 | Serious but mechanical |
| R8 | Cosmetic, and the only one that is purely additive |

**If only two phases are ever built, build R1 and R3.** R1 turns silent resets
into diagnosable faults, which pays for itself on the next kernel bug. R3 is a
correctness defect in code third parties are now being invited to write, and
the fix is small.

---

## 6. Risks

**R2 may not conclude.** An intermittent fault that reproduces once in three
runs can take longer to characterise than everything else here combined. The
phase is written so that a documented dead end is a legitimate outcome — the
virtio-gpu hang is the precedent, and recording what was ruled out is worth
more than a fix that merely stops the symptom.

**R1 changes what stack a fault runs on.** That is the point, and it is also
the kind of change that disturbs unrelated behaviour. The gate requires the
full suite, not just the new test.

**R3 runs before `main()`.** `__errno_location()` is called from paths that
execute before the FS base is installed. Getting the fallback wrong turns
every program into a startup fault — a spectacular failure, at least, rather
than a subtle one.

**R6 is the largest repair.** A stopped state touches the scheduler, signals,
`waitpid` and the shell. It is ranked below the criticals for that reason,
despite being the most user-visible.

**These fixes will surface others.** Arming the IST means kernel stack
overflows start being *reported* rather than presenting as reboots, and there
may be some. That is the plan working, not failing — but it should be expected
rather than treated as a regression.

---

## 7. What this plan does not do

- **No new features.** `epoll`, `scanf`, `readline`, `execvpe`, `MAP_SHARED`
  and `auxv` are missing, not broken.
- **No SMP scheduler.** R2 diagnoses one specific fault; per-CPU run queues and
  TLB shootdown are a design project.
- **No cryptography, no TLS.** `INTERNET_PLAN.md`.
- **No `/opt` persistence, no symlink-aware install allowlist.** Both are
  recorded in `TODO.md` with their reasoning; both need a subsystem this plan
  does not build.
- **No promise that the list is complete.** It is what an audit found. The
  next audit will find more, which is an argument for doing them regularly
  rather than for pretending this one was exhaustive.
