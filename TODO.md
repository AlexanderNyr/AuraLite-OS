# AuraLite OS TODO

The historical 14-phase roadmap is complete. This file tracks the **current**
known limitations and future work for the post-phase tree. See
[`PLAN.md`](PLAN.md) for milestone history and [`docs/status.md`](docs/status.md)
for the feature matrix.

---

## Current Known Limitations

### Kernel / CPU / scheduling

- **The IST is allocated but never used, so a bad-stack fault triple-faults.** (`FIXES_PLAN.md` R1)
  `tss_init()` allocates a per-CPU IST1 stack and panics on OOM allocating it
  (`kernel/arch/x86_64/tss.c`), and `tss_entries[cpu].ist1_low/high` are
  filled in — but `idt_set_gate()` hardcodes `idt[n].ist = 0`
  (`kernel/arch/x86_64/idt.c:22`, comment: *"no IST for now"*), so **no vector
  ever selects it**. The memory is reserved and unreachable. The consequence
  is specific: a fault that occurs when RSP is invalid — a kernel stack
  overflow, or a #DF — cannot push an exception frame, so it escalates to a
  triple fault and the machine resets with no diagnostic. Double fault (8),
  NMI (2) and machine check (18) are the vectors that conventionally need
  IST entries.
- **R2 diagnosis: the `-smp 2` stack-protector trip does not reproduce on this
  tree; the real SMP-only corruption behind the "flaky under `-smp 2`" reports
  is a missing FPU/SSE context switch.** (`FIXES_PLAN.md` R2 →
  `patches/FIX_R2_smp_diagnosis.patch`; reproduction harness
  `tools/repro_smp_chk.sh`, recorded runs in `build/r2_repro/`.)
  Measured per the R2 gate:
  - `selftest` under `-smp 2`, 31 boots: **0 protector trips, 0 IST #DF
    (R1), 0 kernel-stack guard hits** — the anecdotal ~1-in-3 trip rate was
    not reproduced and no trip was recaptured for classification.
  - `gltest` (373 FP-heavy rasteriser checks) under `-smp 2`: **13 of 16
    recorded boots failed at least one check** (6/6 in the final controlled
    batch); every rerun failed *different, random* checks — 13 distinct
    names seen, e.g. `ras_gouraud_blue`, `lit_distance_attenuation`,
    `tex_bilinear_average`, `geo_perspective_foreshortens`.
  - `gltest` under `-smp 1` control: **7/7 clean**.
  Root cause of the SMP-only flakiness: **the kernel switches no FPU/SSE
  state.** `kernel/proc/context.asm` saves only callee-saved GPRs + RFLAGS
  and the TCB has no FPU area; there is no `fxsave`/`fxrstor` anywhere in
  the kernel (the claim in `kernel/proc/signal.c` that save is "deferred
  until OSFXSR" is stale — `CR4.OSFXSR` is already set). xmm registers are
  per-CPU hardware: with the H8 scheduler's per-CPU run queues, preemption
  and work stealing (`scheduler_rq.c`), a thread descheduled or migrated
  mid-computation resumes with another CPU's stale FP state — and gltest
  links ~9.7k xmm instructions. At `-smp 1` the co-tenant threads happen
  not to dirty xmm during its quanta; physical migration under `-smp 2`
  loses the state unconditionally. This is user-visible data corruption,
  not a canary event. The fix (FXSAVE/FXRSTOR in the context switch) is a
  separate follow-up phase and deliberately not part of R2. **✅ DONE
  (`MATURITY_PLAN.md` M1 → `patches/MAT_M1_fpu_context.patch`):**
  `context_switch` now eagerly `fxsave`/`fxrstor`s a per-TCB 512-byte area
  (a `fpu_valid` flag zeroes on memset so the first switch-IN runs `fninit`
  + default MXCSR), and the signal frame's stale FP stub was filled in.
  `gltest` passes 373/373 under `-smp 4` (was 13/16 failing at `-smp 2`);
  new gate `/tests/fpustress` + `test_fpu_smp.sh`; the `IL_SMP=1` pin in
  `test_opengl.sh` is removed.
  Permanent instrumentation merged so any *future* trip is classifiable:
  `__stack_chk_fail()` (`kernel/lib/stack_protector.c`) now dumps, over the
  R0 lock-free serial path, the cpu#, a trip counter, the detection RIP,
  rsp/rbp against the current thread's kernel-stack bounds (via a safe
  `sched_current()`), the expected guard, a bounds-checked heuristic
  frame-cookie readback, and a verdict line: `GENUINE-OVERFLOW` (rsp out of
  bounds) vs `CANARY-VALUE-MISMATCH` (rsp in bounds ⇒ the cookie was
  overwritten by another writer — the racing-CPU shape).
  Ruled out during the investigation:
  - genuine kernel-stack overflow — guard pages armed + R1's IST #DF
    A/B-verified; neither fired in any campaign boot;
  - guard-reseed race — `stack_protector_init()` runs once on the BSP in
    `kmain`, long before `smp_init()` starts the APs;
  - "APs only idle" — false; APs execute real threads (see the updated
    bullet below), and the per-CPU TSS/RSP0 programming re-audited clean
    (`tss_set_rsp0_for_cpu()` on every context switch, `scheduler.c:136`).
  Recorded benign anomalies (no corruption markers, causes unknown, kept on
  file): 1/31 boots read `/tests/selftest` as 0 bytes from the initrd
  (`[elf] too small`); 2/31 boots failed the benign `isatty(stdout)` check.
  The latter cannot come from `tty_ioctl()` itself (TCGETS there returns 0
  unconditionally) — the intermittent `EINVAL`/`EFAULT`/`ENOTTY` must
  originate in the `SYS_IOCTL` dispatch path or the fd layer, so it feeds
  the errno work of R3/R7 rather than this phase.  A 3-run spot check on
  the unpatched base (f09cc69) was clean; no causal link to the R2 delta is
  plausible (docs, a host-side script, and the never-executed trip path).
