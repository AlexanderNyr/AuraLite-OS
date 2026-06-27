# AuraLite OS — POSIX Compliance Plan

## Status: POSIX IMPLEMENTATION IN PROGRESS 🔧

This document is the living development plan for full POSIX.1-2017 compliance
in AuraLite OS. It follows the same structure as `PLAN.md` (the original 14-phase
roadmap that is now complete).

The baseline was analysed from the current source tree on 2026-06-27. Every gap
listed below was identified by reading the actual headers, syscall dispatch table,
`vfs.c`, `thread.h`, `scheduler.h`, `libc/`, and `TODO.md`.

---

## Baseline: What Already Exists

Before tracking what is missing it is important to know what POSIX-relevant
infrastructure is **already present** and only needs hardening:

| Component | Current State | POSIX Gap |
|---|---|---|
| `read` / `write` | Implemented, user-validated | Missing `O_NONBLOCK`, `EINTR` retry |
| `open` / `close` | Open takes only path (no flags/mode) | Missing `O_CREAT`, `O_RDWR`, `O_WRONLY`, `mode_t` |
| `fork` / `execve` | Simplified but functional | Missing COW, FD sharing semantics |
| `wait4` / `waitpid` | Implemented with exit code | Missing `WNOHANG`, `WUNTRACED` |
| `mmap` / `munmap` | Implemented (anon + file-backed stub) | Missing `MAP_SHARED`, full file-backed |
| `brk` / `sbrk` | Implemented | Per-process tracking present |
| `dup` / `dup2` | Implemented | Need `dup3` |
| `pipe` | Implemented (ring buffer, ref-counted) | Missing `O_CLOEXEC` variant (`pipe2`) |
| `fcntl` | `F_GETFD` / `F_SETFD` / `FD_CLOEXEC` | Missing `F_GETFL`, `F_SETFL`, `F_DUPFD` |
| `stat` | `vfs_stat` struct, basic fields | Missing `uid`, `gid`, `dev`, `rdev` |
| `mkdir` / `rmdir` / `unlink` / `rename` | Implemented | Missing `symlink`, `link`, `chmod`, `chown` |
| VFS / FD table | Per-process, 64 FDs, `cloexec[]` | Missing shared open-file descriptions |
| Scheduler | Round-robin, preemptive | Missing `SCHED_FIFO`, `SCHED_RR`, priorities |
| Signals | **Absent** | Full signal subsystem needed |
| `errno` | **Absent** | Thread-local errno needed |
| `pthread` | **Absent** | POSIX threads needed |
| Semaphores | **Absent** | `sem_t` needed |
| `select` / `poll` | **Absent** | I/O multiplexing needed |
| `getenv` / `setenv` | **Absent** | Environment block needed |
| `time` / `clock_gettime` | **Absent** | POSIX clocks needed |
| `getcwd` / `chdir` | **Absent** | Working directory per-process needed |
| Terminal (TTY) | Serial polling only | Full `termios` needed |
| User/Group IDs | **Absent** | UID/GID/permissions needed |
| Shared memory | **Absent** | `shm_open` / `mmap MAP_SHARED` needed |
| `regex` / `fnmatch` | **Absent** | Standard pattern matching needed |
| Math library | **Absent** | `libm` subset needed |

---

## POSIX Phase Roadmap

The plan is divided into **10 POSIX phases** (P1–P10), ordered by dependency
and impact. Each phase has a definition of done, a risk table, and a task list
in the same format as the original roadmap.

---

## Phase P1 — `errno`, Error Codes & libc Foundations

**Objective:** Establish the POSIX error-reporting contract that every
subsequent phase depends on. Every syscall must return `-1` on failure and set
a thread-local `errno`. The libc must grow the standard headers that POSIX
programs expect.

### Status: DONE (host-verified; QEMU integration boot pending cross toolchain)

### Tasks

**Kernel side:**
- [x] Define all standard POSIX `errno` values in `kernel/lib/errno.h`:
  `EPERM=1`, `ENOENT=2`, `ESRCH=3`, `EINTR=4`, `EIO=5`, `ENXIO=6`,
  `E2BIG=7`, `ENOEXEC=8`, `EBADF=9`, `ECHILD=10`, `EAGAIN=11`,
  `ENOMEM=12`, `EACCES=13`, `EFAULT=14`, `EBUSY=16`, `EEXIST=17`,
  `EXDEV=18`, `ENODEV=19`, `ENOTDIR=20`, `EISDIR=21`, `EINVAL=22`,
  `ENFILE=23`, `EMFILE=24`, `ENOTTY=25`, `EFBIG=27`, `ENOSPC=28`,
  `ESPIPE=29`, `EROFS=30`, `EPIPE=32`, `EDOM=33`, `ERANGE=34`,
  `EDEADLK=35`, `ENAMETOOLONG=36`, `ENOLCK=37`, `ENOSYS=38`,
  `ENOTEMPTY=39`, `ELOOP=40`, `EWOULDBLOCK=EAGAIN`, `ENOMSG=42`,
  `EOVERFLOW=75`, `EILSEQ=84`, `ENOTSUP=95`, `EADDRINUSE=98`,
  `ECONNREFUSED=111`, `ETIMEDOUT=110`
- [x] Return proper negative errno codes from the syscall handlers in
  `syscall.c` instead of raw `-1` (dispatch-layer mapping via `vfs_errno()`;
  validation/copy faults → `-EFAULT`, unknown syscall → `-ENOSYS`).
- [x] Update `vfs.c` to return `-Exxx` natively (bad fd → `EBADF`, missing path
  → `ENOENT`, FD table full → `EMFILE`, not-a-dir → `ENOTDIR`, cross-mount
  rename → `EXDEV`, unsupported op → `ENOSYS`, etc.); a `vfs_wrap_err()` helper
  maps the FS drivers' still-generic `-1` to a sensible errno. `process.c`/
  drivers' native `-Exxx` returns remain a follow-up (see TODO.md).
- [x] Audit all syscall cases in `syscall.c` for correct error propagation.

**libc side:**
- [x] `libc/include/errno.h`: declare `errno` (via `__errno_location()`) and all
  `E*` constants.
- [x] `errno` storage in `libc/src/libc.c`: a simple global behind
  `__errno_location()`; upgraded to TLS once pthreads land (P9). (Kept in
  libc.c rather than a separate errno.c to match the single-`libc.o` build.)
- [x] Wrap every syscall return in the libc wrappers through `syscall_ret()`:
  decode the reserved errno band, set `errno`, return `-1` (POSIX convention).
- [x] `libc/include/string.h`: add `strerror(int errnum)`.
- [x] `strerror()` implemented in `libc/src/libc.c` (lookup table + fallback).
- [x] `libc/include/stdio.h`: add `perror(const char *s)`.
- [x] `perror()` implemented in `libc/src/libc.c` (writes "s: msg\n" to fd 2).

**New libc headers required:**
- [x] `libc/include/limits.h`: `PATH_MAX=4096`, `NAME_MAX=255`, `ARG_MAX=131072`,
  `OPEN_MAX=64`, `PIPE_BUF`, `NGROUPS_MAX`, plus all integer-type ranges
  (`INT_MIN/MAX`, `LONG_MIN/MAX`, `ULONG_MAX`, `LLONG_*`, `CHAR_BIT`, etc.).
- [x] `libc/include/stdint.h` / `libc/include/stdarg.h`: provided by the
  freestanding compiler headers (used via `<stdint.h>`/`<stdarg.h>`); no
  AuraLite copy needed.
- [x] `libc/include/stdbool.h`: `bool`, `true`, `false`.
- [x] `libc/include/assert.h`: `assert(expr)` — prints `file:line: func:
  Assertion ... failed` and calls `abort()`; honours `NDEBUG`. Backed by
  `__assert_fail()` + `abort()`/`exit()` in `libc.c`.
- [x] `libc/include/ctype.h`: `isalnum/isalpha/isblank/iscntrl/isdigit/isgraph/
  islower/isprint/ispunct/isspace/isupper/isxdigit/tolower/toupper`.
- [x] ctype implemented in `libc/src/libc.c` (C-locale ASIIC predicates; kept in
  libc.c rather than a separate ctype.c to match the single-`libc.o` build).
  Verified against the host `<ctype.h>` over the full ASCII range
  (`tests/unit/test_ctype.c`).
- [x] `libc/include/math.h`: `fabs`, `floor`, `ceil`, `sqrt`, `pow`, `exp`,
  `log`, `log2`, `sin`, `cos` + `M_PI`/`M_E`/`HUGE_VAL`/`NAN`/`INFINITY`.
- [x] math implemented in `libc/src/libc.c` (sqrt via SSE2 builtin; exp/log/
  sin/cos/pow via range-reduced series, accurate to ~1e-9 vs host libm).

