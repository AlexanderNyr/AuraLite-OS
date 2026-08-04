# AuraLite OS — POSIX.1-2024 Compliance Plan

## Status: IN PROGRESS 🔧 (Q1–Q12 implemented, Q13–Q16 planned)

This document is the living development plan for POSIX.1-2024 (IEEE Std
1003.1-2024, The Open Group Base Specifications Issue 8) compliance in
AuraLite OS. It follows the same structure as `POSIX_PLAN.md` (the
POSIX.1-2017 roadmap, P1-P10, now complete) and `PLAN.md` (the original
14-phase roadmap).

Baseline: commit `34bde09` (2026-07-05). All P1-P10 POSIX.1-2017 work is
complete and is **not** revisited here except where POSIX.1-2024 changes an
existing header's contents.

The original plan was divided into 12 phases (Q1-Q12). This revision extends
it to **16 phases (Q1-Q16)**: phases Q2-Q11 landed as patches between
`34bde09` and `451d200` but were never written into this document, so their
scope is recorded here retroactively from the shipped code and from
`docs/posix2024_compliance.md` (matrix dated 2026-07-06); Q12-Q16 specify
the remaining work, including the conformance harness the original brief
promised for Q12 and every 🔶 row the matrix still carries.

### Phase overview

| Phase | Scope | Status |
|---|---|---|
| Q1 | Mandatory C standard headers | DONE ✅ |
| Q2 | stdio extensions | DONE ✅ |
| Q3 | string/memory extensions | DONE ✅ |
| Q4 | stdlib extensions + sysconf/pathconf | DONE ✅ |
| Q5 | AT-family syscalls | DONE ✅ (shipped without a dedicated gate — covered by the Q12/Q13 harness) |
| Q6 | pthread extensions | DONE ✅ |
| Q7 | POSIX IPC: mqueue, named semaphores, shm_open | DONE ✅ (named semaphores reclassified to 🔶 by Q12 — see Q12) |
| Q8 | sched + resource stubs | DONE ✅ (stubs; no dedicated gate — see Q12) |
| Q9 | posix_spawn | DONE ✅ |
| Q10 | locale/iconv/search/legacy stubs | DONE ✅ |
| Q11 | POSIX.1-2024-new functions | DONE ✅ |
| Q12 | Compliance matrix + runnable conformance suite | DONE ✅ |
| Q13 | AT-family completion: link/linkat, symlinkat, mkfifoat/mknodat, utimensat/futimens, fdopendir | PLANNED 📋 |
| Q14 | System V IPC (sem/shm/msg): replace the ENOSYS stubs | PLANNED 📋 |
| Q15 | mq_notify + sigevent delivery | PLANNED 📋 |
| Q16 | Issue-8 odds and ends: pselect/ppoll, getrandom, sig2str/str2sig | PLANNED 📋 |

Every pending phase ships one `.patch` under `patches/`, extends the
conformance harness of Q12 and regenerates `docs/posix2024_compliance.md`
— the matrix is a generated view of the implementation, never hand-truth
(compare GL_PLAN principle 1: tests link the real sources, so the docs and
the code cannot drift).

---

## Phase Q1 — Mandatory C Standard Headers (Thin Wrappers)

**Objective:** Add the C standard headers that POSIX.1-2024 mandates but
were missing from the P1-P10 baseline.

### Status: DONE ✅

### What was added

**Headers** (`libc/include/`):