- **SMP scheduling.** Superseded baseline: since the H8 scheduler, all
  online CPUs run preemptive round-robin with per-CPU run queues,
  LAPIC-timer ticks, cross-CPU work stealing (`scheduler.c`,
  `scheduler_rq.c`) and IPI TLB shootdown (`tlb_shootdown.c`) — APs
  demonstrably execute real threads (re-audited for R2).
- ~~**Address-space reaping is incomplete.**~~ **Done (H2):** dead TCBs,
  kernel stacks, process FDs, and user page-table/address-space frames are
  deferred-reaped from a safe stack via `thread_reap_zombies()` and
  `paging_free_address_space()`.
- ~~**Blocking model is primitive.**~~ **Done (H4):** wait queues back
  blocking pipes, futex waits, `select()`, and `nanosleep()`.
- **Interrupt model is transitional.** LAPIC enable and per-CPU LAPIC timers
  are implemented. **I/O APIC routing is now done** (`MATURITY_PLAN.md` M2): the
  16 legacy ISA IRQs are delivered through the I/O APIC (PIT → GSI2, etc.) with
  the 8259 masked/decoupled, QEMU-hardcoded at the PC-standard base. The
  remaining gap is real-hw discovery — the I/O APIC base address and Interrupt
  Source Overrides should come from the ACPI MADT (parsed in the bootloader)
  rather than the hard-coded QEMU defaults — and fully interrupt-driven device
  data paths (virtio-net IRQ RX; MSI/MSI-X for virtio/virtio-gpu).

#### P10 / POSIX follow-ups

- **`MAP_SHARED` is not truly shared.** Anonymous `MAP_SHARED` is accepted but
  degraded to a private mapping (no cross-process shared page-cache VMAs yet),
  and file-backed `MAP_SHARED` returns `-ENOSYS`. Implement real shared VMAs
  with write-back before relying on `mmap`-based IPC.
- **execve passes argv/envp but no real auxv.** The initial process stack is
  built per the System V AMD64 ABI (argc/argv/NULL/envp/NULL) but the auxiliary
  vector contains only an `AT_NULL` terminator. Add `AT_PAGESZ`/`AT_RANDOM`/etc.
  when a dynamic loader needs them.
  **Done (MATURITY_PLAN.md M5):** `build_initial_stack` now emits a full auxv
  (AT_PHDR/PHENT/PHNUM, AT_PAGESZ, AT_ENTRY, AT_UID/EUID/GID/EGID, AT_SECURE,
  AT_RANDOM=16 kernel-seeded bytes, AT_EXECFN); `elf_load` exposes the mapped
  phdr address + e_phnum. libc locates the auxv past envp and `getauxval()`
  scans it (new `sys/auxv.h`). Also fixed a latent bug: the init shell booted
  on a garbage stack frame (no argc/argv/envp at all) -- now a valid ABI frame.
  Gate: /tests/auxvtest + test_auxv.sh (6/6); exec/spawn/argv tests still green.
- **No `execvpe`/`fexecve`/`posix_spawn`.** Only `execve`/`execv`/`execvp`
  wrappers exist; `execvp` honours `PATH` (default `/bin`) with no per-segment
  `EACCES` retry semantics.
- **`epoll` is not implemented** (low priority): `poll()` is provided in libc on
  top of `select()`; `epoll_create1`/`epoll_ctl`/`epoll_wait` are unimplemented.

### Security / syscall robustness

- **User pointer validation is basic.** Syscall dispatch now uses
  `validate_user_range`, `copy_from_user` and `copy_to_user`, but AuraLite still
  lacks a fault-recovering uaccess mechanism and a full audit of every future
  user-pointer path.
- ~~**No `errno` or structured negative error codes.** Most failures return
  `-1`.~~ **Done (P1):** in-band negative-errno ABI; kernel returns `-EXXX`,
  libc decodes to `errno`/`-1`. See `docs/syscall_abi.md`.

#### errno follow-ups (discovered during P1)
- ~~**errno granularity is dispatch-layer, not native.**~~ **Done for `vfs.c`
  + tmpfs/initrd/procfs:** they return specific `-Exxx` (EBADF/ENOENT/EMFILE/
  ENOTDIR/EXDEV/ENOSYS/EROFS/ENOSPC/…); a `vfs_wrap_err()` helper maps any
  remaining generic `-1`. Still outstanding: push native errno into the **disk
  FS drivers** (fat32/ext2/diskfs — hundreds of internal block-I/O `-1`s,
  mostly EIO) and into `process.c`.