**Testing:**
- [x] `tests/unit/test_errno.c`: host-side — verifies `E*` constants, Linux ABI
  values, POSIX aliases, and the in-band decode contract (incl. -4095/-4096
  boundary). Wired into `make test-unit`. PASSES.
- [x] `tests/integration/cases/test_errno.sh`: runs `/selftest`, which calls
  `open("/nonexistent")`, asserts `errno == ENOENT`, and prints `perror()`
  output on serial. Registered in `run_all.sh`. (Pending QEMU toolchain to
  execute the boot — see Definition of Done note.)

### Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Kernel returns raw -1 in many places | High | Grep audit + mechanical substitution |
| `errno` becomes stale after thread switch | Low (single-threaded now) | Mark as future work; fix in P6 (pthreads) |
| errno values diverge from Linux ABI | Medium | Cross-check against `<asm/errno.h>` |

### Definition of Done
- [x] `make test-unit` passes `test_errno.c` (verified on host: ALL PASS).
- [~] Integration test: `open("/no")` → errno=ENOENT, `perror()` prints
  `"open: No such file or directory"`. Logic verified on host (selftest +
  strerror/perror harness produce the exact string); the QEMU boot run is
  **pending the cross toolchain** (clang/ld.lld/nasm/xorriso/qemu) which is
  not installed in the current build environment. Run on a tooled host with:
  `make all && bash tests/integration/cases/test_errno.sh`.
- [~] No regressions: `make test-unit` green incl. existing `test_libc`
  (26/26); kernel `syscall.c` and `libc.c` pass `-Wall -Wextra -Werror`
  syntax checks. Full `make all && bash tests/integration/run_all.sh` pending
  toolchain.

---

## Phase P2 — POSIX `open(2)` Flags, File Modes & `fcntl(2)`

**Objective:** `open()` must accept `flags` (O_RDONLY, O_WRONLY, O_RDWR,
O_CREAT, O_EXCL, O_TRUNC, O_APPEND, O_NONBLOCK) and `mode` (permission bits).
`fcntl()` must implement the full standard command set.

### Status: DONE (host-verified; QEMU integration boot pending cross toolchain)

### Tasks

**Kernel — `open()` overhaul:**
- [x] Change `vfs_open(path)` → `vfs_open(path, int flags, int mode)`.
- [x] Add `O_*` flag constants to `kernel/fs/vfs.h`:

```c
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_CLOEXEC   0x80000
#define O_DIRECTORY 0x10000
```

- [x] Propagate `O_RDONLY` / `O_WRONLY` / `O_RDWR` into the `struct file`
  as an `access_mode` field; enforce in `vfs_read()` and `vfs_write()`.
- [x] Implement `O_CREAT`: if file does not exist and `O_CREAT` is set,
  call the filesystem's `create()` op. Return `ENOENT` if absent and
  `O_CREAT` not set.
- [x] Implement `O_EXCL`: if file exists and both `O_CREAT | O_EXCL` are set,
  return `EEXIST`.
- [x] Implement `O_TRUNC`: if file exists and `O_WRONLY | O_RDWR | O_TRUNC`
  are set, call `vfs_truncate(vn, 0)` before returning the fd.
- [x] Implement `O_APPEND`: track `append_mode` flag in `struct file`; on
  every `write()`, seek to end first.
- [x] Implement `O_NONBLOCK`: stored in `struct file`; `vfs_read()`/`vfs_write()` return
  `EAGAIN` instead of blocking when no data is available (pipes, devices).
- [x] Implement `O_CLOEXEC`: set `cloexec[fd] = 1` when the flag is present.
- [x] Update `SYS_OPEN` dispatch in `syscall.c` to pass `a2=flags`, `a3=mode`.
- [x] Update `creat()` = `open(path, O_CREAT|O_WRONLY|O_TRUNC, mode)`.

**Kernel — `fcntl()` expansion:**
- [x] `F_GETFL` (3): return current file status flags (`O_RDONLY`, `O_RDWR`,
  `O_APPEND`, `O_NONBLOCK`) from `struct file`.
- [x] `F_SETFL` (4): set `O_APPEND`, `O_NONBLOCK` on an open file.
- [x] `F_DUPFD` (0): find lowest fd ≥ `arg`, dup into it; like `dup()` but
  with the lower-bound constraint.
- [x] `F_DUPFD_CLOEXEC` (1030): same as `F_DUPFD` but sets `FD_CLOEXEC`.
- [x] `F_GETLK` / `F_SETLK` / `F_SETLKW` (5,6,7): POSIX file locking — stub
  returning `ENOTSUP` initially; full implementation is P10 territory.
- [x] `pipe2(fds, flags)` syscall: like `pipe()` but accepts `O_CLOEXEC |
  O_NONBLOCK` flags atomically. Syscall number: 293 (Linux compat).

**Kernel — `struct file` extension:**

```c
/* kernel/fs/vfs.h — extended struct file */
struct file {
    struct vnode *vnode;
    uint64_t      pos;          /* current seek position */
    int           flags;        /* O_* flags at open time */
    int           access_mode;  /* O_RDONLY / O_WRONLY / O_RDWR */
    int           append_mode;  /* 1 if O_APPEND */
    int           nonblock;     /* 1 if O_NONBLOCK */
    int           refcount;     /* shared open-file descriptions (P3) */
};
```

**libc side:**
- [x] `libc/include/fcntl.h`: all `O_*`, `F_*`, `FD_*` constants.
- [x] `libc/include/sys/stat.h`: `mode_t`, `S_IFREG`, `S_IFDIR`, `S_IRUSR`,
  `S_IWUSR`, `S_IXUSR`, `S_IRGRP`, `S_IROTH`, permission macros.
- [x] Update `open()` wrapper signature: `int open(const char *path, int flags, ...)`.
- [x] Add `creat()`, `pipe2()` wrappers.
- [x] `libc/include/sys/types.h`: `mode_t`, `dev_t`, `ino_t`, `nlink_t`,
  `uid_t`, `gid_t`, `off_t`, `blksize_t`, `blkcnt_t`.

**Testing:**
- [x] `tests/unit/test_open_flags.c`: host-side flag bitmask checks.
- [x] `tests/integration/cases/test_open_flags.sh`:
  - `open("/tmp/x", O_CREAT|O_WRONLY, 0644)` → succeeds, fd valid.
  - `open("/tmp/x", O_CREAT|O_EXCL, 0644)` → EEXIST.
  - `open("/tmp/x", O_RDONLY)` then `write()` → EBADF.
  - `open("/tmp/x", O_RDWR|O_APPEND)` then `write("hi")` → appends.

### Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Changing `vfs_open()` signature breaks all callers | High | Update all 12 call sites in one commit |
| `O_NONBLOCK` on pipes requires scheduler cooperation | Medium | Yield-loop initially; full blocking in P4 |

### Definition of Done
- [x] `open("/tmp/x", O_CREAT|O_WRONLY|O_TRUNC, 0644)` creates file
- [x] `fcntl(fd, F_SETFL, O_APPEND)` makes subsequent writes append
- [x] `fcntl(fd, F_GETFL)` returns correct flags
- [x] `pipe2(fds, O_CLOEXEC)` — both ends have `cloexec` set
- [~] All previous integration tests pass (host checks green; full QEMU run pending toolchain)

---

## Phase P3 — Shared Open-File Descriptions, `lseek`, `pread`/`pwrite`

**Objective:** POSIX mandates that `fork()` duplicates file descriptors but
shares the underlying **open-file description** (including the seek offset).
This is the foundational correctness requirement that shell pipelines and most
UNIX programs depend on.

### Status: TODO

### Tasks

**Kernel — open-file description table:**
- [ ] Introduce `struct open_file_desc` (OFD): a ref-counted object separate
  from the per-process FD table:

```c
/* kernel/fs/vfd.h — open-file description */
struct ofd {
    struct vnode *vnode;
    uint64_t      pos;          /* seek offset — SHARED across dup/fork */
    int           flags;        /* O_* flags */
    int           access_mode;
    int           append_mode;
    int           nonblock;
    int           refcount;     /* atomic; free when reaches 0 */
    spinlock_t    lock;         /* protects pos on concurrent access */
};
```

- [ ] Per-process FD table becomes `struct ofd *fd_table[VFS_MAX_FDS]` —
  each entry is a pointer to a ref-counted OFD, not an embedded struct.
- [ ] `vfs_open()`: allocate a new OFD, refcount=1.
- [ ] `vfs_close()`: decrement OFD refcount; free vnode + OFD when it hits 0.
- [ ] `vfs_dup()` / `vfs_dup2()`: increment OFD refcount; new FD entry points
  to the **same OFD** — they share `pos`.
