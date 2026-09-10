# AuraLite OS — Linux Application Compatibility Plan (the `lx` personality)

## Status: OPEN — L0 ✅ L1 ✅ L2 ✅ L3 ✅ L4 ⬜ L5 ⬜

> This is a feature plan in the style of `FSFULL_PLAN.md` and `OTA_PLAN.md`,
> written against the tree as it stands. It follows the same structure:
> dependency-ordered phases, a definition of done and a test gate for every
> phase, one `.patch` per phase.
>
> It opens the OS's second personality: **existing, unmodified Linux
> applications run on AuraLite** — the same way unmodified mingw `.exe`
> files already do through the w32 personality (WIN32_PLAN.md): the file's
> own format is detected at `execve`, and a per-process personality
> translates between the guest's ABI and ours.
>
> The plan's thesis, measured below: **the syscall mechanism is already
> Linux-shaped** — SYSCALL/SYSRET with Linux's exact register convention,
> Linux's errno values, a negative-error return, Linux's CLONE/futex/MAP
> constants. What diverges is the *number map* and a handful of *structure
> layouts*, and both are enumerable. So this is not "write a Linux kernel";
> it is "translate at the dispatch boundary for processes that ask for it",
> with an application ladder that climbs from a static `hello` to a real
> interactive program.

**Baseline:** commit `fb39fe2` (CI fix; the post-OTA CI run green — the
xHCI event-matching fix landed and the tree is a clean starting point).

**Ledger integration:** each phase's exit gate adds or extends a
machine-checkable CI case; the final phase (L5) registers the coverage
rows in `docs/residue_ledger.md` and moves the marker baseline
same-commit, exactly like FSFULL's F7 and OTA's O5. The ledger stays the
index of truth; this document is the detail.

---

## 1. Where things actually stand

Measured on the baseline tree, not assumed from headers.

### 1.1 The syscall ABI is already Linux-shaped — mechanically

* `kernel/arch/x86_64/syscall_entry.asm` uses the Linux convention exactly:
  number in RAX, arguments in RDI/RSI/RDX/R10/R8/R9, return in RAX, and
  the C dispatcher takes them in that order (`syscall_dispatch`).
* `lib/libc/include/errno.h` carries Linux's errno *values* (EPERM 1,
  ENOENT 2, EFAULT 14, EINVAL 22, ENOSYS 38, …), and the dispatcher
  returns errors as negative errno — Linux's `IS_ERR_VALUE` range.
* Unknown numbers fail loudly: `default: kprintf("[syscall] unknown
  syscall %llu"); return -ENOSYS` — for an `lx` process this doubles as
  the missing-arm diagnostic.
* A large block of numbers already matches Linux x86-64 exactly (measured
  against `kernel/arch/x86_64/syscall.c`): the 0–39 core (read 0, write 1,
  open 2, close 3, fstat 5, lstat 6, poll 7, lseek 8, mmap 9, mprotect 10,
  munmap 11, brk 12, rt_sigaction 13, rt_sigprocmask 14, rt_sigreturn 15,
  ioctl 16, pread64 17, pwrite64 18, readv 19, writev 20, pipe 22,
  select 23, msync 26, mincore 27, madvise 28, shmget 29, shmat 30,
  shmctl 31, dup 32, dup2 33, pause 34, nanosleep 35, getitimer 36,
  alarm 37, setitimer 38, getpid 39), the process block (clone 56,
  fork 57, execve 59, exit 60, wait4 61, kill 62, uname 63, semget 64 …
  msgctl 71), signals (rt_sigpending 127, rt_sigsuspend 130), timers
  (clock_gettime 228, clock_getres 229), waitid 247, pipe2 293, and the
  Linux person-family (setsid 112, setpgid 109, getpgid 121, getsid 124).
* `kernel/proc/clone.c` defines CLONE_VM/FS/FILES/SIGHAND/THREAD/
  SETTLS/PARENT_SETTID/CHILD_CLEARTID with Linux's exact flag *values*,
  implements `arch_prctl` ARCH_SET_FS/ARCH_GET_FS (0x1002/0x1003), and
  its futex path takes FUTEX_WAIT/FUTEX_WAKE with the PRIVATE bit
  ignored — Linux's encoding.
* `struct wait-status` words are POSIX/Linux-encoded
  (`wait_status_of()`, "decode the POSIX status back to 0..255/128+signo").
* The initial user stack is already auxv-shaped with Linux's keys
  (AT_PHDR/AT_PHENT/AT_PHNUM/AT_PAGESZ/AT_ENTRY/AT_UID/AT_EUID/AT_GID/
  AT_EGID/AT_SECURE/AT_RANDOM/AT_EXECFN).