#### P2 / open-flags follow-ups
- ~~**Per-FD status flags, not shared OFDs.**~~ **Done (P3):** ref-counted
  `struct ofd` now holds offset + status flags, shared across dup/dup2/F_DUPFD/
  fork; FD_CLOEXEC stays per-fd.

#### P4 / signal follow-ups
- ~~alarm/pause/sigsuspend~~ **Done:** SYS_ALARM/PAUSE/SIGSUSPEND implemented
  (alarm via the PIT tick + signal_tick()).
- ~~SIGCHLD on child exit~~ **Done:** posted to a living parent in
  thread_exit_with_code (still wants a dedicated fork-based gate once fork is
  robust against the SYSCALL-save-area race).
- ~~SIGPIPE / -EINTR on blocking reads~~ **Done:** pipe-with-no-readers posts
  SIGPIPE + -EPIPE; stdin/pipe yield loops abort with -EINTR (or partial count).
- ~~**No full SA_RESTART rewind.**~~ **Done (H7):** restartable blocking
  syscalls save restart metadata on `-EINTR` and `sigreturn` re-dispatches them
  when the handler was installed with `SA_RESTART`.
- ~~**SA_SIGINFO siginfo_t** not populated (handler gets signo only; rsi/rdx = 0).~~
  **Done (MATURITY_PLAN.md M5):** an SA_SIGINFO handler now receives a real
  siginfo_t (si_signo/si_code/si_addr for faults, si_pid for SI_USER) and a
  ucontext_t mirroring the saved registers. signal_raise_fault carries the
  faulting address + per-exception si_code (CR2 -> SEGV_MAPERR/ACCERR, etc.).
  Landed with a fix to a latent M1 defect: the signal-frame fxsave #GP'd
  because the IRQ/syscall entry stubs do not 16-align the stack before calling
  C -- now uses a runtime-aligned scratch. Gate: /tests/siginfotest +
  test_siginfo.sh (5/5); test_signals (9/9) still green.
- **IRQ/syscall entry stubs do not maintain 16-byte C-ABI stack alignment.**
  isr_common_stub pushes 17 words (errcode+vector+15 GPRs) and never realigns
  before call isr_handler, so a compiler-aligned stack local in an
  IRQ/syscall-context C function can land misaligned and any aligned SSE op
  (e.g. fxsave) #GPs. Harmless while the kernel builds -mno-sse, but a
  dedicated stub fix is the proper cure; the M5 signal path works around it
  with a runtime-aligned scratch. test_stopped is also failing for an unrelated
  Ctrl+Z sendkey timing reason (pre-existing, fails on M1 too).
- ~~**Ctrl+C/Ctrl+Z/Ctrl+\\ → SIGINT/SIGTSTP/SIGQUIT**~~ **Done (P5):** the
  console stdin path and /dev/tty0 line discipline generate these via ISIG and
  the tty->fg_pgid indirection. Full per-process-group routing arrives in P6.

#### P6 / job-control follow-ups
- ~~**Interactive shell job control not implemented.**~~ **Shell side done and
  validated in QEMU by `FIXES_PLAN.md` R6** (`cmd &`, `jobs`, `fg`, `bg` are
  built into `init`; the R6 gate drives them: Ctrl+Z suspends, `jobs` lists,
  `fg` resumes).
- ~~**No stopped state / WUNTRACED.**~~ **Done (`FIXES_PLAN.md` R6 →
  `patches/FIX_R6_stopped_state.patch`, `/tests/stoptest`,
  `tests/integration/cases/test_stopped.sh`).** `THREAD_STOPPED` exists:
  `DFL_STOP` signals (SIGTSTP/SIGSTOP/SIGTTIN/SIGTTOU) park the thread off
  the run queues instead of terminating it; `signal_send()` wakes a stopped
  thread from the sender side for SIGCONT (resume) and SIGKILL (dies at its
  next boundary in `terminate_by_signal`); waitpid(WUNTRACED) reports each
  stop exactly once, encoded `0x7f | (stopsig << 8)` for libc's
  WIFSTOPPED/WSTOPSIG.  The gate asserts a stopped child produces no output
  (consumes no CPU) and that `run`+Ctrl+Z+`jobs`+`fg` drive it end to end.
- **n_children is fork/spawn-tracked but not perfectly precise** across orphan
  adoption (a parent that exits without waiting leaves its count stale, but it
  is dead so this is benign). Revisit if a reparent-to-init policy is added.

#### P5 / TTY + stdio follow-ups
- **scanf/fscanf** not implemented (fgets + manual parsing only).
- **readline() line editor** (arrows/history, raw-mode shell input) deferred.
- **/dev/ttyS0** (UART serial tty) not registered; only /dev/tty0 exists.
- **init not rewired to /dev/tty0** — kept the existing fd-0 console path to
  avoid shell regressions; the line discipline runs only for programs that open
  /dev/tty0 directly. ISIG/Ctrl+C is bridged into the fd-0 path manually.
- **printf now line-buffered via stdout FILE*** — programs that print a prompt
  without a trailing newline must fflush(stdout) (POSIX-correct, but a behavior
  change worth a QEMU smoke test).