- [ ] `fork()` (`do_fork()`): copy FD table entries as OFD pointers, increment
  each refcount — child shares OFDs with parent.
- [ ] `execve()`: close all FDs with `cloexec` set; other FDs remain (sharing OFD).

**Kernel — `lseek(2)`:**
- [ ] `vfs_lseek(int fd, int64_t offset, int whence) → int64_t`
  - `SEEK_SET` (0): `ofd->pos = offset`
  - `SEEK_CUR` (1): `ofd->pos += offset`
  - `SEEK_END` (2): `ofd->pos = vnode->size + offset`
  - Return `ESPIPE` for pipes, sockets, and character devices.
- [ ] Syscall number: 8 (Linux compat). Add `SYS_LSEEK = 8` to dispatch table.
- [ ] Update all `vfs_read()` / `vfs_write()` to use `ofd->pos` and advance it.

**Kernel — `pread(2)` / `pwrite(2)` (positional I/O):**
- [ ] `pread(fd, buf, count, offset)` — read at `offset` without changing `ofd->pos`.
- [ ] `pwrite(fd, buf, count, offset)` — write at `offset` without changing `ofd->pos`.
- [ ] Syscall numbers: `pread64=17`, `pwrite64=18`.

**Kernel — `readv` / `writev` (scatter-gather I/O):**
- [ ] `struct iovec { void *iov_base; size_t iov_len; }`.
- [ ] `readv(fd, iov, iovcnt)`: loop over iov array, sum of reads.
- [ ] `writev(fd, iov, iovcnt)`: loop over iov array, sum of writes.
- [ ] Syscall numbers: `readv=19`, `writev=20`.
- [ ] `libc/include/sys/uio.h`: `struct iovec`, `readv()`, `writev()`.

**libc side:**
- [ ] `lseek()` wrapper.
- [ ] `pread()` / `pwrite()` wrappers.
- [ ] `readv()` / `writev()` wrappers.
- [ ] `ftell()` / `fseek()` / `rewind()` in `stdio` (use `lseek` internally).

**Testing:**
- [ ] `tests/integration/cases/test_lseek.sh`:
  - Write "hello", lseek to 0, read back "hello".
  - Fork; parent and child both write to same fd; verify both writes present.
  - Pipe lseek returns `ESPIPE`.
- [ ] `tests/integration/cases/test_fork_fd_sharing.sh`:
  - Parent opens file, writes 5 bytes (pos=5). Child writes 3 bytes.
  - Parent reads back: offset should be 8 (shared OFD).

### Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Changing FD table from embedded to pointer breaks all kernel code | High | One large mechanical refactor; run full test suite after |
| Concurrent pos update without lock causes data races | Medium | Spinlock in `ofd`; always held during pos read+update |

### Definition of Done
- [ ] `lseek(fd, 0, SEEK_SET)` correctly repositions reads
- [ ] `fork()` child and parent share `pos` on inherited FDs
- [ ] `dup2(old, new)` also shares `pos`
- [ ] `pread` / `pwrite` do not move `pos`
- [ ] All previous tests pass

---

## Phase P4 — Signals

**Objective:** Implement the full POSIX signal subsystem: delivery, masking,
`sigaction`, `kill`, `raise`, `sigprocmask`, `sigpending`, `sigsuspend`,
default and user-defined handlers.

### Status: TODO

### Tasks

**Kernel — signal infrastructure:**
- [ ] `kernel/proc/signal.h`: define all 32 standard POSIX signals:

```c
#define SIGHUP    1    /* Hangup */
#define SIGINT    2    /* Interrupt (Ctrl+C) */
#define SIGQUIT   3    /* Quit (Ctrl+\) */
#define SIGILL    4    /* Illegal instruction */
#define SIGTRAP   5    /* Trace/breakpoint trap */
#define SIGABRT   6    /* Abort */
#define SIGBUS    7    /* Bus error */
#define SIGFPE    8    /* Floating-point exception */
#define SIGKILL   9    /* Kill (cannot be caught) */
#define SIGUSR1  10    /* User-defined 1 */
#define SIGSEGV  11    /* Segmentation fault */
#define SIGUSR2  12    /* User-defined 2 */
#define SIGPIPE  13    /* Broken pipe */
#define SIGALRM  14    /* Alarm */
#define SIGTERM  15    /* Termination */
#define SIGCHLD  17    /* Child stopped or exited */
#define SIGCONT  18    /* Continue */
#define SIGSTOP  19    /* Stop (cannot be caught) */
#define SIGTSTP  20    /* Terminal stop (Ctrl+Z) */
#define SIGTTIN  21    /* Terminal input for background process */
#define SIGTTOU  22    /* Terminal output for background process */
#define SIGWINCH 28    /* Window size change */
#define NSIG     32
```

- [ ] Per-process signal state in `tcb_t`:

```c
/* Added to struct tcb in kernel/proc/thread.h */
uint32_t sig_pending;           /* bitmask of pending signals */
uint32_t sig_mask;              /* currently blocked signals (sigmask) */
struct sigaction sig_actions[NSIG]; /* registered handlers */
uint64_t sig_alt_stack;         /* alternate signal stack (sigaltstack) */
```

- [ ] `struct sigaction` in kernel:

```c
struct sigaction {
    void     (*sa_handler)(int);    /* SIG_DFL=0, SIG_IGN=1, or fn ptr */
    uint32_t  sa_mask;              /* additional signals to block during handler */
    int       sa_flags;             /* SA_RESTART, SA_SIGINFO, SA_NODEFER */
};
```

- [ ] `kernel/proc/signal.c`: implement:
  - `signal_send(tcb_t *target, int signum)` — set bit in `sig_pending`.
  - `signal_deliver(tcb_t *tcb)` — called on return-to-userspace path;
    check `sig_pending & ~sig_mask`; dispatch highest-priority pending signal.
  - Default actions per signal:
    - Terminate: SIGHUP, SIGINT, SIGKILL, SIGTERM, SIGUSR1, SIGUSR2,
      SIGPIPE, SIGALRM, SIGBUS, SIGFPE, SIGSEGV, SIGILL.
    - Ignore: SIGCHLD (default), SIGWINCH, SIGCONT.
    - Stop: SIGSTOP, SIGTSTP.
  - `SIGKILL` and `SIGSTOP` cannot be caught or ignored — enforce in
    `do_sigaction()`.
  - User handler delivery: build a signal frame on the user stack (push
    saved registers + `struct sigcontext`), then `iretq` to handler;
    handler returns via `sigreturn()` syscall which restores the frame.

- [ ] Signal frame layout on user stack:

```
┌─────────────────────────┐ ← user RSP before signal
│  struct sigcontext      │   (saved: rax,rbx,rcx,rdx,rsi,rdi,rbp,
│  (saved registers)      │    rsp,r8–r15, rip, rflags, cs, ss)
├─────────────────────────┤
│  signum (arg to handler)│
├─────────────────────────┤
│  return address →       │
│  __sigreturn trampoline │   (in libc, calls SYS_SIGRETURN)
└─────────────────────────┘
```