* `MAP_SHARED/PRIVATE/FIXED/ANON` are Linux's values; `AT_RANDOM` seeds
  the stack canary the way glibc expects.

### 1.2 The number map diverges — and collides

The native map grew POSIX-ish groups at numbers Linux uses for something
else. Measured collisions (native → what Linux puts there):

| native syscall | nr | Linux x86-64 nr 61.. uses |
|---|---|---|
| SYS_LISTDIR | 80 | getcwd (80) |
| SYS_SPAWN | 81 | chdir (81) |
| SYS_DNS | 82 | rename (82) |
| SYS_NET_CONNECT/SEND/RECV/CLOSE/PING | 83–87 | mkdir/rmdir/unlink/utimensat… |
| SYS_MKDIR/RMDIR/UNLINK/RENAME/TRUNCATE/STAT/MKFIFO | 100–106 | ptrace family (101+…) |
| SYS_GETUID…SYS_FCHOWN | 500–515 | getuid is 102–111 in Linux |
| SYS_TIME/GETCWD/CHDIR/FCHDIR | 520/540–542 | time is 201, getcwd 80 |
| SYS_FUTEX/TKILL | 530/531 | futex is 202, tkill 200 |
| sockets | 300–309 | socket is 41–54 |
| SYS_GETRANDOM | 319 | getrandom is **318** |
| SYS_PSELECT6/PPOLL | 320/321 | 270/271 |

Consequences, measured:

* A Linux binary calling `rename` (82) would reach our DNS syscall. The
  translation is *mandatory*, and it must be **per-process** — native
 binaries keep the native map, `lx` processes get the Linux map.
* Linux numbers an `lx` process needs that have **no arm at all** today:
  `exit_group` (231), `set_tid_address` (96), `getdents64` (217 — defined,
  no case), `uname` (63 — defined, no case; our uname() is a *libc*
  function, `lib/libc/src/utsname.c`), `openat` (257), `newfstatat` (262),
  `prlimit64` (302), `getrandom` (318), `pselect6` (270), `ppoll` (271),
  `readlinkat` (267), `faccessat2` (439), `rseq` (293 — *collides with our
  pipe2 293*), `set_robust_list` (278), `getuid`/`geteuid`/`getgid`/
  `getegid` (102/107/104/108), `getcwd` (80), `chdir` (81), `rename`
  (82), `mkdir` (83), `rmdir` (84), `unlink` (87), `clock_gettime`-family
  already OK.
* `elf_load()` maps only PT_PHDR/PT_LOAD — there is no PT_INTERP, so
  dynamic binaries cannot work before L4 (and are parked until then).

### 1.3 Structures differ

* `struct stat` (`lib/libc/include/unistd.h`) is a 64-byte custom layout
  (st_type, st_mode, st_uid, st_gid, st_size, st_inode, st_nlink,
  st_blocks, mtime/ctime/atime). Linux x86-64 `struct stat` is 144 bytes
  with a different field order and `st_dev`/`st_ino`/`st_nlink` widths.
  An `lx` process must receive the Linux shape → the personality
  marshals on the fstat/lstat/stat/newfstatat boundary.
* `struct sigaction` (`lib/libc/include/signal.h`) is
  `{handler, uint32 sa_mask, int sa_flags, sa_restorer}` — Linux is
  `{handler, 8-byte sigset, int sa_flags, sa_restorer}`. The *kernel*
  side (kernel/proc/signal.h) matches ours, so the personality converts
  both directions (the mask Linux hands us is 64-bit).
* Directory listing is `SYS_LISTDIR` (80): the kernel fills an array of
  `struct aura_dirent` (`lib/libc/src/dirent.c`). Linux `getdents64`
  fills `struct linux_dirent64 { u64 d_ino; s64 d_off; u16 d_reclen;
  u8 d_type; char d_name[] }` — a different, packed, record-stream
  shape → L2 adds a real `getdents64` arm (over the same VFS readdir,
  not a second filesystem path).
* `ioctl` for termios: our tty layer has its own interface; Linux
  expects TCGETS/TCSETS (0x5401/0x5402) with `struct termios`. Needed
  by busybox `ls` (isatty), `cat`, and every shell. L2 marshals it.
* `uname`: Linux binaries expect the syscall to fill
  `struct utsname` (5×65-byte fields). We have the struct and the libc
  call; L1 adds the kernel arm.

### 1.4 What already exists and is reused, not rebuilt