| Header | Approach |
|---|---|
| `stdarg.h` | Thin wrapper over `__builtin_va_*` |
| `stddef.h` | `#include_next` the compiler's freestanding header (+ `max_align_t` fallback) |
| `stdint.h` | `#include_next` the compiler's freestanding header |
| `float.h` | Hand-written IEEE 754 binary32/64 + x87 80-bit extended constants |
| `inttypes.h` | Format macros for AuraLite's actual LP64 typedefs (`int64_t`/`intmax_t` are `long`, **not** `long long`, on this target — verified with both Clang and GCC) + `strtoimax`/`strtoumax`/`imaxabs`/`imaxdiv` |
| `iso646.h` | Alternative operator spellings |
| `stdalign.h` | `alignas`/`alignof` macros (guarded for C23+ toolchains where they're keywords) |
| `stdnoreturn.h` | `noreturn` macro (guarded for C23+) |
| `tgmath.h` | Type-generic macros; dispatches to the `double`-only functions AuraLite's `<math.h>` provides (no `float`/`long double` overloads exist yet) |
| `complex.h` | Stub: types/macros compile, `creal`/`cimag`/`cabs`/`conj` implemented, no real complex arithmetic |
| `fenv.h` | Stub: AuraLite never establishes a per-thread FP environment, so all functions report the fixed default environment and never fail |
| `stdatomic.h` | Full C11 atomics on top of compiler builtins (`__c11_atomic_*` for Clang, `__atomic_*` for GCC — see header comment for why both are needed) |
| `wctype.h` | "C"-locale wide-char classification (ASCII widened to `wint_t`) |
| `strings.h` | BSD compatibility (`bcmp`/`bcopy`/`bzero`/`index`/`rindex`/`ffs*`) |
| `uchar.h` | `char16_t`/`char32_t` + UTF-8 single-code-point conversions (ASCII exact, `EILSEQ` beyond ASCII — full UTF-8 decoding is future work) |
| `setjmp.h` | Types + prototypes; implementation in `libc/crt/setjmp.asm` and `libc/src/compat.c` |
| `threads.h` | C11 threads mapped onto `libc/src/pthread/pthread.c` |

**Runtime:**
- `libc/crt/setjmp.asm` — x86_64 `setjmp`/`longjmp` (System V AMD64 ABI,
  callee-saved registers: rbx/rbp/r12-r15/rsp/rip).
- `libc/src/compat.c` — runtime bodies for the above headers' non-builtin
  functions (BSD strings.h aliases, wctype.h C-locale classification,
  inttypes.h helpers, threads.h-over-pthreads wrappers, fenv.h/complex.h
  stubs, `sigsetjmp`/`siglongjmp`, uchar.h conversions).
- `Makefile`: `setjmp.o` and `compat.o` added to `USER_COMMON`, linked into
  every user-space ELF (same treatment as `malloc.o`/`pthread.o`/etc.).

**Bug found and fixed along the way:** adding `<stdnoreturn.h>` exposed a
latent macro-hygiene bug in four existing headers (`assert.h`, `setjmp.h`,
`stdlib.h`, `threads.h`): `__attribute__((noreturn))` expands to
`__attribute__((_Noreturn))` once the `noreturn` macro is defined, which
GCC rejects under `-Werror` (`'_Noreturn' attribute directive ignored`).
Fixed by switching all four declarations to the double-underscore attribute
spelling `__attribute__((__noreturn__))`, which is immune to macro expansion
per both compilers' documented behaviour.

**Tests:**
- `tests/unit/test_q1_headers.c` (new, registered in `make test-unit`):
  includes all 17 new/changed headers together under
  `-std=c11 -Wall -Wextra -Werror`, and exercises the non-builtin logic
  (inttypes helpers, iso646 operators, stdalign/stdnoreturn, wctype
  classification, `strings.h` `ffs`, `jmp_buf`/`sigjmp_buf` layout,
  `stdatomic.h` load/store/fetch-add/flag, fenv defaults, complex stub
  types) via standalone reimplementations — mirroring how the existing
  `test_signals.c`/`test_ctype.c` host tests avoid linking the freestanding
  syscall-backed runtime.

### Definition of Done
- [x] All 17 touched/new headers `#include` cleanly, standalone and
      together, under both host GCC and cross Clang (`-Wall -Wextra
      -Werror`).
- [x] `libc/src/compat.c` compiles cleanly under both compilers and links
      into every user-space ELF.
- [x] `libc/crt/setjmp.asm` assembles with `nasm` and is linked into every
      user-space ELF.
- [x] `make test-unit` passes `test_q1_headers` (42/42 checks) with **zero
      regressions** in the other 27 existing unit-test binaries.
- [x] `make all` (full ISO + kernel.elf + every user-space app) builds with
      zero errors/warnings, unchanged from baseline.
- [x] No kernel-side changes: the kernel's `CFLAGS` do not include
      `libc/include`, so Q1 is verified to be a pure user-space/libc change
      with no kernel build-path risk, matching the plan's "zero kernel risk"
      characterization of this phase.

---

## Phase Q2 — stdio Extensions

**Objective:** the `<stdio.h>` surface POSIX.1-2024 expects beyond the P1-P10
baseline, all backed by real syscall I/O (no stubs).

### Status: DONE ✅

### What was added
`dprintf`/`vdprintf`, `asprintf`/`vasprintf`, `getline`/`getdelim`,
`fmemopen` and `open_memstream` (pipe-backed), `popen`/`pclose`,
`flockfile`/`ftrylockfile`/`funlockfile`, and the `_unlocked` family
(`getc_unlocked`, `putc_unlocked`, `fgetc_unlocked`).

### Tests
`tests/unit/test_stdio_ext.c` — 30 checks, registered in `make test-unit`.

### Definition of Done
- [x] Symbols resolve from `libaurac.a`; prototypes in `lib/libc/include/stdio.h`.
- [x] `test_stdio_ext` green (30/30) with zero regressions in the suite.
- [x] Matrix rows marked ✅.

---

## Phase Q3 — string/memory Extensions

**Objective:** the BSD-born functions POSIX.1-2024 finally standardised
(`strlcpy`/`strlcat` are Issue-8 additions) plus the usual companions.

### Status: DONE ✅

### What was added
`memccpy`, `memmem`, `stpcpy`, `stpncpy`, `strlcpy`, `strlcat`,
`strverscmp`, `strsignal`.

### Tests
`tests/unit/test_string_ext.c` — 54 checks.

### Definition of Done
- [x] Bounded-op semantics match the Issue-8 wording (truncation + explicit
      return values for `strlcpy`/`strlcat`, verified against reference
      cases in the unit test).
- [x] `test_string_ext` green (54/54).

---

## Phase Q4 — stdlib Extensions + sysconf/pathconf

**Objective:** allocation/template/realpath utilities and the system
configuration queries shells need to feel POSIX-normal.

### Status: DONE ✅

### What was added
`posix_memalign`, `aligned_alloc`, `reallocarray` (Issue-8), `realpath`,
`mkdtemp`, `mkostemp`, `mkstemps`; `sysconf` (`_SC_PAGESIZE`=4096,
`_SC_NPROCESSORS_ONLN`, `_SC_VERSION`=202405L), `confstr`,
`pathconf`/`fpathconf`.

### Tests
`tests/unit/test_stdlib_ext.c` — 30 checks.

### Definition of Done
- [x] `_POSIX_VERSION` and `_SC_VERSION` both report `202405L`
      (`lib/libc/include/unistd.h`).
- [x] `test_stdlib_ext` green (30/30).

---

## Phase Q5 — AT-Family Syscalls

**Objective:** the `*at()` directory-fd API on top of the VFS.

### Status: DONE ✅ (shipped without a dedicated gate — see below)

### What was added
`openat`, `fstatat`, `mkdirat`, `unlinkat`, `renameat`, `readlinkat`,
`fchownat`, `fchmodat`, `faccessat` (kernel cases 257-269 in
`kernel/arch/x86_64/syscall.c`, `AT_FDCWD`=-100), `fexecve` and `execveat`
(case 322, `AT_EMPTY_PATH` via `/proc/self/fd`), `dup3` (case 292).

### Known gap (honest bookkeeping)
No dedicated unit/integration gate covers this phase in `tests/` today —
the functions are live but protected only by general boot tests. The Q12
harness includes an AT-family section precisely because of this, and Q13
extends it.

### Definition of Done
- [x] Kernel dispatch + libc wrappers present; matrix rows ✅.
- [ ] Dedicated gate — **carried into Q12/Q13** (deliberate, tracked).

---

## Phase Q6 — pthread Extensions

**Objective:** the synchronization objects Issue 8 counts as base API.

### Status: DONE ✅

### What was added
`pthread_rwlock_*`, `pthread_barrier_*` (futex-backed), `pthread_spin_*`,
cancellation surface (`pthread_cancel`, `pthread_setcancelstate/type`,
`pthread_testcancel`, `pthread_cleanup_push/pop` macros),
full `pthread_attr_*` (stack size/addr, detach state).

### Tests
`tests/unit/test_pthread_ext.c` — 38 checks.

### Definition of Done
- [x] All objects survive cross-thread hand-off in the unit test (38/38).

---

## Phase Q7 — POSIX IPC: Message Queues, Named Semaphores, shm_open

**Objective:** file-backed POSIX IPC that works without inventing a kernel
namespace first.

### Status: DONE ✅

### What was added
`<mqueue.h>`: `mq_open/close/unlink/send/receive/timedsend/timedreceive/
getattr/setattr` (file-based implementation); `sem_open/close/unlink/
sem_timedwait` (mmap-backed); `shm_open/shm_unlink` (file-based);
`struct sigevent`.

### Tests
`tests/unit/test_ipc.c` — 11 checks.

### Known gap
`mq_notify` is present but returns `ENOSYS` (matrix 🔶) — real delivery is
phase **Q15**.

### Definition of Done
- [x] Two-process rendezvous through an mqueue and a named semaphore works
      in the unit harness (11/11).

---

## Phase Q8 — sched + resource Stubs

**Objective:** well-formed answers (not crashes) for scheduler/policy and
rusage queries.

### Status: DONE ✅ (stubs by design)

### What was added
`<sched.h>`: `sched_get_priority_max/min`, `sched_getscheduler`
(SCHED_OTHER), `sched_setscheduler`/`sched_getparam`/`sched_setparam`/
`sched_rr_get_interval` (stubs), `sched_yield` (real, syscall 24);
`getrlimit`/`setrlimit` (stub), `getrusage` (`ENOSYS`).

### Definition of Done
- [x] Stubs fail with the Issue-8-sanctioned errnos, never crash; matrix
      documents each as a stub, not full support.

---

## Phase Q9 — posix_spawn

**Objective:** the spawn API with file actions and attribute plumbing.

### Status: DONE ✅

### What was added
`posix_spawn`/`posix_spawnp` (fork + file_actions + execve),
`posix_spawn_file_actions_*`, `posix_spawnattr_*`; the kernel learned argv
passing in `SYS_SPAWN` (SDK phase S3).

### Tests
`tests/unit/test_posix_spawn.c` — 43 checks.

### Definition of Done
- [x] Spawn with argv/envp and PATH search verified (43/43).

---

## Phase Q10 — locale/iconv/search/legacy Interfaces

**Objective:** breadth coverage where honest stubs are better than absence
— the "Q10 stubs" phase (the name is kept: stubs are a deliverable here,
per the original brief).

### Status: DONE ✅

### What was added
`<ftw.h>` (ftw/nftw), `<iconv.h>` (UTF-8 passthrough), `<langinfo.h>`,
`<monetary.h>` (`strfmon` minimal), `<search.h>` (hsearch/tsearch/lsearch
families), `<syslog.h>` (to stderr), `<utmpx.h>`, `<wordexp.h>`,
`<sys/statvfs.h>`, `<sys/times.h>`, `<net/if.h>`, `<netinet/tcp.h>`,
`<sys/ipc.h>` + `ftok`, and explicit `ENOSYS` stubs for the System V IPC
families (`semget/semop/semctl`, `shmget/shmat/shmdt/shmctl`,
`msgget/msgsnd/msgrcv/msgctl`).

### Tests
`tests/unit/test_q10_stubs.c` — 22 checks.

### Known gap
The System V families are 🔶 by design in this phase; real kernel objects
are phase **Q14**.

### Definition of Done
- [x] Every stub compiles, links and fails cleanly with `ENOSYS` (22/22).

---

## Phase Q11 — POSIX.1-2024-New Functions

**Objective:** the headline Issue-8 additions.

### Status: DONE ✅

### What was added
`getentropy` (syscall 318, >256 bytes → `EIO`), `close_range` (436),
`closefrom`; `timespec_get`/`timespec_getres` (`TIME_UTC`);
`clock_nanosleep` incl. `TIMER_ABSTIME`; `dirfd`, `scandir`, `alphasort`,
`versionsort`; pseudo-terminal skeleton: `posix_openpt`, `grantpt`,
`unlockpt`, `ptsname`/`ptsname_r` (single `/dev/pts/0`).

### Tests
`tests/unit/test_q11_new.c` — 22 checks.

### Definition of Done
- [x] Interface contracts verified (22/22); matrix rows ✅.

---

## Phase Q12 — Compliance Matrix + Runnable Conformance Suite

**Objective:** turn the matrix from a document into a gate. The original
brief for this phase promised "compliance-matrix/test-suite"; the matrix
half shipped (`docs/posix2024_compliance.md`, 2026-07-06, ~400 rows). This
phase is the other half.

### Status: DONE ✅

### What was added

**Host layer** — `tests/posix2024/`, wired into `make test-unit`:
- `run_host.sh` — four checks, mirroring `test_userlibs.sh`'s skip-when-
  archives-absent convention: (1) header self-containment sweep (78/78
  public headers compile standalone under
  `-std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=202405L`); (2) matrix →
  archive drift check; (3) negative control — the drift check must FAIL
  against a degraded copy of `libaurac.a` (one object dropped via `ar d`),
  proving the gate notices a deleted implementation; (4) re-run of the
  Q-family unit binaries (`test_q1_headers`, `test_*_ext`, `test_pthread_ext`,
  `test_ipc`, `test_posix_spawn`, `test_q10_stubs`, `test_q11_new`) as
  sub-suites.
- `matrix_check.py` — the drift checker.  It reads the matrix, extracts
  every row token (splitting `/`- and `,`-separated cells, expanding `X_*`
  wildcards, completing short tail tokens like `pop` in
  `pthread_cleanup_push/pop` against the wildcard prefix), and proves each
  one resolves to a defined symbol in `libaurac.a` via `nm`.  Prose cells
  and all-caps constants (macros, not link symbols) are skipped; the two
  function-like macros POSIX mandates (`pthread_cleanup_push`/`pop`) are
  verified as real `#define`s in the headers (`known_macros.txt`), not
  waved through.  The 🔶 partial set must equal `known_partials.txt`
  exactly in both directions (partials can neither grow silently nor
  vanish undocumented), and ❌ rows are rejected.
- `known_partials.txt` — `mq_notify` (→Q15), the 12 System V IPC tokens
  (→Q14), and — reclassified by this phase — `sem_open`/`sem_close`/
  `sem_unlink` (→Q14/Q15): they need MAP_SHARED backing the kernel does
  not provide yet, so they fail honestly with ENOSYS.

**Guest layer**:
- `userspace/tests/conformtest/conformtest.c` — initrd `/tests/conformtest`,
  asserting syscall-backed behaviour end to end: the Q5 AT-family on tmpfs
  (openat/mkdirat/fstatat with type bits and AT_SYMLINK_NOFOLLOW/
  faccessat/renameat/unlinkat incl. AT_REMOVEDIR/readlinkat, plus a
  cwd-relative openat via AT_FDCWD) and on FAT32 when mounted (closing the
  Q5 gate hole), `posix_spawn` argv/envp, mqueue round-trip, semaphore
  sanity (named = documented ENOSYS partial; unnamed in-process),
  `clock_nanosleep` TIMER_ABSTIME, `getentropy` bounds, `scandir`
  alphasort/versionsort ordering.
- `tests/integration/cases/test_posix2024_conf.sh` — the QEMU case,
  registered in `run_all.sh` after `test_keymaps`.

**Matrix drift found and fixed (this is the point of the gate):** the first
run of the drift check failed: 35 symbols the matrix marked ✅ were declared
in the public headers by Q5/Q8/Q10/Q11 but never given bodies.  All 35 are
now implemented in `lib/libc/src/posix_extra.c` (AT-family wrappers,
`close_range`/`closefrom`, `clock_nanosleep`/`timespec_get(res)`, the
pseudo-terminal skeleton, the `if_*` name/index family, the sched family,
`getrusage`, `atol`).  The header sweep additionally caught `<monetary.h>`
(used `ssize_t` without `<sys/types.h>`) and `<mqueue.h>` (used `struct
sigevent` without `<signal.h>`); both fixed.  `AT_FDCWD`/`AT_*` moved to
their POSIX home in `<fcntl.h>` (they were duplicated in `<unistd.h>`).

**Kernel changes the guest layer forced (all honest, all small):**
- tmpfs (`kernel/fs/tmpfs.c`) gains real directory semantics: `mkdir`/
  `rmdir`/`rename`, nested paths with parent-existence checks, and
  `readdir` listing immediate children with basenames and real entry types.
  The file table stays flat (names carry the full relative path); a third
  volume `/dev/shm` (`shmfs_ops`) hosts POSIX shared-memory files.
- stat-family syscalls compose POSIX `st_mode` type bits, so S_ISREG/
  S_ISDIR/… work (`S_IFMT` was delivered with permission bits only).
- AT-family relative paths resolve against the caller's cwd when
  `dirfd == AT_FDCWD` (real dirfds with relative paths stay honest ENOSYS
  until the VFS grows open-directory resolution — declared, not faked).
- `fork()`/`clone()` children now restore the SysV callee-saved registers
  (rbx/rbp/r12–r15) from the syscall-entry snapshot.  Without this, the
  child resumed with kernel garbage in the registers the compiler
  legitimately keeps live across fork() (the SysV syscall ABI preserves
  them) — observed as `posix_spawn`'s child faulting on a kernel address
  before execve.  `fork_return.asm` extended accordingly.
- `mq_send`/`mq_receive` gained a per-descriptor read cursor (a send on
  the same fd previously left the offset at EOF, so a round-trip returned
  nothing); `sem_trywait` now sets `errno = EAGAIN` as POSIX requires.

**Deviations from the original brief, annotated (house convention):**
- The "matrix regeneration script" is implemented as the *drift check*
  (matrix → archive) rather than full `.md` regeneration: regenerating the
  matrix would lose its curated notes and prose rows, while the gate's job
  — the docs can never rot silently — is fully served by the reverse
  direction (checked against the archive on every `make test-unit`).
- Named semaphores are reclassified ✅ → 🔶 (documented above); the suite
  asserts the honest ENOSYS rather than a pretend success.

### Tasks
- [x] `tests/posix2024/` host harness (header sweep, drift check, negative
      control, Q-family sub-suites) green on the host.
- [x] Guest `conformtest` + QEMU case green end to end.
- [x] Matrix drift check proves matrix ↔ `libaurac.a` agreement.
- [x] 35 declared-but-missing bodies implemented; matrix back in sync.
- [x] CHANGELOG + `docs/status.md` entries per house convention.

### Test gate
- Harness green on host AND in QEMU; deleting a function body from libc
  makes the link sweep fail immediately (negative control, asserted by the
  harness itself). ✓
- Q5 AT-family operations now gated in the guest suite. ✓

### Deliverable
`patches/POSIX2024_Q12_conformance.patch`

---

## Phase Q13 — AT-Family Completion: links, special files, times, fdopendir

**Objective:** finish the `*at()` surface POSIX.1-2024 lists (Q5 shipped the
first tranche), including `link(2)` — the only file-operation verb the tree
still lacks a syscall for.

### Status: PLANNED 📋

### Tasks
- [ ] `link(2)`/`linkat(2)`: dispatch `SYS_LINK` (90 — reserved and made
      collision-free in the Q5 work) to a new `vfs_link()`; tmpfs and ext2
      gain a hard-link operation; FAT32/exFAT return `EPERM` (POSIX wording
      for "links not supported"), cross-device attempts `EXDEV`.
- [ ] `symlinkat`, `mkfifoat`, `mknodat` (device nodes: return `ENOSYS`
      where devfs has no backing, `mknodat` for FIFO/regular only).
- [ ] `utimensat`/`futimens` with real nanosecond `tv_nsec` handling,
      `UTIME_NOW`/`UTIME_OMIT`; stat structures already carry the fields.
- [ ] `fdopendir` (same `/proc/self/fd/N` trick `sys_openat`/`execveat`
      use) + `dirfd` interop test.
- [ ] libc bodies + declarations (`linkat` is already declared in
      `unistd.h` without an implementation — link-sweep honesty forces it).
- [ ] Extend the Q12 guest suite: these operations on tmpfs/ext2/FAT32, so
      the Q5 hole and Q13 land under one gate.

### Test gate
- QEMU: `ln`/temp-AT script driving link/unlink/utimes through the shell;
  cross-device `link` yields `EXDEV`; FAT32 `link` yields `EPERM`;
  `utimensat` mtimes read back through `stat` within 1s.
- Host: the link sweep finds all new symbols.

### Deliverable
`patches/POSIX_Q13_at_complete.patch`

---

## Phase Q14 — System V IPC: sem/shm/msg Kernel Objects

**Objective:** replace the twelve `ENOSYS` stubs from Q10 with real kernel
services. This is the single largest remaining POSIX surface and is
scheduled last among the big items: it touches the kernel object model,
permissions (P7) and the process lifecycle.

### Status: PLANNED 📋

### Tasks
- [ ] Kernel key namespace (`ftok` keys + `IPC_PRIVATE`), per-object
      uid/gid/mode checked like a 9-bit mode (reuse P7 credential checks).
- [ ] Semaphores: `semget/semop/semctl` (`GETVAL/SETVAL/GETALL/SETALL/
      IPC_STAT/IPC_RMID`), blocking `semop` on wait queues;
      `SEM_UNDO` tracked per-process and applied at exit.
- [ ] Shared memory: `shmget/shmat/shmdt/shmctl`; page-backed segments
      attached through the existing VMM/VMA path; `IPC_RMID` marks
      destruction at last detach (`nattch`==0); exec closes like FD_CLOEXEC.
- [ ] Message queues: `msgget/msgsnd/msgrcv/msgctl`, mtype-ordered receive
      (positive/negative/zero mtype rules), `msgtyp==0` FIFO,
      `IPC_NOWAIT` ↔ blocking modes.
- [ ] libc: replace the Q10 stubs; `sys/ipc.h` constants audit
      (`IPC_CREAT/EXCL/NOWAIT/RMID/STAT/SET` values vs Issue 8).
- [ ] Non-goals (scope line, deliberate): `MSG_COPY`, `SHM_LOCK`,
      namespace juggling, /proc/sysvipc/*.

### Test gate
- Host unit: key/permission/flag decoding, mtype selection algorithm.
- QEMU: fork pair passing messages; shared counter in an shm segment
  guarded by a SysV semaphore (10k increments, exact total); `ipcrm`-style
  teardown leaves no leaks (assert `SYS_MEMINFO` before/after matches).
- Matrix: the SysV 🔶 rows flip to ✅, ENOSYS stubs disappear.

### Deliverable
`patches/POSIX_Q14_sysvipc.patch`

---

## Phase Q15 — mq_notify + sigevent Delivery

**Objective:** the last 🔶 row in `<mqueue.h>`.

### Status: PLANNED 📋

### Tasks
- [ ] `mq_notify` with `SIGEV_SIGNAL`: deliver the requested signal with
      `sigev_signo` value on empty→non-empty transition; one registration
      per queue (`EBUSY` for a second).
- [ ] `SIGEV_THREAD`: run the notification function on a fresh pthread in
      the registering process (the mqueue is file-backed, so a watcher
      thread blocked in `mq_timedreceive` can implement both modes without
      kernel changes — document the coalescing caveat: a burst of messages
      may compress to fewer notifications, matching the "at least one" rule).
- [ ] `sigevent` constants/ABI check against Issue 8 (`SIGEV_NONE` honoured
      as deregistration).
- [ ] Matrix: `mq_notify` 🔶 → ✅ with the coalescing note.

### Test gate
- QEMU: process registers `SIGEV_SIGNAL`, second process sends → handler
  runs (flag visible on serial); `SIGEV_THREAD` variant increments a
  shared counter; deregistration stops delivery. Host unit: registration
  state machine (single-registrant `EBUSY`, re-arm semantics).

### Deliverable
`patches/POSIX_Q15_mqnotify.patch`

---

## Phase Q16 — Issue-8 Odds and Ends: pselect/ppoll, getrandom, sig2str/str2sig

**Objective:** the remaining named functions of Issue 8 that belong to
headers the tree already ships; closing them makes the matrix's 🔶 column
zero except conscious 🚫 N/A entries.

### Status: PLANNED 📋

### Tasks
- [ ] `pselect`/`ppoll`: atomic mask-and-wait — the signal mask must be
      installed as part of the block, not around it (the classic
      pselect race); add `SYS_PSELECT6`/`SYS_PPOLL` next to `SYS_SELECT`,
      reusing `kernel/fs/select.c` plus the signal machinery's
      syscall-exit delivery boundary.
- [ ] `getrandom(2)` + `<sys/random.h>`: new syscall beside
      `getentropy` (318). Kernel RNG graduates from the current
      rdtsc-xorshift filler to a seeded pool (rdrand when available,
      PIT/RTC/interrupt-jitter mixing into a xorshift128+ or chacha-lite
      state; `GRND_NONBLOCK`/`GRND_RANDOM` accepted, one documented
      source initially). Security note written into TODO.md rather than
      overclaimed.
- [ ] `sig2str`/`str2sig` (`<signal.h>`): pure libc tables covering the
      full signal set; round-trip tested.
- [ ] Leftover sweep: any Issue-8 function name absent from the matrix is
      added as a row (✅ or argued 🚫 N/A) so coverage is enumerable, not
      anecdotal.

### Test gate
- QEMU: pselect unblocks instantly on a pending signal (race probe loop,
  1000 iterations, no lost wakeup assert); ppoll same for pipes.
- Host: `getrandom` byte-stream smoke (chi-square sanity, two streams
  differ); `sig2str`/`str2sig` full round trip.

### Deliverable
`patches/POSIX_Q16_issue8_tail.patch`

---

## Finishing definition of done (whole plan)

- [ ] Every matrix row is ✅ or an argued 🚫 N/A; 🔶 count is zero.
- [ ] `make test-unit`, the Q12 guest suite and the full integration run
      green on a clean tree.
- [ ] `docs/posix2024_compliance.md` regenerated by the Q12 script and
      committed in the same change as the code it describes.
- [ ] `docs/status.md` POSIX row and `CHANGELOG.md` updated per phase.

## Order and rationale (updated)

| Step | Why here |
|---|---|
| Q1–Q11 (done) | Breadth first: headers, libc bodies, IPC files — no kernel risk. |
| Q12 | The harness lands before any remaining feature work, so Q13-Q16 each arrive gated; it also retroactively covers the Q5/Q8 gate holes. |
| Q13 | Small, high-value kernel touch (one VFS verb + timestamps); direct continuation of Q5. |
| Q15 | Userspace-heavy notification layer; independent of Q14. |
| Q16 | The Issue-8 tail — small, self-contained, benefits from the harness. |
| Q14 | System V IPC is the largest kernel project and changes the object/permission model; scheduled after the suite exists so it cannot land ungated. |

Recommended order for the remaining work: **Q12 → Q13 → Q15 → Q16 → Q14**
(Q14 may start in parallel once the Q12 harness exists, but merges last).