- [ ] Hook `signal_deliver()` into:
  - SYSCALL return path (after `syscall_dispatch`, before `sysretq`).
  - IRQ return path (after any timer/IRQ handler when returning to Ring 3).
  - Exception handler return path (after #PF, #GP recovery).

**Kernel — signal syscalls:**
- [ ] `SYS_KILL = 62`: `kill(pid_t pid, int sig)` — send signal to process.
  - `pid > 0`: send to that process.
  - `pid == 0`: send to all processes in process group (stub: send to all).
  - `pid == -1`: send to all processes except init.
- [ ] `SYS_SIGACTION = 13`: `sigaction(int sig, struct sigaction *act, struct sigaction *oldact)`.
- [ ] `SYS_SIGPROCMASK = 14`: `sigprocmask(int how, sigset_t *set, sigset_t *oldset)`.
  - `SIG_BLOCK` (0): `mask |= *set`.
  - `SIG_UNBLOCK` (1): `mask &= ~*set`.
  - `SIG_SETMASK` (2): `mask = *set`.
- [ ] `SYS_SIGPENDING = 127`: return `sig_pending & sig_mask` to user.
- [ ] `SYS_SIGSUSPEND = 130`: replace mask temporarily, sleep until signal.
- [ ] `SYS_SIGRETURN = 15`: restore saved `struct sigcontext` from user stack,
  restore original sigmask, return to interrupted instruction.
- [ ] `SYS_ALARM = 37`: schedule `SIGALRM` after N seconds via PIT tick counter.
- [ ] `SYS_PAUSE = 34`: sleep until any signal is delivered.

**Kernel — CPU exception → signal mapping:**
- [ ] `#DE` (divide-by-zero) in Ring 3 → `SIGFPE` (instead of killing thread directly).
- [ ] `#PF` (page fault) in Ring 3, unresolvable → `SIGSEGV`.
- [ ] `#GP` (general protection) in Ring 3 → `SIGSEGV`.
- [ ] `#UD` (invalid opcode) in Ring 3 → `SIGILL`.
- [ ] `#BP` (breakpoint) in Ring 3 → `SIGTRAP`.

**Keyboard → signal:**
- [ ] TTY layer detects `Ctrl+C` → `SIGINT` to foreground process group.
- [ ] `Ctrl+Z` → `SIGTSTP`.
- [ ] `Ctrl+\` → `SIGQUIT`.

**libc side:**
- [ ] `libc/include/signal.h`: `sigset_t`, `struct sigaction`, all `SIG*` constants,
  `SIG_DFL`, `SIG_IGN`, `SA_*` flags, `sigset_t` manipulation macros
  (`sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset`, `sigismember`).
- [ ] `libc/src/signal.c`: `signal()`, `sigaction()`, `kill()`, `raise()`,
  `sigprocmask()`, `sigpending()`, `sigsuspend()`, `alarm()`, `pause()`.
- [ ] `libc/crt/sigreturn.asm`: `__sigreturn` trampoline (just `syscall` with
  `rax = SYS_SIGRETURN`).

**Testing:**
- [ ] `tests/integration/cases/test_signals.sh`:
  - `kill(getpid(), SIGUSR1)` with `SIGUSR1` handler installed → handler runs,
    prints "got SIGUSR1".
  - `SIGINT` from keyboard (Ctrl+C sim) → process terminates.
  - `SIGCHLD` from child exit → parent handler runs.
  - `sigprocmask(SIG_BLOCK, {SIGTERM})` → `kill(self, SIGTERM)` → signal pending,
    not delivered; unblock → delivered immediately.

### Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Signal frame corruption on user stack | High | Test with simple handler first; add stack-alignment check |
| Re-entrant signals during handler | Medium | Block `sa_mask` + current signal during delivery |
| `sigreturn` restores wrong RIP | High | Careful frame layout; debug with GDB first |
| SIGKILL of current process while in kernel | Medium | Check at syscall exit; defer to safe context |

### Definition of Done
- [ ] `signal(SIGUSR1, handler)` + `kill(getpid(), SIGUSR1)` → handler executes
- [ ] `SIGSEGV` on null-deref in userspace → default handler terminates, parent gets `SIGCHLD`
- [ ] `Ctrl+C` in shell kills foreground child process
- [ ] `sigprocmask` correctly blocks/unblocks delivery
- [ ] `alarm(1)` → `SIGALRM` fires after ~1 second

---

## Phase P5 — Terminal (TTY) & `termios`

**Objective:** Provide a proper POSIX terminal interface. Programs like `sh`,
`vi`, `less`, and any line editor need `termios` for raw/cooked mode, line
discipline, echo control, and special character processing.

### Status: TODO

### Tasks

**Kernel — TTY / line discipline layer:**
- [ ] `kernel/tty/tty.h` / `tty.c`: new subsystem.
- [ ] `struct tty`: line buffer, `termios` settings, read/write queues,
  associated VFS device node (`/dev/tty0`, `/dev/ttyS0`).
- [ ] Line discipline (`N_TTY`):
  - **Canonical mode** (default): accumulate input until `\n` or `EOF` (^D);
    support `ERASE` (Backspace/^H), `KILL` (^U), `EOF` (^D), `INTR` (^C→SIGINT),
    `QUIT` (^\→SIGQUIT), `SUSP` (^Z→SIGTSTP), `NL`/`CR` handling.
  - **Raw mode** (`~ICANON`): pass each byte directly; no echo unless `ECHO` set;
    `MIN` and `TIME` parameters for `read()` blocking behavior.
  - **Echo**: if `ECHO` set, echo input characters back to output.
- [ ] Wire PS/2 keyboard IRQ → TTY input queue.
- [ ] Wire framebuffer console / UART → TTY output.
- [ ] `/dev/tty0`: the current VT (framebuffer console).
- [ ] `/dev/ttyS0`: UART serial.
- [ ] `init.c` (PID 1): `open("/dev/tty0")` and use it as stdin/stdout/stderr
  instead of raw UART syscalls.

**Kernel — `termios` syscalls:**
- [ ] `SYS_IOCTL = 16`: dispatch table for TTY ioctls:
  - `TCGETS` (0x5401): copy `struct termios` to userspace.
  - `TCSETS` (0x5402): copy `struct termios` from userspace, apply immediately.
  - `TCSETSW` (0x5403): apply after output drains.
  - `TCSETSF` (0x5404): apply + flush input queue.
  - `TIOCGWINSZ` (0x5413): return `struct winsize {ws_row, ws_col, ws_xpixel, ws_ypixel}`.
  - `TIOCSWINSZ` (0x5414): set window size, send `SIGWINCH` to foreground group.
  - `TIOCGPGRP` (0x540F): get foreground process group.
  - `TIOCSPGRP` (0x5410): set foreground process group.
  - `TIOCSCTTY` (0x540E): set controlling terminal.

**Kernel — `struct termios`:**

```c
/* kernel/tty/termios.h */
struct termios {
    uint32_t c_iflag;   /* input modes: ICRNL, IXON, BRKINT, INPCK, ISTRIP */
    uint32_t c_oflag;   /* output modes: OPOST, ONLCR */
    uint32_t c_cflag;   /* control modes: CSIZE, CS8, CREAD, CLOCAL, HUPCL */
    uint32_t c_lflag;   /* local modes: ISIG, ICANON, ECHO, ECHOE, ECHOK,
                           ECHONL, NOFLSH, TOSTOP, IEXTEN */
    uint8_t  c_cc[NCCS]; /* NCCS=20 special characters:
                            VINTR=0, VQUIT=1, VERASE=2, VKILL=3, VEOF=4,
                            VTIME=5, VMIN=6, VSWTC=7, VSTART=8, VSTOP=9,
                            VSUSP=10, VEOL=11, VREPRINT=12, VDISCARD=13,
                            VWERASE=14, VLNEXT=15, VEOL2=16 */
};
```

**libc side:**
- [ ] `libc/include/termios.h`: `struct termios`, all `I/O/C/L` flag constants,
  `tcgetattr()`, `tcsetattr()`, `cfgetispeed()`, `cfsetispeed()`, `cfgetospeed()`,
  `cfsetospeed()`, `cfmakeraw()`.
- [ ] `libc/include/sys/ioctl.h`: `ioctl()` declaration, `TIOC*` constants.
- [ ] `libc/src/termios.c`: thin wrappers over `SYS_IOCTL`.
- [ ] Update `libc/include/unistd.h`: `isatty(int fd)` → ioctl `TCGETS` returns 0 if TTY.
- [ ] `libc/src/stdio/`: `printf`, `scanf` should use fd 1/0 via TTY.
- [ ] `libc/src/stdio/readline.c`: `readline()`-style line editor using raw termios:
  left/right arrows (ANSI sequences), backspace, history (simple circular buffer).

**libc — `stdio` FILE streams:**
- [ ] `FILE` type, `stdin` / `stdout` / `stderr` as global `FILE*` pointing to
  fd 0/1/2.
- [ ] `fopen(path, mode)` / `fclose(FILE*)` / `fread()` / `fwrite()` / `fgets()` /
  `fputs()` / `fprintf()` / `fscanf()` / `fflush()` / `feof()` / `ferror()` /
  `clearerr()` / `fileno()` / `fdopen()` / `freopen()`.
- [ ] Internal buffering: line-buffered for TTY, fully buffered for files.

**Testing:**
- [ ] `tests/integration/cases/test_termios.sh`:
  - Program calls `cfmakeraw()`, reads one byte without echo, then restores.
  - `TIOCGWINSZ` returns nonzero rows/cols.
  - `SIGWINCH` is sent when window size changes (simulated via `TIOCSWINSZ`).

### Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Framebuffer console and UART need unified TTY abstraction | Medium | Abstract via `tty_ops` (write_char, read_char) callbacks |
| Raw mode breaks existing init/shell | Medium | Keep existing path; switch to TTY path behind `O_RDONLY` of `/dev/tty0` |

### Definition of Done
- [ ] `tcgetattr` / `tcsetattr` work on `/dev/tty0`
- [ ] Shell switches to raw mode for line editing (arrows, backspace)
- [ ] `isatty(1)` returns 1 for terminal, 0 for pipe
- [ ] `scanf` / `fgets` work via TTY line discipline in canonical mode

---

## Phase P6 — Process Groups, Sessions & Job Control

**Objective:** Implement POSIX process groups and sessions so that the shell
can manage foreground/background jobs, `Ctrl+C` sends `SIGINT` to the right
group, and `waitpid(-1)` collects any child.

### Status: TODO

### Tasks

**Kernel — process group / session fields:**
- [ ] Add to `tcb_t`:

```c
pid_t  pgid;        /* process group ID (leader: pgid == pid) */
pid_t  sid;         /* session ID (leader: sid == pid) */
int    is_session_leader;
struct tty *ctty;   /* controlling terminal */
```

- [ ] `setsid()` syscall (`SYS_SETSID=112`): create new session; caller becomes
  leader; detach from controlling terminal.
- [ ] `setpgid(pid, pgid)` syscall (`SYS_SETPGID=109`): move process to
  process group `pgid` (must be in same session).
- [ ] `getpgid(pid)` syscall (`SYS_GETPGID=121`) / `getpgrp()`.
- [ ] `getsid(pid)` syscall (`SYS_GETSID=124`).
- [ ] TTY ioctls `TIOCGPGRP` / `TIOCSPGRP`: get/set the foreground process group
  of the terminal.
- [ ] `kill()`: when `pid < 0`: send to process group `|pid|`.
- [ ] `signal_send_group(pgid, sig)`: iterate all TCBs with matching pgid.
- [ ] Keyboard interrupt (`Ctrl+C`): send `SIGINT` to the **foreground** process
  group of the controlling terminal.
- [ ] `SIGCHLD`: sent to parent when any child in its process group changes state.

**Kernel — `waitpid()` upgrade:**
- [ ] `waitpid(pid_t pid, int *status, int options)`:
  - `pid > 0`: wait for specific child.
  - `pid == 0`: wait for any child in same process group.
  - `pid == -1`: wait for any child (current behavior).
  - `pid < -1`: wait for any child in group `|pid|`.
  - `WNOHANG` (1): return 0 immediately if no child has exited.
  - `WUNTRACED` (2): also return for stopped children.
  - Status encoding: `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED`, `WTERMSIG`,
    `WIFSTOPPED`, `WSTOPSIG` macros.

**libc side:**
- [ ] `libc/include/sys/wait.h`: `WNOHANG`, `WUNTRACED`, `W*` macros.
- [ ] `libc/include/unistd.h`: `setsid()`, `setpgid()`, `getpgid()`, `getsid()`,
  `getpgrp()`, `tcgetpgrp()`, `tcsetpgrp()`.
- [ ] Update `userspace/init/init.c` (shell): implement job control:
  - `fork()` child → `setpgid(child, child)` → `tcsetpgrp(tty, child_pgid)`.
  - On child exit/stop: restore foreground to shell's pgid.
  - Built-ins: `jobs`, `fg [%job]`, `bg [%job]`.
  - `&` suffix: start child in background (don't call `tcsetpgrp`).

**Testing:**
- [ ] `tests/integration/cases/test_jobcontrol.sh`:
  - `sleep 100 &` runs in background; `jobs` lists it.
  - `kill %1` sends SIGTERM; job disappears.
  - Ctrl+C with foreground process → only foreground process gets SIGINT.

### Definition of Done
- [ ] `setsid()` creates new session
- [ ] `waitpid(child, &st, WNOHANG)` returns 0 when child still running
- [ ] Shell job control: `cmd &`, `fg`, `bg`, `jobs` work
- [ ] Ctrl+C only kills foreground process group

---

## Phase P7 — User & Group IDs, File Permissions

**Objective:** Implement POSIX user identity (UID/GID), file permission checks
(rwx for user/group/other), and the associated syscalls.

### Status: TODO

### Tasks

**Kernel — credential fields in TCB:**
- [ ] Add to `tcb_t`:

```c
uid_t  uid, euid, suid;   /* real, effective, saved-set UID */
gid_t  gid, egid, sgid;   /* real, effective, saved-set GID */
gid_t  supplementary_gids[NGROUPS_MAX]; /* NGROUPS_MAX=32 */
int    ngroups;
```

- [ ] Boot: kernel sets UID/GID=0 (root) for `init`.
- [ ] `fork()`: child inherits parent's credentials.
- [ ] `execve()`: if the ELF file has setuid bit set (`S_ISUID`), set `euid`
  to file owner; if `S_ISGID`, set `egid` to file group.

**Kernel — permission checking:**
- [ ] `vfs_check_perm(vnode, int access, tcb_t *tcb) → int`:
  - `access`: `R_OK=4`, `W_OK=2`, `X_OK=1`.
  - If `tcb->euid == 0` (root): allow all.
  - Compare `vnode->uid`, `vnode->mode` against `tcb->euid/egid`.
  - Return `0` on success, `-EACCES` on denial.
- [ ] Insert `vfs_check_perm()` at `vfs_open()`, `vfs_stat()`, `vfs_mkdir()`,
  `vfs_unlink()`, `vfs_readdir()`.
- [ ] Filesystems must store `uid`, `gid`, `mode` in inodes (ext2 already does;
  tmpfs/diskfs need fake values).

**Kernel — credential syscalls:**
- [ ] `SYS_GETUID=102`, `SYS_GETGID=104`, `SYS_GETEUID=107`, `SYS_GETEGID=108`.
- [ ] `SYS_SETUID=105`: set UID (root only, or to saved UID).
- [ ] `SYS_SETGID=106`: set GID.
- [ ] `SYS_SETREUID=113`, `SYS_SETREGID=114`: set real+effective UID/GID.
- [ ] `SYS_GETGROUPS=115`, `SYS_SETGROUPS=116`.
- [ ] `SYS_ACCESS=21`: `access(path, mode)` — check file access for **real** UID/GID.
- [ ] `SYS_CHMOD=90`: change file permission bits.
- [ ] `SYS_CHOWN=92`: change file owner/group (root only, or own files).
- [ ] `SYS_UMASK=95`: set/get file creation mask.

**libc side:**
- [ ] `libc/include/unistd.h`: `getuid()`, `getgid()`, `geteuid()`, `getegid()`,
  `setuid()`, `setgid()`, `getgroups()`, `setgroups()`, `access()`.
- [ ] `libc/include/sys/stat.h`: `chmod()`, `chown()`, `umask()`,
  `S_ISREG`, `S_ISDIR`, `S_ISLNK`, `S_ISSOCK`, `S_ISBLK`, `S_ISCHR` macros.

**Testing:**
- [ ] `tests/integration/cases/test_permissions.sh`:
  - Create file with `0600` mode; read as self → OK; attempt via new process
    with different UID → `EACCES`.
  - `chmod(path, 0644)` → read now succeeds.
  - Root process can read any file.

### Definition of Done
- [ ] `getuid()` / `geteuid()` return correct values
- [ ] `open("/root-only-file", O_RDONLY)` as non-root → `EACCES`
- [ ] `chmod(path, 0777)` makes file world-accessible
- [ ] `umask(022)` masks out group/other write on new file creation

---

## Phase P8 — POSIX Clocks, Timers & `sleep`

**Objective:** Implement `clock_gettime`, `nanosleep`, `getitimer` / `setitimer`,
POSIX timers (`timer_create`), and `gettimeofday`.

### Status: TODO

### Tasks

**Kernel — clock sources:**
- [ ] Read RTC (Real-Time Clock, `drivers/rtc/`) at boot to get wall-clock time.
- [ ] Maintain `struct timespec kernel_boot_time` (seconds since Unix epoch at boot).
- [ ] Maintain `uint64_t kernel_monotonic_ns` incremented by PIT/APIC timer.
- [ ] `CLOCK_REALTIME`: `boot_time + monotonic_ns / 1e9`.
- [ ] `CLOCK_MONOTONIC`: `monotonic_ns` from boot.
- [ ] `CLOCK_PROCESS_CPUTIME_ID`: per-TCB CPU tick counter × tick period.
- [ ] `CLOCK_THREAD_CPUTIME_ID`: same but per-thread.

**Kernel — time syscalls:**
- [ ] `SYS_CLOCK_GETTIME=228`: `clock_gettime(clockid_t, struct timespec*)`.
- [ ] `SYS_CLOCK_GETRES=229`: `clock_getres(clockid_t, struct timespec*)` — return PIT period (~10ms).
- [ ] `SYS_NANOSLEEP=35`: `nanosleep(const struct timespec *req, struct timespec *rem)` —
  put thread to sleep for the requested duration using tick counter; update `rem`
  on signal interruption (return `-EINTR`).
- [ ] `SYS_GETTIMEOFDAY=96`: `gettimeofday(struct timeval*, struct timezone*)`.
- [ ] `SYS_TIME=201`: `time(time_t*)` — seconds since epoch.
- [ ] `SYS_GETITIMER=36` / `SYS_SETITIMER=38`:
  - `ITIMER_REAL`: sends `SIGALRM` when it expires.
  - `ITIMER_VIRTUAL`: counts only when process runs.
  - `ITIMER_PROF`: counts when process or kernel runs on behalf of process.

**libc side:**
- [ ] `libc/include/time.h`: `struct timespec`, `struct timeval`, `struct tm`,
  `time_t`, `clock_t`, `clockid_t`, `CLOCKS_PER_SEC`, `CLOCK_REALTIME`,
  `CLOCK_MONOTONIC`, `clock_gettime()`, `nanosleep()`, `gettimeofday()`,
  `time()`, `difftime()`, `mktime()`, `localtime()`, `gmtime()`,
  `asctime()`, `ctime()`, `strftime()`.
- [ ] `libc/src/time.c`: implement `mktime`, `gmtime`, `localtime` (UTC only
  initially), `strftime` (subset of format specifiers).
- [ ] `libc/include/unistd.h`: `sleep(unsigned int seconds)` → `nanosleep`.
- [ ] `libc/src/time/usleep.c`: `usleep(useconds_t)`.

**Testing:**
- [ ] `tests/integration/cases/test_clock.sh`:
  - `clock_gettime(CLOCK_MONOTONIC)` returns increasing values.
  - `nanosleep({0, 100000000})` (100ms) delays by ~100ms (verify with tick counter).
  - `ITIMER_REAL` with 1s interval delivers `SIGALRM`.

### Definition of Done
- [ ] `clock_gettime(CLOCK_MONOTONIC)` returns valid sub-second resolution timestamps
- [ ] `sleep(1)` delays for approximately 1 second
- [ ] `alarm(2)` delivers `SIGALRM` after ~2 seconds
- [ ] `strftime(buf, sizeof(buf), "%Y-%m-%d", gmtime(&t))` formats correctly

---

## Phase P9 — POSIX Threads (`pthread`)

**Objective:** Implement `pthread_create`, `pthread_join`, `pthread_exit`,
mutexes, condition variables, and thread-local storage — the minimal `pthreads`
subset used by the majority of multi-threaded POSIX programs.

### Status: TODO

### Tasks

**Kernel — thread syscall extension:**
- [ ] `SYS_CLONE=56`: Linux-compatible clone for thread creation:
  - `CLONE_VM` (0x100): share address space.
  - `CLONE_FS` (0x200): share filesystem (cwd).
  - `CLONE_FILES` (0x400): share FD table.
  - `CLONE_SIGHAND` (0x800): share signal handlers.
  - `CLONE_THREAD` (0x10000): place in same thread group.
  - `CLONE_SETTLS` (0x80000): set TLS base (FS register).
- [ ] Extend `tcb_t` with `thread_group_id` (= main thread's PID for threads in
  the same process), `tls_base` (FS.base loaded via `WRFSBASE` MSR or `arch_prctl`).
- [ ] `SYS_ARCH_PRCTL=158`: `ARCH_SET_FS` (0x1002) / `ARCH_GET_FS` (0x1003)
  for setting FS.base (thread pointer / TLS).
- [ ] `SYS_TKILL=200`: send signal to specific thread (`tgid`, `tid`).
- [ ] `SYS_FUTEX=202`: fast user-space mutex:
  - `FUTEX_WAIT`: atomically check `*uaddr == val`, then sleep.
  - `FUTEX_WAKE`: wake up to `val` threads waiting on `uaddr`.
  - `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET` (optional, for condition variables).
  - Kernel maintains a futex hash table (by physical address of the futex word).

**Kernel — `errno` becomes thread-local:**
- [ ] Once `CLONE_VM` threads exist, `errno` must be per-thread.
- [ ] Implement as `int *__errno_location(void)` returning thread-specific
  pointer via FS-relative offset (TLS slot 0).
- [ ] Redefine `errno` macro: `#define errno (*__errno_location())`.

**libc — `libpthread` (or integrated):**
- [ ] `libc/include/pthread.h`: `pthread_t`, `pthread_attr_t`, `pthread_mutex_t`,
  `pthread_mutexattr_t`, `pthread_cond_t`, `pthread_condattr_t`,
  `pthread_rwlock_t`, `pthread_key_t`, `pthread_once_t`.
- [ ] `libc/src/pthread/pthread_create.c`:
  - Allocate thread stack (via `mmap`).
  - Set up TLS block at top of stack.
  - Call `clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS, ...)`.
- [ ] `libc/src/pthread/pthread_join.c`: `futex_wait` on thread state.
- [ ] `libc/src/pthread/pthread_exit.c`: set exit value, wake joiners via futex, exit thread.
- [ ] `libc/src/pthread/mutex.c`:
  - `pthread_mutex_lock`: `FUTEX_WAIT` on locked mutex.
  - `pthread_mutex_trylock`: atomic CAS, no sleep.
  - `pthread_mutex_unlock`: clear, `FUTEX_WAKE`.
  - Mutex types: `PTHREAD_MUTEX_NORMAL`, `PTHREAD_MUTEX_RECURSIVE`,
    `PTHREAD_MUTEX_ERRORCHECK`.
- [ ] `libc/src/pthread/cond.c`:
  - `pthread_cond_wait(cond, mutex)`: release mutex, sleep on cond futex, reacquire.
  - `pthread_cond_signal(cond)`: `FUTEX_WAKE 1`.
  - `pthread_cond_broadcast(cond)`: `FUTEX_WAKE INT_MAX`.
- [ ] `libc/src/pthread/rwlock.c`: reader-writer lock using futexes.
- [ ] `libc/src/pthread/tls.c`:
  - `pthread_key_create` / `pthread_key_delete`.
  - `pthread_getspecific` / `pthread_setspecific`.
  - `pthread_once`.
- [ ] `libc/src/pthread/barrier.c`: `pthread_barrier_t` via futex counter.
- [ ] `libc/include/semaphore.h`: unnamed POSIX semaphores:
  - `sem_init` / `sem_destroy`.
  - `sem_wait` / `sem_post` / `sem_trywait` / `sem_timedwait`.
  - Implemented on top of futex.

**Testing:**
- [ ] `tests/unit/test_pthread.c` (host-side with POSIX pthreads for comparison):
  - Mutex lock/unlock contention with 4 threads.
  - Condition variable producer/consumer.
- [ ] `tests/integration/cases/test_pthread.sh`:
  - Two pthreads increment a shared counter 10000 times each under a mutex;
    final value must be exactly 20000.
  - `pthread_cond_wait` / `pthread_cond_signal` pipeline.

### Definition of Done
- [ ] `pthread_create` / `pthread_join` work for simple cases
- [ ] `pthread_mutex_lock` / `unlock` protect shared data correctly
- [ ] `pthread_cond_wait` / `signal` / `broadcast` work
- [ ] `sem_wait` / `sem_post` work
- [ ] Thread-local `errno` does not bleed between threads
- [ ] `pthread_key_create` / `getspecific` / `setspecific` work

---

## Phase P10 — POSIX Compliance Hardening, Missing Syscalls & libc Completion

**Objective:** Fill all remaining gaps — `select`/`poll`/`epoll`, environment
variables, locale, regex, math library, symbolic links, `getcwd`/`chdir`,
`getopt`, and the miscellaneous syscalls that POSIX programs frequently use.

### Status: TODO

### Tasks

**I/O Multiplexing:**
- [ ] `SYS_SELECT=23`: `select(nfds, readfds, writefds, exceptfds, timeout)` —
  poll VFS FDs and timeout; use `fd_set` bitmask.
- [ ] `SYS_POLL=7`: `poll(struct pollfd *fds, nfds_t nfds, int timeout)` —
  more flexible; `POLLIN`, `POLLOUT`, `POLLHUP`, `POLLERR`.
- [ ] `SYS_EPOLL_CREATE1=291`: create epoll FD.
- [ ] `SYS_EPOLL_CTL=233`: `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, `EPOLL_CTL_DEL`.
- [ ] `SYS_EPOLL_WAIT=232`: wait for events.
- [ ] Implement by having each `struct ofd` maintain a wait-queue; `poll`/`select`
  register on those queues and sleep via futex until any FD becomes ready.

**Missing file system syscalls:**
- [ ] `SYS_SYMLINK=88`: `symlink(target, linkpath)` — VFS symlink support,
  new `VFS_TYPE_SYMLINK` vnode type; readlink follows `->` stored in vnode data.
- [ ] `SYS_READLINK=89`: `readlink(path, buf, bufsiz)`.
- [ ] `SYS_LINK=86`: hard links (increase `nlink` count; share vnode).
- [ ] `SYS_GETCWD=79`: return current working directory path.
- [ ] `SYS_CHDIR=80`: change working directory (per-process `cwd` field in TCB).
- [ ] `SYS_FCHDIR=81`: `chdir` via fd.
- [ ] `SYS_GETDENTS64=217`: get directory entries (used by `opendir`/`readdir`/`closedir`).
- [ ] `SYS_FSTAT=5`: `fstat(fd, struct stat*)` — stat an open file by fd.
- [ ] `SYS_LSTAT=6`: `lstat(path, struct stat*)` — stat without following symlinks.
- [ ] `SYS_FCHMOD=91`, `SYS_FCHOWN=93`: change mode/owner by fd.
- [ ] `SYS_SYNC=162`, `SYS_FSYNC=74`, `SYS_FDATASYNC=75`: flush dirty pages/inodes.
- [ ] `SYS_TRUNCATE=76`: truncate file by path.
- [ ] `SYS_FTRUNCATE=77`: truncate file by fd.
- [ ] `SYS_MKFIFO`: named pipes (FIFO files) via `mknod(path, S_IFIFO, 0)`.

**Working directory:**
- [ ] Add `char cwd[VFS_PATH_MAX]` to `tcb_t`; init to `"/"`.
- [ ] Path resolution: if path does not start with `/`, prepend `cwd`.
- [ ] `fork()`: child inherits `cwd`.
- [ ] `execve()`: preserve `cwd`.

**Environment variables:**
- [ ] `execve()` must accept `const char *envp[]` (third argument).
- [ ] Kernel places `argv[]` and `envp[]` on the user stack below the initial `RSP`
  in the System V AMD64 ABI format:
  ```
  argc | argv[0] | … | NULL | envp[0] | … | NULL | auxv[]
  ```
- [ ] `libc/crt/crt0.asm`: parse stack frame, extract `argc`, `argv`, `envp`,
  call `__libc_start_main(main, argc, argv, envp)`.
- [ ] `libc/include/stdlib.h`: `getenv()`, `setenv()`, `unsetenv()`, `putenv()`,
  `clearenv()`.
- [ ] `libc/src/stdlib/env.c`: maintain a `char **environ` global.

**Shared memory:**
- [ ] `SYS_SHM_OPEN` (via `SYS_OPEN` on `/dev/shm/name`): open shared memory object.
- [ ] `MAP_SHARED` in `mmap()`: map the same physical pages into two processes.
- [ ] Kernel must track shared physical frames (reference-counted in PMM).
- [ ] `shm_unlink(name)`.

**Locale & character classification:**
- [ ] `libc/include/locale.h`: `locale_t`, `setlocale()`, `LC_ALL`, `LC_CTYPE`, etc.
- [ ] Minimal implementation: `setlocale(LC_ALL, "C")` always succeeds; C locale only.
- [ ] `libc/include/wchar.h`: `wchar_t`, `wint_t`, `WEOF`, `wcslen()`, `wcscpy()`,
  `wcscat()`, `wcscmp()`, `mbstowcs()`, `wcstombs()`, `btowc()`, `wctob()`.

**Pattern matching:**
- [ ] `libc/include/regex.h`: `regex_t`, `regmatch_t`, `regcomp()`, `regexec()`,
  `regfree()`, `regerror()`. Implement POSIX ERE/BRE.
- [ ] `libc/include/fnmatch.h`: `fnmatch(pattern, string, flags)` — glob matching.
- [ ] `libc/include/glob.h`: `glob()` / `globfree()` — pathname expansion.

**Miscellaneous POSIX syscalls:**
- [ ] `SYS_GETRLIMIT=97` / `SYS_SETRLIMIT=160`: resource limits.
  Minimal: `RLIMIT_NOFILE`, `RLIMIT_STACK`, `RLIMIT_AS`.
- [ ] `SYS_GETRUSAGE=98`: resource usage (CPU time, memory).
- [ ] `SYS_SYSINFO=99`: system info (uptime, total/free RAM, load).
- [ ] `SYS_UNAME=63`: `uname(struct utsname*)` — return kernel name, release, etc.
- [ ] `SYS_MPROTECT=10`: change protection on a range of pages.
- [ ] `SYS_MINCORE=27`: query residency of pages.
- [ ] `SYS_MADVISE=28`: `MADV_NORMAL`, `MADV_SEQUENTIAL`, `MADV_DONTNEED`.
- [ ] `SYS_PRLIMIT64=302`: combined get+set resource limit.

**libc completion:**
- [ ] `libc/include/stdlib.h`: `abort()`, `exit()` (flush stdio + `_exit`),
  `atexit()` / `on_exit()`, `qsort()`, `bsearch()`, `abs()` / `labs()` /
  `llabs()`, `div()` / `ldiv()` / `lldiv()`, `strtoul()`, `strtoll()`,
  `strtoull()`, `strtod()`, `strtof()`, `strtold()`, `system()` (fork+exec sh),
  `mkstemp()`, `mkstemps()`.
- [ ] `libc/include/string.h` completion: `strchr()`, `strrchr()`, `strstr()`,
  `strtok_r()`, `strpbrk()`, `strspn()`, `strcspn()`, `strnlen()`,
  `stpcpy()`, `stpncpy()`, `memchr()`, `memmem()`, `strdup()`, `strndup()`,
  `strcasecmp()`, `strncasecmp()`.
- [ ] `libc/include/stdio.h` completion: `sprintf()`, `snprintf()`, `sscanf()`,
  `vsprintf()`, `vsnprintf()`, `vprintf()`, `vfprintf()`, `vsscanf()`,
  `getchar()` / `putchar()`, `getc()` / `putc()`, `ungetc()`, `fgetc()` /
  `fputc()`, `fgets()` / `fputs()`, `gets_s()`, `setvbuf()`, `setbuf()`,
  `tmpfile()`, `tmpnam()`, `remove()`, `rename()`.
- [ ] `libc/include/dirent.h`: `DIR*`, `opendir()`, `readdir()`, `closedir()`,
  `rewinddir()`, `seekdir()`, `telldir()`.
- [ ] `libc/include/sys/mman.h` completion: `mmap()`, `munmap()`, `mprotect()`,
  `msync()`, `mlock()`, `munlock()`, `shm_open()`, `shm_unlink()`.
- [ ] `libc/include/dlfcn.h`: stub `dlopen()` / `dlsym()` / `dlclose()` /
  `dlerror()` (no dynamic linker; return `NULL` + `ENOTSUP`).
- [ ] `getopt()` / `getopt_long()` in `libc/src/stdlib/getopt.c`.
- [ ] `libc/include/grp.h` / `pwd.h`: `struct passwd`, `struct group`,
  `getpwuid()`, `getpwnam()`, `getgrgid()`, `getgrnam()` — backed by
  flat `/etc/passwd` and `/etc/group` files read via VFS.

**Math library (`libm`):**
- [ ] `libc/src/math/`: software implementations of:
  `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`,
  `sinh`, `cosh`, `tanh`, `exp`, `exp2`, `log`, `log2`, `log10`,
  `pow`, `sqrt`, `cbrt`, `hypot`, `fabs`, `floor`, `ceil`, `round`,
  `trunc`, `fmod`, `remainder`, `fma`, `frexp`, `ldexp`, `modf`.
- [ ] Use Cephes / MUSL math sources (BSD-licensed) as reference for correctness.

**Network socket ABI completion:**
- [ ] `libc/include/sys/socket.h`: `struct sockaddr`, `struct sockaddr_in`,
  `socklen_t`, `AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_NONBLOCK`,
  `SOL_SOCKET`, `SO_REUSEADDR`, `SO_RCVTIMEO`, `SO_SNDTIMEO`.
- [ ] `libc/include/netinet/in.h`: `struct in_addr`, `htons()`, `htonl()`,
  `ntohs()`, `ntohl()`, `INADDR_ANY`, `INADDR_LOOPBACK`.
- [ ] `libc/include/arpa/inet.h`: `inet_aton()`, `inet_ntoa()`, `inet_pton()`,
  `inet_ntop()`.
- [ ] `libc/include/netdb.h`: `struct hostent`, `gethostbyname()`,
  `struct addrinfo`, `getaddrinfo()`, `freeaddrinfo()`, `gai_strerror()`.
- [ ] Full BSD socket syscalls: `bind`, `listen`, `accept` (TCP server support).
- [ ] `UDP socket`: `sendto()` / `recvfrom()` / `bind()`.

**Testing:**
- [ ] `tests/integration/cases/test_select.sh`:
  - `select()` on pipe returns when data is available.
  - `select()` with timeout expires correctly.
- [ ] `tests/integration/cases/test_cwd.sh`:
  - `getcwd()` returns `"/"` at start.
  - `chdir("/tmp")` → `getcwd()` returns `"/tmp"`.
- [ ] `tests/integration/cases/test_env.sh`:
  - `setenv("FOO", "bar")` → `getenv("FOO")` returns `"bar"`.
  - `execve("/bin/printenv", ...)` inherits environment.
- [ ] `tests/integration/cases/test_select_poll.sh`: POSIX I/O multiplexing.
- [ ] `tests/unit/test_regex.c`: host-side regex compile + match.
- [ ] `tests/unit/test_math.c`: host-side math function accuracy.

### Definition of Done
- [ ] `select()` works on pipes and sockets
- [ ] `getcwd()` / `chdir()` are correct; paths resolve relative to cwd
- [ ] `getenv()` / `setenv()` work; `execve` passes environment
- [ ] `opendir()` / `readdir()` / `closedir()` work
- [ ] `regex.h` — `regcomp` + `regexec` match simple patterns
- [ ] `math.h` — `sqrt`, `sin`, `cos`, `pow` return correct values
- [ ] `pthread_create` + mutex + cond work in integration test
- [ ] BSD socket `bind`/`listen`/`accept` allow TCP server

---

## POSIX Gate Criteria Summary

| Phase | Name | Gate Criterion |
|---|---|---|
| P1 | errno & libc foundations | `open("/no")` → errno=ENOENT; `perror()` prints correct string |
| P2 | open flags & fcntl | `O_CREAT`, `O_EXCL`, `O_APPEND`, `F_SETFL` all work correctly |
| P3 | Shared OFD & lseek | `fork` inherits shared pos; `lseek(SEEK_END)` correct |
| P4 | Signals | `SIGUSR1` handler fires; `SIGSEGV` on null-ptr kills process |
| P5 | TTY & termios | Raw-mode shell works; `tcgetattr`/`tcsetattr` function |
| P6 | Process groups & job control | `fg`, `bg`, `jobs` in shell; Ctrl+C sends SIGINT to right group |
| P7 | UIDs, GIDs & permissions | `chmod` / `chown` / `access()` / `umask` all correct |
| P8 | Clocks & sleep | `nanosleep(100ms)` accurate ±5%; `alarm(1)` fires SIGALRM |
| P9 | pthreads & futex | Mutex-protected counter reaches exact value across N threads |
| P10 | Compliance hardening | `select`, `getcwd`/`chdir`, env vars, regex, math, BSD sockets all pass |

---

## New Files Required (Summary)

```
kernel/
├── tty/
│   ├── tty.c / tty.h             # TTY/line-discipline layer
│   └── termios.h                  # struct termios, constants
├── proc/
│   └── signal.c / signal.h        # Signal subsystem
├── fs/
│   └── ofd.c / ofd.h             # Open-file description ref-counting
├── lib/
│   └── errno.h                    # Kernel errno codes
└── sync/
    └── futex.c / futex.h          # FUTEX_WAIT / FUTEX_WAKE

libc/
├── include/
│   ├── errno.h                    # errno macro + E* constants
│   ├── signal.h                   # sigset_t, struct sigaction, SIG*
│   ├── termios.h                  # struct termios, tc*attr, cfmakeraw
│   ├── sys/ioctl.h                # ioctl(), TIOC* constants
│   ├── sys/wait.h                 # WNOHANG, W* macros
│   ├── sys/uio.h                  # struct iovec, readv, writev
│   ├── sys/mman.h (complete)      # mprotect, msync, shm_open
│   ├── sys/socket.h               # sockaddr, socklen_t, SO_*
│   ├── sys/stat.h (complete)      # mode_t, S_IS*, chmod, stat
│   ├── sys/types.h (complete)     # uid_t, gid_t, off_t, etc.
│   ├── sys/resource.h             # rlimit, getrlimit, getrusage
│   ├── sys/utsname.h              # struct utsname
│   ├── sys/select.h               # fd_set, select()
│   ├── poll.h                     # struct pollfd, poll()
│   ├── pthread.h                  # pthread_t, mutex, cond, rwlock
│   ├── semaphore.h                # sem_t, sem_init, sem_wait
│   ├── time.h (complete)          # clock_gettime, nanosleep, strftime
│   ├── locale.h                   # setlocale, LC_*
│   ├── regex.h                    # regcomp, regexec
│   ├── fnmatch.h                  # fnmatch()
│   ├── glob.h                     # glob(), globfree()
│   ├── dirent.h                   # DIR, opendir, readdir
│   ├── fcntl.h (complete)         # O_*, F_*, FD_CLOEXEC
│   ├── limits.h                   # PATH_MAX, NAME_MAX, ARG_MAX
│   ├── math.h                     # sin, cos, sqrt, pow, ...
│   ├── wchar.h                    # wchar_t, wide string functions
│   ├── ctype.h                    # isalpha, isdigit, toupper, ...
│   ├── assert.h                   # assert()
│   ├── dlfcn.h                    # dlopen stub
│   ├── grp.h                      # struct group, getgrnam
│   ├── pwd.h                      # struct passwd, getpwnam
│   ├── netinet/in.h               # sockaddr_in, htons, htonl
│   ├── arpa/inet.h                # inet_pton, inet_ntop
│   └── netdb.h                    # getaddrinfo, gethostbyname
└── src/
    ├── errno.c                     # errno TLS implementation
    ├── ctype.c                     # character classification table
    ├── math/                       # libm software implementations
    ├── pthread/                    # pthreads on futex
    ├── regex/                      # POSIX regex engine
    ├── signal.c                    # signal wrappers
    ├── termios.c                   # termios wrappers
    ├── time/                       # clock, strftime, mktime
    ├── stdlib/
    │   ├── env.c                   # getenv, setenv, environ
    │   ├── getopt.c                # getopt, getopt_long
    │   └── qsort.c                 # qsort, bsearch
    └── stdio/ (complete)           # FILE*, fopen, fread, fwrite, ...
```

---

## Risks & Mitigations (Global)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Signal frame layout wrong → crash on return | High | Critical | Test with trivial handler in GDB first; use known-good frame structure |
| OFD refcount races under SMP | Medium | High | Atomic `refcount`; always hold spinlock for pos update |
| TTY deadlock: IRQ writes to buffer, reader holds lock | Medium | High | IRQ-side uses `spinlock_irqsave`; never block in IRQ context |
| `pthreads` on single-CPU scheduler is untested | Medium | Medium | Run under `-smp 1` first; promote to multi-CPU after scheduler is SMP-safe |
| `errno` global bleed between threads | High (post-P9) | High | Implement TLS before shipping pthreads |
| math library precision errors | Low | Low | Compare against glibc output in host unit tests |
| Binary ABI compat (struct layout changes) | Medium | High | Version all userspace structs; rebuild all userspace when kernel structs change |
| `select`/`poll` blocking starvation | Medium | Medium | Implement proper wait queues in P9; poll-loop is only interim |

---

## Testing Strategy

Every POSIX phase follows the same three-level testing discipline as the
original roadmap:

1. **Host unit tests** (`make test-unit`):
   Compile and run against the host libc/pthreads for algorithmic correctness
   (regex engine, math functions, qsort, etc.). These run fast and catch logic
   bugs before QEMU is involved.

2. **In-kernel self-tests** (boot-time):
   Each new subsystem registers a self-test that runs during `kmain()` and
   prints `[PASS]` / `[FAIL]` on the serial console. The CI gate asserts every
   `[PASS]` line appears.

3. **QEMU integration tests** (`tests/integration/cases/test_posix_*.sh`):
   Boot the ISO, send shell commands, assert expected output on serial. These
   are the final gate criterion for each phase.

A new integration test category is added for this plan:

```bash
tests/integration/cases/
├── test_errno.sh
├── test_open_flags.sh
├── test_lseek.sh
├── test_fork_fd_sharing.sh
├── test_signals.sh
├── test_termios.sh
├── test_jobcontrol.sh
├── test_permissions.sh
├── test_clock.sh
├── test_pthread.sh
├── test_select.sh
├── test_cwd.sh
├── test_env.sh
├── test_regex.sh
└── test_math.sh
```

CI pipeline addition in `.github/workflows/integration.yml`:
```yaml
- name: POSIX compliance tests
  run: bash tests/integration/run_all.sh --filter posix
```

---

## Communication Style

- All code comments and commit messages: **English**.
- Communication with the project team: **Russian** if preferred.
- Every commit that implements a POSIX feature must reference the POSIX.1-2017
  spec section in the commit message, e.g.:
  `proc: implement sigaction (POSIX.1-2017 §2.4.3)`.
- Every new syscall must be documented in `docs/syscall_abi.md` with:
  number, name, arguments, return value, errno codes.

---

*AuraLite OS POSIX Plan v1.0 — 2026-06-27*
*Based on source analysis of commit 18b02d9 (GUI fix, 2026-06-27)*
*POSIX reference: IEEE Std 1003.1-2017 (POSIX.1-2017)*