- **No column tracking** for ECHOE/tab-expansion/multi-column ^X erase; VERASE
  of a tab or control char erases a fixed 1–2 columns, not the true width.
- **VMIN/VTIME timers** are approximated (the syscall layer's yield loop honors
  VMIN counts; VTIME deciseconds timing is not yet wired to the PIT).
- ~~**FP/SSE state is not saved in the signal frame.**~~ **Done (H7):**
  signal delivery saves a 512-byte FXSAVE frame and `sigreturn` restores it.
- **Signal state is single-CPU safe only** (guarded by IF-disabled return
  boundaries); SMP needs atomic sig_pending updates and locking.
- ~~**Job-control STOP** is treated as terminate for now (no stopped state
  until P6).~~ **Done (`FIXES_PLAN.md` R6):** DFL_STOP now enters a real
  THREAD_STOPPED state with SIGCONT resume.
- **alarm()/signal_tick() scan all threads each tick** (O(threads)); fine at
  current scale, revisit with a timer wheel if thread counts grow.

#### P3 / OFD follow-ups
- **OFD refcounts are non-atomic.** Plain `int refcount`, safe only under the
  single-threaded VFS. SMP/preemptive FS access needs atomic dec-and-test
  (release/acquire ordering) plus a strict lock hierarchy (files-table → OFD
  offset → vnode) and an fget/fput temporary reference held across blocking I/O
  to avoid use-after-free.
- **`close_process_fds()` assumes the exiting thread is current** (vfs_close
  operates on `current_fd_table()`). True today (called with `self`); would be
  wrong if ever invoked on a non-current zombie — add a table-explicit close.
- **fork() FD-sharing integration test deferred.** Needs fork robust against the
  per-thread SYSCALL-save-area race; dup() sharing in test_lseek validates the
  same OFD mechanism meanwhile.
- **O_APPEND atomicity** currently relies on the single-threaded VFS; needs a
  per-vnode write lock once FS access becomes preemptible/SMP.
- **mkdir() still takes only a path** (no `mode_t`); POSIX `mkdir(path, mode)`
  and `umask` arrive in P7. `sys/stat.h` deliberately omits the mkdir prototype.
- **O_NONBLOCK** is honored for pipes (EAGAIN); devices/sockets that can block
  are not yet wired to it.
- ~~**Socket/net syscalls return bare `-1`.**~~ **Done (`FIXES_PLAN.md` R7,
  gated by `tests/integration/cases/test_socket_errno.sh`).** `SYS_SOCKET*` /
  `SYS_NET_*` failures now carry specific negative errnos end-to-end:
  `ECONNREFUSED` (RST during connect), `EHOSTUNREACH` (unresolvable ARP —
  failed fast in `tcp_open()` instead of a full SYN retry ladder),
  `ETIMEDOUT` (SYN-ACK never came), `EBADF` (unknown/foreign/closed socket),
  `ENOTCONN` (send/recv before connect), `EAFNOSUPPORT` (non-AF_INET family),
  `EMFILE` (socket or TCP-handle table exhausted).  Nothing is routed through
  `vfs_errno()` (the EPERM==1 substitution trap); `strerror`/`perror` name the
  cause.
- ~~**P1 libc headers still missing.**~~ **Done:** `limits.h`, `stdbool.h`,
  `assert.h`, `ctype.h` (+impl), `math.h` (+impl). `stdint.h`/`stdarg.h` use the
  freestanding compiler headers.
- **libm accuracy is series-based (~1e-9), not last-ULP**, and only covers the
  ten functions listed in `math.h`; no `tan/asin/atan2/fmod/modf/frexp`, no
  `float` variants, no errno/`HUGE_VAL` domain-error reporting. Revisit in P10.
- ~~**`errno` is a single global, not thread-local.**~~ **Done (`FIXES_PLAN.md`
  R3 → `patches/FIX_R3_tls_errno.patch`,`/tests/errnotest`,
  `tests/integration/cases/test_tls_errno.sh`).** `errno` now lives in
  `pthread_tcb.errno_cell`: `__errno_location()` reads it via `%fs:0` once the
  main thread's static TCB is installed at the very top of
  `__libc_start_main()` (any pre-install errno survives the cut-over), and
  falls back to the old global before that — so non-threaded programs and the
  pre-`main()` window keep working.  Landing R3 surfaced that the P9 pthread
  runtime had in fact never executed successfully (no in-tree test ever
  created a thread); five pre-existing defects are repaired with it:
  (a) `context_switch`/`do_arch_prctl` used `wrfsbase` with CR4.FSGSBASE
  never enabled → kernel `#UD` on the first switch into a thread;
  (b) `fork_child_sysret` never installed the child's FS.base (garbage TLS
  on first entry);
  (c) libc's `pthread_create` seeded no return frame on the clone child's
  fresh stack — the `syscall()` wrapper's `ret` jumped into the TCB;
  (d) `kthread_create()` published half-initialised TCBs — a REMOTE cpu is
  not stopped by the caller's `cli` and could steal a child mid-do_clone/
  do_fork/spawn (now: `kthread_create_unstarted()` + `kthread_start()`);
  (e) `thread_reap_zombies()` ignored `switch_parked` — a zombie could be
  memset + have its kernel stack freed while its last cpu still executed
  the exit tail on that stack; this is what the R2 instrumentation then
  CAPTURED live during the R3 regression: the first recorded canary trip
  (cpu1, `spinlock_acquire_irqsave`, TCB already zeroed — its fingerprint).
  The kernel invariant is now `FS.base == current->tls_base` at every
  context switch and every first user entry (clone, fork, exec), and a
  thread is invisible to other cpus until fully initialised.