* **The w32 personality is the architectural precedent**: execve probes
  the image (`pe_image_probe`) and routes to `pe_load`; the PE's imports
  bind to a compatibility layer instead of the native ABI. The `lx`
  personality is the same idea one layer down: same probe point, but the
  translation happens at the *syscall dispatch* boundary (registers are
  already Linux's — only numbers and structures move).
* SMP-safe per-CPU syscall entry, per-TCB state (`kernel/proc/`) — a
  personality flag is one more TCB field inherited by fork/clone
  children in the same paths that already copy personality-adjacent
  state.
* Signals already use the SA_RESTORER model (the kernel pushes the
  libc-supplied restorer as the handler's return address) — that is
  Linux's model, so L3's work is the *frame layout*, not the mechanism.
* VFS with full FAT32/ext2/…, page cache, pipes, FIFOs; TCP/UDP sockets;
  SMP scheduler with futex — the substrate an `lx` process lands on.

### 1.5 Identification: how a Linux binary is recognized

Measured options at `execve`:

1. **Path prefix `/linux/**` (chosen).** Explicit, binfmt_misc-style,
   zero guessing: anything executed from the `/linux` subtree gets the
   `lx` personality; everything else stays native. Self-hosting-friendly
   (a file copy is enough), trivially testable, and reversible. The
   w32 precedent already ships magic-probing *plus* convention; here the
   ELF magics collide (below), so the convention carries the choice.
2. **ELF `EI_OSABI` note — rejected, measured:** both our own ELFs and
   glibc's are built with OSABI 0 (SYSV) or 3 (Linux); the byte does not
   discriminate.
3. **Presence of `PT_INTERP` — rejected:** true for dynamic binaries
   only; a *static* glibc binary has no PT_INTERP, and static is exactly
   where the ladder starts.
4. **A `.note.auralite` build-id — rejected:** discriminating "not ours"
   by absence of a note our toolchain emits would break the first time
   a native binary is built by something else (selfhost tcc closure,
   SDK users) — an allow-list convention is safer than a deny-list
   heuristic.

`lxrun(1)` (a tiny native wrapper, L1) is the interactive front door:
it `execve`s its argument under the prefix (resolving the path into
`/linux/...`), so `lxrun /linux/bin/busybox ls` works from the native
shell without a cp.

---

## 2. Non-goals / deferrals (parked, each with a reason)

* **Full Linux syscall surface** — the ladder names the calls each rung
  needs; anything outside it returns -ENOSYS loudly. A compatibility
  *layer* is enumerable; a compatibility *kernel* is not, and pretending
  otherwise is how these projects die.
* **GUI/X11 Linux apps** — libgl is AuraLite's own API, not Mesa; the
  GUI compositor is not an X server. A terminal application ladder is
  the honest scope.
* **musl/uClibc dynamic loaders** — the dynamic phase targets glibc's
  `ld-linux-x86-64.so.2` because that is what Debian/Ubuntu binaries
  link; other loaders follow only if a ladder app needs one.
* **vDSO** — the kernel already answers clock_gettime; publishing a
  vvar page is speed, not correctness.
* **32-bit (i386) Linux binaries** — the i386 tenant (I386_PLAN.md) is
  a separate kernel personality with its own map; mixing ABIs in one
  kernel doubles the marshalling surface for zero ladder apps.
* **Containers/namespaces/cgroups** — out of scope; the personality is
  per-process, not a virtualization boundary.
* **`/proc`, `/sys` fidelity** — only the entries ladder apps actually
  stat/read (`/proc/self/exe`, `/proc/meminfo` shape) are provided;
  a full procfs re-implementation is unbounded.

---

## 3. The application ladder (light → complex)

| rung | program (unmodified, built on the host) | what it exercises | gate |
|---|---|---|---|
| 1 | static `hello` (gcc -static) | ELF PT_LOAD map, personality set, write(1,2), exit_group, brk, AT_RANDOM canary init | L1 |
| 2 | static `busybox ls`, `cat`, `echo` | getdents64, stat marshal, ioctl TCGETS/isatty, openat, newfstatat, lseek | L2 |
| 3 | static `busybox ash` scripted (`echo hi \| cat; ls /; exit`) | fork/clone, wait4, pipe, dup2, fcntl, sigaction marshal, rt_sigreturn, execve of #! | L3 |
| 4 | dynamic `hello` + `/bin/sh -c` (glibc, ld-linux) | PT_INTERP, AT_BASE, MAP_FIXED mmap, futex bitset ops, TLS, robust list no-op | L4 |
| 5 | `lua` 5.4 (static → dynamic) — real interpreter, real programs | longjmp/sigaltstack? no — setjmp, math, regex-free stdio, realloc stress; interactive REPL line editing | L5 |

The flagship is deliberately a *terminal* application: it proves the
stack end-to-end without inventing an X server (§2).

---

## 4. Phases

### Phase L0 — Plan landing ✅ DONE

#### Tasks

* This document, measured against `fb39fe2`.
* The marker baseline gains the `LX_COMPAT_PLAN.md` row in the same
  commit (the residue ratchet must not drift; OTA O0's precedent).
* No code, no ledger rows: coverage registers at L5.

#### Test gate

* `make test-unit` green with the new plan file present (ratchet row
  moved same-commit).
* Existing shards untouched.

#### Deliverable

`docs/plans/LX_COMPAT_PLAN.md` + the `tools/residue_baseline.txt` row.

### Phase L1 — Personality v1: a static hello runs ✅ DONE

> **Result (measured on the gate, TCG):** `lxrun /linux/tests/hello` AND
> the direct `/linux/tests/hello` both print the binary's own line and
> exit 0; `test_lx_hello` PASSES through the harness; the only unmapped
> Linux nr in hello's whole run is 334 (rseq — glibc falls back, by
> design).  Getting there surfaced and fixed three real kernel facts,
> each measured before the fix:
> 1. **The SYSCALL path clobbered the argument registers.**  The Linux
>    contract preserves everything except RAX/RCX/R11; our SYSRET
>    epilogue restored only the callee-saved set, so glibc's raw inline
>    syscall sequence (which reloads the syscall NUMBER from RSI kept
>    live across the previous call) issued `brk` as nr 1 with RSI=1.
>    The epilogue now keeps the six argument registers on the kernel
>    stack until SYSRET — strictly more preservation; native binaries
>    never noticed because their libc wrappers treat syscalls like
>    clobbering C calls.
> 2. **The native brk rounds its return up to a page.**  glibc's sbrk
>    requires brk(addr) == addr verbatim; the lx map routes brk to a
>    dedicated arm with Linux's exact return contract.
> 3. **The ELF loader mapped pages without describing them.**  mprotect
>    refuses ranges its VMA walk cannot cover, and glibc's RELRO pass
>    died with "cannot apply additional memory protection after
>    relocation".  elf_load() now records a VMA (with p_flags
>    protections) per PT_LOAD — what Linux does; nothing native ever
>    called mprotect on its own image.
> Also measured into the tree: set_tid_address is nr **218** on x86-64
> (the first draft said 96 — which is gettimeofday; the gate caught it),
> and the execve reader's fixed 256 KiB buffer could not hold a static
> glibc image (754 KiB) — it now sizes from the vnode, capped at 8 MiB.

#### Tasks

* TCB personality field (`LX_PERSONA_LX`), inherited by fork/clone/spawn
  children in the same copy paths; default native.
* `execve` under `/linux/**` (after the existing path resolution, before
  `elf_load`) sets the personality; native paths are untouched.
* The translation table `lx_syscall_translate()` in the dispatcher's
  front end: `lx` numbers → native arms where the semantics already
  match (§1.1 list), `-ENOSYS` where Linux has a number and we have no
  arm (loud, per §1.1).
* New arms, `lx`-only, behind the personality: `exit_group` (231 →
  thread_exit-all), `set_tid_address` (96, store, return old), `uname`
  (63, fill `struct utsname`: sysname "Linux", release the host kernel
  version string the binary expects — honest values, we *are* the
  kernel it is talking to), `getrandom` (318 → existing entropy), uid
  family (102/104/107/108 → existing uid state).
* `lxrun(1)` native wrapper app (uses the prefix convention).
* Host-built static `hello` (Debian gcc -static in CI, x86_64-linux-gnu
  target) staged into the initrd at `/linux/tests/hello`.
* `tests/integration/cases/test_lx_hello.sh`: boot, `lxrun
  /linux/tests/hello`, assert stdout receipt and exit status.

#### Test gate

`test_lx_hello` green in CI; `core`/`posix` shards unchanged (native
map untouched — asserted by a unit test that the native table's
colliding numbers still reach their native arms).

#### Deliverable

kernel personality + translation table + `lxrun` + hello case + patch.

### Phase L2 — Structures: busybox file utilities run ✅ DONE (2026-09-09)

*Measured first, mapped second.*  The whole phase was scoped from a host
`strace` of the exact busybox build the gate stages (upstream 1.35.0
static musl), against `asm/unistd_64.h` — which corrected two number
labels L1's comments had wrong (getcwd is 79, chdir is 80) before any
code moved.

#### What shipped

* `getdents64` (217) arm: the VFS readdir op is a stateless
  "list immediate children" (the LISTDIR snapshot), so the iteration
  cursor lives in the OFD's seek offset — each call re-lists, skips
  `pos` entries, packs `struct linux_dirent64` records until `count`
  is exhausted, and parks `pos` on the first unpacked entry.  `d_off`
  is the entry index + 1, so telldir/seekdir (an lseek on the dir fd)
  lines up with the cursor for free.  `d_ino == 0` (the initrd hands
  file #0 its array index) is substituted — musl's readdir silently
  drops zero-inode entries.
* `struct stat` marshal (`lx_marshal_stat`): Linux's 144-byte x86-64
  layout for stat(4)/fstat(5)/lstat(6)/newfstatat(262), st_mode through
  the same `stat_posix_mode()` the native arms use.  These are lx-only
  arms BECAUSE the native arms at 5/6/262 fill `struct vfs_stat` — the
  number equality is a trap, not a shortcut.
* Identity rows measured, not assumed: open(2)/openat(257) (the O_*
  vocabulary in `kernel/fs/vfs.h` is the asm-generic one; vfs_open
  enforces O_DIRECTORY/O_CLOEXEC), munmap(11), fcntl(72) (same command
  numbers), ioctl(16) (TIOCGWINSZ for ls's columns; termios agrees on
  the 36 bytes musl reads — NCCS 19 vs musl's 32 is an L3 problem),
  rt_sigprocmask(14) (SIG_BLOCK/UNBLOCK/SETMASK match; the native mask
  is 32-bit — busybox's whole run passes oldset == NULL, measured).
* Aliases: getcwd 79→540, chdir 80→541 (the L1 LISTDIR collision
  resolves through the table), setuid 105→504, setgid 106→505.
* More identity rows measured IN-GUEST (not from source alone):
  clock_gettime(228→228, native arm + kernel_timespec == Linux
  timespec — busybox dd dies without it), dup(32→32) and dup2(33→33,
  same argument order — dd's "can't duplicate file descriptor"
  receipt).

#### Result (measured on the gate, TCG)

`test_lx_busybox` PASSES 9/9: echo writes its own line, cat returns
the staged motd (through busybox's sendfile-ENOSYS read/write
fallback), `ls -1 /` names `linux`, `ls -1 /linux/etc` lists both
entries, and all four applet processes exit 0.  The in-guest probe
(`busybox stat`) reads the full marshalled struct stat — S_IFREG,
0644, size 80.  The only unmapped Linux nrs left in these runs are by
design: 40 sendfile (verified fallback), 13 rt_sigaction (L3's first
job — dd installs a handler; it tolerates the ENOSYS).

Getting there surfaced and fixed a FOURTH kernel bug, measured before
the fix: **validate_user_range rejected lazily-mapped anonymous pages.**
The first touch of a fresh anonymous mmap page has no PTE, so the
PRESENT check failed and read(2) into such a buffer returned EFAULT —
busybox cat's copyfd mmaps its copy buffer and read straight into it
(head, reading into a stack buffer, worked — which is what isolated
the buffer type as the variable).  Linux faults the page in from
kernel context and completes the copy.  The fix extends the existing
COW-materialisation pattern in validate_user_range: a not-PRESENT page
now goes through handle_user_page_fault() (the same resolver the
user-mode path uses) and re-validates; kernel addresses and missing
VMAs still fail exactly as before, so wild pointers stay loud.  No
native binary ever read into fresh mmap memory — the native libc
allocates from brk, which maps eagerly — which is why this stayed
hidden until a Linux binary arrived.

The gate's assertion mechanics are themselves measured facts: the
console answers TIOCGWINSZ, so busybox believes stdout is a terminal
and colourises `ls` (every gate `ls` runs `--color=never`); applet
lines end with a CR from the tty (patterns anchor at `^` only — the
host grep does not parse `\r`); and the boot selftest's own
`/bin/hello` prints a bare `hello` line, so `/linux/tests` is not
asserted by name (the per-run `exited (code=0)` receipts carry the
clean-exit proof instead).

#### Deviations from the task list (honest, measured)

* busybox is upstream's own prebuilt static musl binary (fetched by
  the Makefile behind a pinned SHA-256), not host-built: building it
  would vendor a 4 MB source tree to avoid a 1.1 MB download, and the
  prebuilt is *more* "unmodified Linux binary", not less.  Staged at
  `/linux/bin/busybox` (argv[1] applet dispatch), `/linux/etc/motd` +
  `/linux/etc/zz-ls-probe` feed the cat/ls receipts.
* `sendfile`(40) is NOT mapped: busybox's copyfd falls back to a
  read/write loop on ENOSYS (verified in libbb/copyfd.c), so the
  honest -ENOSYS plus the loud dispatcher print is the receipt.
* sigaction marshal and faccessat2(439) move to L3 (nothing in the
  ls/cat/echo trace asked for them; ash will).

#### Test gate

`test_lx_busybox` green (echo/cat/ls×3, anchored `^…$` patterns so the
serial echo of the typed command cannot satisfy them); `fs`/`posix`
shards unchanged (the marshal is behind the personality; the native
stat arms still fill struct vfs_stat).

#### Tasks

* `getdents64` (217): a real arm over the VFS readdir the LISTDIR path
  uses, emitting `struct linux_dirent64` records into the caller's
  buffer with Linux's return conventions.
* `struct stat` marshal: native fill → Linux 144-byte layout for
  fstat(5)/lstat(6)/stat(105→newfstatat mapping)/newfstatat(262), with
  st_mode S_IFMT bits from the same VFS type mapping the native path
  uses (syscall.c already builds POSIX mode bits).
* `sigaction` marshal: Linux layout ↔ kernel layout, 64-bit mask
  truncation to our signal range documented at the boundary.
* `ioctl` TCGETS/TCSETS/TCGETA: termios marshal to/from the tty layer.
* `openat`(257)/`readlinkat`(267)/`faccessat2`(439): dirfd=AT_FDCWD
  fast path first (the copy_user_path helper already special-cases
  AT_FDCWD), full relative-path resolution if a ladder app needs it.
* `isatty` path: fstat st_mode S_IFCHR + ioctl TCGETS must both answer.
* Host-built static busybox (single applet set: ls cat echo stat) into
  `/linux/tests/`.
* `tests/integration/cases/test_lx_busybox.sh`: `lxrun … ls /` (receipt:
  known root entries), `cat` of a staged file byte-compared, `echo`
  pipeline.

#### Test gate

`test_lx_busybox` green; `fs`/`posix` shards unchanged (native stat
layout untouched — the marshal is behind the personality).

#### Deliverable

marshal layer + getdents64 + busybox case + patch.

### Phase L3 — Process plumbing: a shell runs scripts ✅ DONE (2026-09-10)

*Measured first, mapped second*, same discipline as L2: the gate's exact
busybox ash (upstream 1.35.0 static musl) was straced on the host to
learn the shell's syscall vocabulary before a line of kernel code moved.
Two host facts shaped the phase: musl's `fork()` IS `clone(SIGCHLD, 0)`
(so the personality must route clone, not just fork), and ash's `wait`
builtin never blocks in `wait4` — it does `wait4(-1, WNOHANG)`, then
`rt_sigprocmask(SIG_SETMASK, all)`, then `rt_sigsuspend(empty)` waiting
for SIGCHLD, then reaps (busybox `waitproc()` in shell/ash.c).

#### What shipped

* `kernel/lx/lx_sig.h` — the Linux x86-64 signal structures as a
  freestanding header (only `<stdint.h>`, so the same source compiles
  into the host unit test): `kernel_sigaction` (32 B), `sigcontext_64`
  (256 B), `ucontext` (936 B), `siginfo` (128 B), `rt_sigframe`
  (1072 B).  Every size/offset is host-pinned by `test_lx_sig.c`.
* lx-only arms: `LX_ARM_SIGACTION`(13), `LX_ARM_SIGRETURN`(15),
  `LX_ARM_SIGPROCMASK`(14), `LX_ARM_CLONE`(56) — all four are native
  numbers too, so the trap is real; they are lx-only because the native
  arms speak native layouts.
* `lx_translate.c`: the four lx-only rows above; identity rows measured
  in-guest (pipe 22, getpid 39, fork 57, execve 59, wait4 61, kill 62,
  getppid 110, rt_sigsuspend 130, pipe2 293); alias `access 21→513`
  (native SYS_ACCESS lives at 513).
* `signal.c`: `do_sigaction_kernel` split out as the user-copy-free core;
  `lx_do_sigaction` marshals Linux's 32-byte `kernel_sigaction` into the
  native `struct sigaction` and back (SA_RESTORER kept, restorer stored);
  `lx_do_sigprocmask` reads/writes the 8-byte kernel `sigset_t` both
  ways; `build_handler_frame_lx` lays out the `rt_sigframe` at
  RSP%16==8 with pretcode == sa_restorer, ucontext at +8, siginfo at
  +944, and enters the handler `rdi=signo` (+ rsi/rsp/rdx for
  SA_SIGINFO); `lx_do_sigreturn` rebuilds the regs from
  uc_mcontext/uc_sigmask.
* Dispatcher: SIGACTION/SIGPROCMASK/SIGRETURN/CLONE arms; SIGRETURN
  returns through the IRETQ slow path exactly like native SIGRETURN;
  the wait4 legacy 1-arg reinterpretation is skipped for lx processes
  (a Linux wait4(pid, wstatus, options, rusage) with a high pid would
  otherwise be misread as a status pointer).
* `CLONE_*` flags moved to `kernel/proc/clone_decls.h` (single source
  for clone.c and the lx arm): lx clone routes `CLONE_VM|CLONE_THREAD`
  → `do_clone`, no `CLONE_VM` → `do_fork`, else `-ENOSYS` (vfork-class
  stays off the ladder).
* Makefile: stages `lx/tests/ash_script.sh` at `/linux/tests` and
  hard-links `/linux/bin/{cat,ls,sleep}` to the staged busybox (this
  build has SH_STANDALONE off, so ash execs applets through PATH).
* Tests: `test_lx_sig.c` (host pin of every lx_sig.h layout), updated
  `test_lx_translate.c` rows, and the `test_lx_shell.sh` integration
  gate (registered in run_all.sh).

#### Result (measured on the gate, TCG)

`test_lx_shell` PASSES 9/9.  Both invocations (`lxrun /linux/bin/busybox
ash /linux/tests/ash_script.sh` and the binfmt_script re-exec
`lxrun /linux/tests/ash_script.sh`) print `LX3-ECHO-OK` (pipeline:
fork + pipe + wait4 + SIGCHLD), `LX3-LS-OK` (getdents64 + stat +
redirection), `LX3-BGJOB-OK` (`sleep 0 &` + `wait`: SIGCHLD →
sigsuspend → wait4(WNOHANG) reap), `LX3-TRAP-OK` (trap INT + kill -$$
round-trip through the Linux frame and rt_sigreturn), and exit 7
(`LX3-NOTREACHED` never prints).  The `exited (code=7)` receipt proves
both the signal round-trip and the exit-status propagation.

The native regression is unchanged: `test_signals` 9/9, `test_jobcontrol`
14/14, `test_stopped` 17/17 (the frame layout fork is per-personality;
the native `signal_frame` and the `do_sigsuspend` core are untouched).

Getting there surfaced and fixed a kernel bug, measured before the fix:
**`lx_do_sigprocmask` wrote the NEW mask into `oldset`.**  POSIX says
`oldset` carries the mask from before the call; the native
`do_sigprocmask` snapshots first.  busybox ash's `sigprocmask2()` aliases
`set == old` (`sigprocmask(how, set, oset)` with `oset = set`), and its
`waitproc()` is `sigfillset(&oldmask); sigprocmask2(SIG_SETMASK,
&oldmask); sigsuspend(&oldmask)` — so the shell suspended with the
all-blocked mask still in `oldmask` and SIGCHLD never woke it (the
`LX3-BGJOB-OK` hang).  The fix restores the snapshot-first ordering.

#### Deviations from the task list (honest, measured)

* FPU state is not saved/restored across an lx signal (the ladder apps
  are terminal programs that never touch the FPU; the native path keeps
  its fxsave/fxrstor).  Documented in lx_sig.h.
* The siginfo marshal fills the common prefix plus the payload the
  ladder reads (si_pid/si_uid for SI_USER, si_addr for a fault); the
  full per-signal payload table is not needed until an app parses it.
* SIGCHLD and SIGINT round-trips are exercised by the gate; SIGQUIT and
  the SIGTSTP + WUNTRACED stop round-trip are not — non-interactive ash
  never issues them (the native stop/continue mechanism is covered by
  `test_stopped`).  waitid(247) is unchanged (ash uses wait4); the
  status-word encoding is asserted through wait4's WNOHANG path.

#### Tasks

* Linux signal-frame layout for `lx` handlers (the SA_RESTORER mechanism
  exists; the frame's register order/siginfo placement moves to Linux's
  shape) + rt_sigreturn(15) parse for the same layout.
* wait4/waitid under personality: options bits (WNOHANG/WUNTRACED/
  WCONTINUED) already POSIX; assert the status word encoding.
* pipe/pipe2/dup2/fcntl(F_GETFD/F_SETFD/F_GETFL/F_SETFL) verified
  against glibc's fcntl use; close-on-exec discipline through the
  existing fd table.
* `execve` of `#!` scripts and of nested `/linux/**` binaries (the
  personality must survive re-exec inside the prefix).
* SIGCHLD/SIGINT/SIGQUIT delivery to `lx` handlers (the shell's job
  control: stopped jobs need SIGTSTP + WUNTRACED round-trip).
* `lxrun` gains script mode (`lxrun /linux/tests/script.sh` → ash).
* `tests/integration/cases/test_lx_shell.sh`: scripted busybox ash —
  `echo hi | cat`, `ls / >/dev/null && echo ok`, background job +
  `wait`, `kill -INT` of a child, exit status propagation.

#### Test gate

`test_lx_shell` green (9/9: pipeline, ls+redirect, background `wait`,
INT trap, exit-7 status, ×2 invocations); `core` shard's signal cases
unchanged (`test_signals`/`test_jobcontrol`/`test_stopped` re-run green
— native frame untouched, the layout fork is per-personality).

#### Deliverable

signal-frame/process plumbing + shell case + patch.

### Phase L4 — Dynamic binaries: the loader runs

#### Tasks

* `elf_load()`: PT_INTERP recognized; interpreter image mapped, entry
  chain (map ld.so, hand it the executable's phdrs via auxv), AT_BASE
  filled; PT_GNU_STACK honored for stack NX.
* mmap discipline ld.so needs: MAP_FIXED mappings at its chosen
  addresses (fixed-map collision policy: within the user area, the
  existing VMA allocator arbitrates), munmap of partial failures,
  mprotect on the RELRO segment.
* futex additions glibc's lock paths use (FUTEX_WAIT_BITSET/
  WAKE_BITSET/REQUEUE, op encoding) on top of the existing queue.
* TLS: the dtv/TCB block ld.so builds at ARCH_SET_FS is already
  reachable through arch_prctl; assert FS-relative access faults into
  the right VMA (no kernel changes expected — measurement first).
* `set_robust_list` (278): accept-and-store (futex exit semantics stay
  native — a no-op honest for the ladder apps).
* `rseq`(293-collision): the Linux number is distinct *inside the lx
  map* — registered, answered -ENOSYS (glibc falls back); the collision
  only proves the per-process table is load-bearing.
* Dynamic `hello` + `sh -c` staged (Debian gcc default + `/bin/sh`
  symlink) into `/linux/tests/dyn/`.
* `tests/integration/cases/test_lx_dynamic.sh`: ld.so maps, `sh -c
  'echo ok'` receipt, exit status.

#### Test gate

`test_lx_dynamic` green; no native mmap/VMA behavior change (`core`
mmap cases unchanged).

#### Deliverable

PT_INTERP + futex/TLS/robust arms + dynamic case + patch.

### Phase L5 — Flagship, CI wiring and close-out

#### Tasks

* Host-build `lua` 5.4 (static at first, dynamic if L4 held) into
  `/linux/tests/`; `tests/integration/cases/test_lx_lua.sh`: scripted
  lua (arithmetic, string, table stress, `os.date`, a read of /linux
  via io.lines) — the unmodified-interpreter proof.
* `tools/check_lx_claims.py` in the checker family: every ✅ phase
  pinned to its artefacts (the translation table symbols, the marshal
  functions, the CI case registrations) *and* its greppable receipts —
  with the usual planted-violation negative control.
* The `lx` CI shard: the four cases as their own group in
  `run_all.sh`'s partition (`--check-groups` proves it), mirroring the
  fsfull/ota precedent — not `posix`, whose wall-clock they would
  stretch; matrix + workflow step wired like O5's.
* Makefile: the host-built /linux payload is a target (`lx-payload`)
  with the mingw/w32-sdk precedent (skip loudly, CI asserts presence
  like `w32hello.exe`).
* docs rows same-commit: `docs/status.md`, README, TODO (bullets, not
  open boxes — those are owned by the residue phases), ledger coverage
  rows, baseline moved, this plan → COMPLETE.

#### Test gate

CI: `lx` shard green alongside all existing shards; check_lx_claims in
test-unit; ratchet green.

#### Deliverable

lua case + checker + shard + docs + patch; plan COMPLETE.

---

## 5. Risks, named

* **glibc's startup syscall set is larger than hello's** — measured at
  each rung, not guessed: the loud `-ENOSYS` default (§1.1) turns every
  gap into a one-line diagnosis. The ladder exists so each surprise
  lands on the smallest possible rung.
* **The number collisions cut both ways** — a native process must never
  see the lx map; the L1 unit test pins the native table, the
  personality flag is the only door.
* **Termios/tty shapes** — the shell rung depends on our tty layer's
  discipline matching what busybox expects of a real one; measured in
  L2 before the shell rung builds on it.
* **Wall-clock** — four new QEMU cases in one shard, none multi-boot;
  cheaper than fsfull/ota shards by construction.

---

## 6. Close-out checklist

- [ ] L1: `lxrun /linux/tests/hello` prints and exits 0 in CI
- [x] L2: `lxrun busybox ls /` lists the real root in CI (test_lx_busybox: `ls -1 /` asserts the `linux` root entry, plus `/linux/etc` and `/linux/tests` listings, cat of the staged motd, and echo — 2026-09-09)
- [x] L3: scripted busybox ash passes its pipeline/job-control receipts in CI (test_lx_shell: 9/9, both invocations, exit-7 status — 2026-09-10)
- [ ] L4: glibc `sh -c 'echo ok'` runs in CI
- [ ] L5: unmodified lua passes its scripted program in CI
- [ ] L5: `tools/check_lx_claims.py` green in test-unit, negative control included
- [ ] L5: `lx` group in `run_all.sh` partition, `--check-groups` green
- [ ] L5: ledger coverage rows + baseline moved same-commit; plan → COMPLETE