- **SYSCALL state uses globals.** Saved user `RCX/R11/RSP` state is not designed
  for true concurrent SMP syscalls.
- ~~**ELF final permissions are simplified.**~~ **Done (N4):** user
  ELFs now use page-aligned RX/R/NX/RW/NX `PT_LOAD` segments, and the kernel
  maps `PF_W`/`PF_X` into exact writable/NX PTE flags. User stacks are NX.

### Processes and file descriptors

- ~~**FD semantics still need final POSIX precision.**~~ **Done (MATURITY_PLAN.md M5):**
  fork()/dup() share the open-file description (same seek offset), now gated by
  /tests/fdsharetest (parent reads, fork, child continues at the shared offset;
  dup'd fd then continues from there). close_range/closefrom wired; precise
  waitpid + reparent-to-init confirmed present.
- **`fork`, `execve`, `wait4` are simplified.** They are sufficient for the
  bundled demos/tests, but not POSIX-complete.
- **User VM is still eager/simple.** `brk`, `mmap`, and `munmap` exist, but
  lazy VMAs and true file-backed `MAP_SHARED` remain future work.
- **IPC primitives are partial.** Pipes, signals, futexes, wait queues, baseline
  in-memory named FIFOs (`mkfifo`) and baseline in-memory symbolic links
  (`symlink`/`readlink`/`lstat`) exist; shared memory, hard links (`link`),
  persistent per-filesystem FIFO/symlink storage and full symlink path-component
  following remain future work.

### Security / cryptography

- ~~**`getrandom()`/`getentropy()` draw from a PRNG, not a CSPRNG (Q16).**~~
  **Done (`INTERNET_PLAN.md` phase N0 → `patches/NET_N0_entropy.patch`).**
  The generator is now a ChaCha20 DRBG (`kernel/rng_core.h`, RFC 8439) fed
  by RDSEED/RDRAND when the CPU provides them and by an interrupt-timing
  jitter pool otherwise.  Until real entropy exists, `getentropy()` returns
  `-ENOSYS` and `getrandom()` blocks (or `EAGAIN` with `GRND_NONBLOCK`) —
  guessable bytes are never served.  The estimated entropy is logged at
  boot.  It remains unaudited hobby-OS entropy; do not treat it as an
  `/dev/urandom` equivalent on real hardware.
- **Crypto primitives and X.509 parsing exist; protocols do not.**
  `INTERNET_PLAN.md` phases N1/N2 shipped `lib/libatls/` (SHA-256/512,
  HMAC, HKDF, ChaCha20-Poly1305 AEAD, X25519, Ed25519 verify, and
  zero-copy depth-bounded X.509 parsing) — userspace, RFC-vector-verified.
  Still missing: the TLS 1.3 handshake and record layer (N3/N4),
  certificate validation with RSA-PKCS#1v1.5 verification (N5), and the
  HTTPS client (N6).  Until then there is no HTTPS.  `kernel/fs/btrfs.c`
  still writes its SHA-256 checksum field as zeros (kernel code; it does
  not use libatls, by design D2).

### Storage / filesystems

- **AHCI is QEMU-focused.** DMA read/write passes the integration tests on QEMU
  AHCI disks, but broad hardware/hypervisor coverage is still experimental.
- **`/disk` is intentionally tiny.** Flat namespace, 8 files maximum, 4 KiB per
  file.
- ~~**`initrd_init()` does not check its `kmalloc`.**~~ **Done (`FIXES_PLAN.md`
  R4 → `patches/FIX_R4_null_checks.patch`, host test
  `tests/unit/test_initrd_allocfail.c`).** `initrd_init()` now returns `int`;
  when the vnode pool cannot be allocated it logs a diagnostic, reports the
  image back as empty and returns -1, and `kernel.c` skips the
  `vfs_mount("/")`.  The R4 sweep then audited every `kmalloc`/`slab_alloc`
  call in `kernel/` and `drivers/` (58 + 10 sites): all the other unchecked
  same-shape sites it found — the lazy scratch buffers of `btrfs_init()`,
  `ext4_init()`, `f2fs_init()`, `ext2`'s `block_buf` (mount and format) and
  `fat32`'s `cluster_buf` (mount and format), the six `get_ind()` users in
  ext2's `bmap()`, and the `sched_init()` kmain TCB — now fail the
  mount/format/init with a diagnostic too; every remaining site in the sweep
  was already NULL-checked.
- **tmpfs has no `mkdir`.** `/tmp` and `/opt` are flat: `tmpfs_ops` has no
  `.mkdir` entry and `valid_name()` rejects any path containing a slash, so
  `mkdir /tmp/x` fails and always has. Directories work on FAT32 and ext2.
  Found while fixing `test_shell_all.sh`, which had been asserting that the
  mkdir *succeeded* — the case is not in `run_all.sh`, so nothing had run it.
- ~~**`.init_array` is never executed.**~~ **Done (`FIXES_PLAN.md` R5 →
  `patches/FIX_R5_init_array.patch`, `/tests/ctortest`,
  `tests/integration/cases/test_init_array.sh`).** `user.ld` now keeps
  `.init_array`/`.fini_array` with `__init_array_start/end` (and fini)
  symbols, and `__libc_start_main()` walks `.init_array` forwards before
  `main()` and `.fini_array` in reverse after it returns (empty arrays link
  as start == end, so plain programs are untouched).  `gusb`'s constructor
  finally prints `[gusb] ctor`; `/tests/ctortest` proves constructors run
  before `main()` in link order and destructors after it in reverse, and
  fails when the runtime walk is reverted.
- **`/opt` does not persist across a reboot** (FSLAYOUT_PLAN phase F1). It is
  a tmpfs volume, so an installed package is gone after a restart. Making it
  durable needs a writable disk that is present on every boot; the persistent
  filesystems here mount only when their device exists, so the choice is
  between a location that is always there and one that always survives, and F1
  took the first. The defect it fixed was `apm` installing into `/tmp` — the
  one directory guaranteed to be wiped *and* not reserved for programs.
- **The installation allowlist does not follow symlinks** (FSLAYOUT_PLAN F1).
  `exec_path_canonical()` is lexical: it defeats `/opt/../etc/evil`, but a
  symlink inside an allowed directory pointing outside it would let a write
  through. Closing it means resolving the parent through the VFS, following
  links, before judging the path.
- **The VFS does not canonicalise paths at all.** `/tmp/../evil` is split at
  the `/tmp` mount and the remainder handed to tmpfs, which rejects names
  containing a slash. Traversal therefore fails today for an incidental
  reason. This is worth fixing on its own terms — until it is, path handling
  behaves differently from every POSIX system.
- **FAT32/ext2 are hobby implementations.** FAT32 supports subdirs/LFN and FAT date/time stat decoding, and ext2 supports Linux-mkfs images plus in-kernel mkfs with inode timestamps. Crash consistency, journaling, full permission semantics and extensive fsck-style recovery are out of scope.

### Input

- ~~**The keyboard layout is hardcoded US, with no way to change it.**~~
  Done (`FIXES_PLAN.md` R8): `keymap_us` and `keymap_de` live in
  `drivers/keyboard/keymap.c` behind `struct keymap` with a Shift layer and
  an AltGr third layer (`KB_MOD_ALTGR`, raised by right Alt on PS/2 and USB);
  `make KEYMAP=de` selects the boot default at compile time and the `kbd`
  shell command (non-standard `SYS_KBD_LAYOUT` 601) switches/enumerates at
  runtime.  The remaining gap from the old text is unchanged: still no
  dead-key support (the German ´ key emits nothing unshifted).

### USB / devices

- **USB class support is still intentionally narrow.** UHCI/OHCI/EHCI/xHCI now
  have QEMU-tested paths for the supported HID/MSC cases, and hub downstream
  enumeration works including xHCI route strings. HID and MSC runtime
  attach/read/detach are QEMU-tested through the polling hotplug monitor, and
  active media is exposed at `/usb` through usbfs. FAT32 superfloppy/partition
  root files are auto-detected read-only under `/usb/fat`; writable FAT32, ext2
  hotplug automount, isochronous devices and broader hardware recovery paths are
  still future work.
- **USB HID generic support is partial.** Boot keyboard/mouse works through UHCI,
  OHCI, high-speed EHCI and xHCI; generic keyboard and mouse/tablet report
  descriptors are parsed for common QEMU-tested layouts. Full HID collections/usages
  and EHCI full/low-speed split transactions remain future work.
- **Bluetooth and Wi-Fi are protocol frameworks.** No complete lower-level
  chipset/transport driver is registered by default.

### Networking

- **Network I/O transition in progress.** e1000 now has an INTx IRQ-capable RX/TX core and software RX queue; TCP, ARP, DHCP, ICMP, and kernel UDP/DNS receive waits use bounded IRQ-backed NIC waits. UDP user sockets and a basic fixed-RTO TCP retransmission path are implemented; remaining N2 work is deeper socket blocking edge cases and production TCP features.
- **NIC backend abstraction (netdev).** The IP stack now talks to an active NIC through `kernel/net/netdev.{h,c}` rather than calling a driver directly. e1000 is the default; modern virtio-net (`drivers/virtio_net/`) is a fully working fallback (DHCP/ICMP/DNS/TCP validated under QEMU `-device virtio-net-pci`). virtio-net is currently a polling data path with no IRQ.
- **Minimal TCP.** User space now has process-owned socket-style handles, and
  the underlying TCP transport now has a one-segment fixed-RTO retransmission
  strategy for SYN/data/FIN. It still lacks congestion control, sliding windows
  and production-grade packet queues.
- **DHCP can fall back in QEMU SLIRP.** Integration tests tolerate fallback
  static addressing for deterministic boots.
- **e1000 RX ring is not drained while idle.** Once the boot-time network
  self-tests finish and nothing in user space is reading, unsolicited frames
  fill the RX ring and the driver emits `[e1000] RX overrun (drops=N)` on the
  console every few seconds, with `N` climbing without bound for the life of
  the boot. Nothing malfunctions — the counter is honest and the stack recovers
  — but it is real packet loss and it floods the serial log, which makes long
  integration runs harder to read. The fix is to consume and discard frames
  that no socket claims (or to mask RX interrupts when no consumer exists)
  rather than to silence the message.

### Graphics / GUI

- ~~**`gfx_fill_rect()` does not check `back_fb` for NULL.**~~ **Done
  (`FIXES_PLAN.md` R4 → `patches/FIX_R4_null_checks.patch`).** `graphics.c`
  allocates the back buffer with `kmalloc` and tolerates failure
  (`if (back_fb) memset(...)`), and `gfx_putpixel()`, `gfx_clear()`,
  `gfx_flip()` and `gfx_flip_rect()` all begin with a `!back_fb` guard.
  `gfx_fill_rect()` now does too, so on a machine where the back-buffer
  allocation failed every drawing entry point degrades quietly.

- **RESOLVED (residue ledger R1, RES-40): the virtio-gpu init hang no
  longer reproduces.** A current boot with `-device virtio-gpu-pci`
  answers GET_DISPLAY_INFO (`scanout 0: 1280x800`), reaches the shell,
  and `test_virgl_gpu.sh` now runs with ENABLE_FULL_ASSERTS=1 — green.
  The fix was never made deliberately for this symptom; the G13/K1
  backing-store work is the era it disappeared in.  The original
  record kept below for the trail:
  Booting with `-device virtio-gpu-pci` used to stop after
  `[virtio-gpu] found modern GPU` and never reach the shell.

  *Not caused by any GL phase:* bisected to before G11d — commit `9188c85`
  hangs identically. It was simply never exercised, because no integration
  case attached a GPU until `tests/integration/cases/test_virgl_gpu.sh` was
  added in G13.

  *Traced as far as* the first `GET_DISPLAY_INFO`. Ruled out by
  instrumentation: the BAR mapping, the notify-register write (it completes),
  the queue setup (`status=0b`, `queue_enable=1`, sane notify offset and
  multiplier), and the 64-bit-BAR case (this device's BAR is 32-bit). The wait
  loop then makes **zero** iterations and still does not return, which points
  at the used-ring page — the descriptor/avail/used pages come from
  `pmm_alloc_frame()` and are read through the HHDM, so the suspicion is that
  mapping rather than the notification path.

  Fixing it is virtio driver work, not GL work, so it is recorded here rather
  than folded into a GL phase. `test_virgl_gpu.sh` asserts what holds today
  (the device is found, nothing faults) and has an `ENABLE_FULL_ASSERTS` flag
  that turns the rest on in one edit once this is fixed.

- **GPU acceleration is early.** The bootloader-provided framebuffer remains the primary GUI
  surface, while virtio-gpu 2D mirroring and the VirGL command transport are
  present as experimental acceleration paths. The VirGL path now completes a
  present pipeline (fenced SUBMIT_3D -> TRANSFER_TO_HOST_3D -> SET_SCANOUT ->
  RESOURCE_FLUSH) to scan a 3D render target out to the display, falling back to
  software rendering when no virgl-capable host GPU is attached. Phase G13 added
  the user-space half: `libgl/src/glvirgl.c` implements probe, clear and
  present over `SYS_GPU_CALL`, and 3D resources now get guest-side backing so
  transfers actually reach the device (K1 shipped without it — see
  `CHANGELOG.md`). What remains is `DRAW_VBO`, which needs the GLSL compiler
  retargeted to TGSI: G11 produces an AST and interprets it, and a TGSI back
  end is a phase in its own right.
- **GUI is educational.** The kernel compositor, GUI syscalls and `libauragui`
  are functional in tests, and windows are cleaned up on client exit, but it is
  not yet a protected multi-client production desktop.

---

## Future Enhancements

### Memory management

- [x] Strict per-segment user ELF permissions and NX for user data/stack.
- [x] Basic user pointer validation and safe copy helpers for syscall dispatch.
- [ ] Fault-recovering user access and continued audits for new syscall paths.
- [x] Copy-on-write `fork`.
- [x] User `mmap` / `munmap` / `brk` baseline syscalls (eager private mappings; true lazy/shared VMAs remain future work).
- [x] Slab allocator for common fixed-size kernel objects.
- [x] Guard pages around kernel/user stacks, with explicit overflow diagnosis in the `#PF` handler (`kernel/proc/guard.c`; kernel-stack hit is fatal, user-stack hit → SIGSEGV). Heap-region guard pages remain future work.
- [ ] Large-page support for selected kernel mappings.
- [x] `paging_free_address_space()` walker (user half) with PMM accounting.
- [ ] Wire it on for all reaped zombies once TLB shootdown + per-PML4 refcounting land.

### Scheduling and processes

- [x] SMP-aware scheduler baseline: CPU-local current/idle state, global scheduler lock, AP idle scheduler loop. Per-CPU run queues remain future work.
- [x] Deferred TCB/kernel-stack reaper and missed-wakeup-safe wait notifications.
- [x] Full address-space/page-table reaping via `paging_free_address_space()` in `thread_reap_zombies()`.
- [x] BLOCKED state, wait queues and sleepable kernel primitives baseline.
- [ ] Real parent/child process table and precise `waitpid` semantics.
- [x] Basic per-process FD tables.
- [x] `dup`, `dup2`, `pipe`, `fcntl(F_GETFD/F_SETFD/FD_CLOEXEC)` syscalls + `execve` honouring `FD_CLOEXEC`.
- [x] `waitpid(pid, *exit_code)` with real exit-code propagation and zombie collection on wait.
- [x] Precise POSIX shared-open-file description semantics across `fork`.
- [x] Signals and process notification baseline (`kill`, alarms, terminal signals, SIGCHLD, SA_RESTART).

### Filesystems and storage

- [ ] Broaden AHCI compatibility beyond the QEMU test path.
- [ ] Add fsck/recovery tooling or defensive consistency checks for FAT32/ext2.
- [x] Add baseline file timestamps for VFS stat plus tmpfs, diskfs, ext2 and FAT32. Remaining permission-mode persistence/audit across every experimental FS stays future work.
- [ ] Add symbolic links and richer path handling where useful.
- [ ] Add block cache and writeback policy instead of direct synchronous writes.
- [ ] Add virtio-blk / virtio-scsi or NVMe as modern virtual storage targets.

### Networking

- [x] Interrupt-capable e1000 RX/TX driver core (INTx, RX software queue, wait queues).
- [x] Rewire TCP receive waits to timed IRQ-backed NIC waits.
- [x] Rewire ARP/DHCP/ICMP and kernel UDP/DNS boot paths to bounded IRQ-backed NIC waits.
- [x] Add AF_INET/SOCK_DGRAM user sockets with `sendto(44)` / `recvfrom(45)`.
- [ ] Make remaining socket edge cases fully blocking.
- [x] Process-owned socket-style client handles (`socket/connect/send/recv/close`).
- [x] Per-connection TCP state (`tcp_handle_t`, up to `TCP_MAX_CONNS=8`).  Legacy `SYS_NET_*` syscalls are now a thin shim over the per-connection layer and are formally **deprecated**.
- [x] Full BSD socket ABI baseline including `sockaddr`, `bind`, `listen` and `accept` for AF_INET/SOCK_STREAM.
- [x] UDP user sockets.
- [x] Basic one-segment TCP retransmission and fixed RTO for SYN/data/FIN. Better packet queues, congestion control and sliding windows remain future work.
- [x] netdev NIC abstraction with boot-time backend selection (e1000 default, virtio-net fallback).
- [x] virtio-net modern data-path driver (RX/TX virtqueues, 12-byte hdr, MAC from device cfg).
- [ ] virtio-net IRQ-driven RX (currently polling); vmxnet3 / e1000e data-path drivers.

### USB and wireless

- [x] Add stable OHCI/EHCI/xHCI control/bulk backend API hooks into `usb_core`.
- [ ] Complete OHCI ED/TD transfer scheduling.
- [ ] Complete EHCI async/control/bulk qTD transfers and MSC backend.
- [ ] Complete xHCI command/event/transfer rings, slot addressing and endpoint contexts.
- [x] USB HID keyboard/mouse class drivers for UHCI Boot Protocol devices.
- [ ] Generic HID report parsing and OHCI/EHCI/xHCI HID transport.
- [ ] Real Bluetooth USB transport and at least one tested HCI controller path.
- [ ] Real Wi-Fi chipset driver backend for the existing 802.11 MAC layer.

### GUI and userspace

- [x] GUI dirty-rect compositor partial redraw (`compositor_render_dirty()` + `gfx_flip_rect()`).
- [x] Clean up GUI windows when the owning process exits.
- [x] Basic GUI process ownership enforcement for user-facing window syscalls.
- [x] Audit every GUI sub-op for out-of-range/negative wid and bad userspace pointers (with integration test).
- [ ] Stronger compositor/client isolation and permission model for GUI internals.
- [ ] More complete text input, clipboard and focus behavior.
- [ ] Persisted user settings/theme.
- [x] USB Manager GUI (`/gusb`) wired to `/usb` hotplug/storage status.
- [ ] More GUI apps and richer file editor/terminal behavior.
- [ ] Dynamic user-space allocation once `brk`/`mmap` exist.

### Infrastructure and docs

- [x] Sync `docs/status.md`, `TODO.md`, and `CHANGELOG.md` for completed H1–H8 hardening work.
- [ ] Keep `README.md`, `docs/status.md`, `docs/syscall_abi.md` and
      `docs/driver_guide.md` in sync with every future feature change.
- [ ] Add GDB helper scripts / pretty-printers for kernel structures.
- [ ] Reduce integration-test timing flakiness around process spawn and serial
      input pacing.
- [x] Add fsck-style FAT32/ext2 churn + reboot regression test case (`test_fs_stress.sh`).
- [x] Add integration cases for GUI bad-pointer hardening, process-exit GUI cleanup, FD lifecycle.
- [ ] Add CI artifacts for screenshots and QEMU serial logs on every failure.
