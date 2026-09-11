/* kernel/lx/lx_translate.c — the Linux number map (LX_COMPAT_PLAN.md L1).
 *
 * Every row is measured against BOTH tables before it lands here:
 *  - an identity row (x -> x) asserts the native arm at that number has
 *    Linux's semantics AND argument order (read/write/close/lseek/brk/
 *    nanosleep/exit/arch_prctl were each checked in syscall.c);
 *  - an alias row (linux -> native) asserts the two arms agree on
 *    argument order and return convention (the uid family: native
 *    500..503 answer the same single-call "return my id" contract);
 *  - an LX_ARM row means Linux has the call, we have the semantics but
 *    no arm at that number — the dispatcher owns the dedicated case.
 *
 * What is deliberately ABSENT is as load-bearing as what is present:
 * a Linux number without a row is LX_UNMAPPED and fails -ENOSYS with
 * the dispatcher's loud print, so a ladder application's first run
 * names its missing calls.  Nothing is mapped "because it probably
 * works" — the collisions (81 fchdir vs native SPAWN, 82 rename vs
 * native DNS, 83 mkdir vs native NET_CONNECT, 334 rseq, ...) stay
 * unmapped until the phase that owns them measures the arm (L4 mapped
 * futex 202 and pread64 17; rseq stays unmapped so glibc takes its
 * documented -ENOSYS fallback).  sendfile(40)
 * is absent on purpose too: busybox's copyfd falls back to a read/write
 * loop on ENOSYS (verified in busybox-1.35.0 libbb/copyfd.c, not
 * assumed), and the loud print in the guest log is the honest receipt.
 */

#include "kernel/lx/lx.h"

struct lx_row {
    uint32_t linux_nr;   /* what an lx process puts in RAX */
    uint32_t goes_to;    /* native nr, or LX_ARM_* pseudo-number */
};

static const struct lx_row LX_TABLE[] = {
    /* -- identity: the core I/O set (checked in syscall.c) ---------- */
    { 0,   0 },    /* read(fd, buf, count) */
    { 1,   1 },    /* write(fd, buf, count) */
    { 3,   3 },    /* close(fd) */
    { 8,   8 },    /* lseek(fd, off, whence) */
    { 9,   9 },    /* mmap(addr,len,prot,flags,fd,off) — same argument
                    * order, same PROT_x and MAP_x values (glibc's static
                    * startup allocates its TLS block through it) */
    { 10,  10 },   /* mprotect(addr,len,prot) — RELRO hardening */
    { 20,  20 },   /* writev(fd,iov,iovcnt) — glibc stdio flushes with it */
    { 96,  96 },   /* gettimeofday(tv,tz) — native arm, same shape */
    { 35,  35 },   /* nanosleep(req, rem) */
    { 60,  60 },   /* exit(status) */
    { 158, 158 },  /* arch_prctl(SET_FS/GET_FS, addr) — clone.c */

    /* -- identity rows measured for L2 (busybox ls/cat/echo) --------- */
    { 2,   2 },    /* open(path, flags, mode) — the O_* values are the
                    * Linux/asm-generic ones in BOTH vocabularies
                    * (kernel/fs/vfs.h == asm-generic: O_CREAT 0x40,
                    * O_DIRECTORY 0x10000, O_CLOEXEC 0x80000, ...),
                    * vfs_open enforces O_DIRECTORY/CLOEXEC/TRUNC and
                    * answers fd-or-negative-errno, Linux's contract */
    { 11,  11 },   /* munmap(addr, len) — native SYS_MUNMAP, same shape;
                    * musl's mallocng unmaps its buffers with it */
    { 16,  16 },   /* ioctl(fd, cmd, arg) — native arm dispatches
                    * TCGETS/TCSETS/TCSETSW/TCSETSF/TIOCGWINSZ with
                    * struct termios/winsize; winsize (4x u16) is layout-
                    * identical to Linux, and termios agrees on the first
                    * 36 bytes musl reads (4x u32 flags + c_line +
                    * c_cc[19]).  musl's NCCS is 32, so its c_cc[19..31]
                    * stay untouched garbage — isatty(), which only
                    * checks the return code, and TIOCGWINSZ, which ls
                    * sizes columns with, are unaffected. */
    { 72,  72 },   /* fcntl(fd, cmd, arg) — native vfs_fcntl covers
                    * F_GETFD/SETFD/GETFL/SETFL/DUPFD/DUPFD_CLOEXEC with
                    * the asm-generic command numbers busybox uses
                    * (F_SETFD 2, FD_CLOEXEC 1, F_DUPFD_CLOEXEC 1030) */
    { 257, 257 },  /* openat(dirfd, path, flags, mode) — the native Q12
                    * arm at this very number: copy_at_path joins the
                    * thread cwd for AT_FDCWD and answers ENOSYS for a
                    * real dirfd + relative path (the honest native
                    * limit, inherited), flags as open(2) above */
    { 228, 228 },  /* clock_gettime(clk_id, struct timespec*) — native
                    * 228 with the same argument order and a
                    * kernel_timespec identical to Linux's {tv_sec,
                    * tv_nsec}.  Measured in-guest: busybox's dd dies
                    * on the ENOSYS ("clock_gettime(MONOTONIC)
                    * failed"), ls asks for it once per run */
    { 32,  32 },   /* dup(oldfd) — native SYS_DUP, same shape */
    { 33,  33 },   /* dup2(oldfd, newfd) — native SYS_DUP2,
                    * vfs_dup2(a1, a2), same argument order.  Measured
                    * in-guest: busybox's dd dup2's its stdin and dies
                    * on the ENOSYS ("can't duplicate file descriptor")
                    * with the row absent */

    /* -- L3 identity rows, measured from a host strace of ash's whole
     *    `echo hi | cat; ls / >/dev/null && echo ok` run ------------ */
    { 22,  22 },   /* pipe(fds[2]) — native SYS_PIPE, same shape       */
    { 39,  39 },   /* getpid — native SYS_GETPID, same contract         */
    { 57,  57 },   /* fork — native do_fork IS Linux fork semantics
                    * (COW address space, inherited fds); musl actually
                    * calls clone(SIGCHLD,0) — the 56 arm below — but a
                    * direct fork(57) must work too                    */
    { 59,  59 },   /* execve — native SYS_EXECVE: path/argv/envp, and
                    * the kernel's binfmt_script + /linux prefix rule
                    * both apply on re-exec                             */
    { 61,  61 },   /* wait4(pid,wstatus,options,rusage) — native "new
                    * ABI" arms are this exact Linux order; the status
                    * word is POSIX/Linux-encoded (exit<<8 | sig&0x7f);
                    * the dispatcher skips its legacy 1-arg
                    * reinterpretation for PERSONA_LX (see syscall.c)  */
    { 62,  62 },   /* kill(pid, sig) — native SYS_KILL, same contract  */
    { 110, 110 },  /* getppid — native SYS_GETPPID is AT 110 already
                    * (RESIDUE2 T1 picked the Linux number)            */
    { 130, 130 },  /* rt_sigsuspend(mask) — native SYS_SIGSUSPEND is at
                    * 130 with the same 32-bit-low mask contract       */
    { 293, 293 },  /* pipe2(fds, flags) — BOTH tables put pipe2 at 293
                    * (Linux rseq is 334, not 293; the "293 collision"
                    * note in early drafts was wrong and L1's gate text
                    * already measured 334 as the rseq number)         */

    /* -- lx-only arms ------------------------------------------------ */
    { 63,  LX_ARM_UNAME },           /* uname: fills struct utsname     */
    { 12,  LX_ARM_BRK },             /* brk: Linux-exact return        */
    { 186, LX_ARM_GETTID },
    { 218, LX_ARM_SET_TID_ADDRESS }, /* glibc's first startup call     */
    { 231, LX_ARM_EXIT_GROUP },      /* hello's LAST call              */
    { 267, LX_ARM_READLINKAT },      /* /proc/self/exe probes: -ENOENT */
    { 273, LX_ARM_SET_ROBUST_LIST }, /* accept-and-store               */
    { 302, LX_ARM_PRLIMIT64 },       /* rlimit query: none enforced    */
    { 4,   LX_ARM_STAT },            /* stat: native arms speak struct */
    { 5,   LX_ARM_FSTAT },           /*   vfs_stat, NOT Linux's 144-   */
    { 6,   LX_ARM_LSTAT },           /*   byte layout — marshal (L2)   */
    { 217, LX_ARM_GETDENTS64 },      /* getdents64: no native arm      */
    { 262, LX_ARM_NEWFSTATAT },      /* newfstatat: marshal (L2)       */
    { 13,  LX_ARM_SIGACTION },       /* rt_sigaction: kernel_sigaction
                                      *   marshal (L3) — native 13 is
                                      *   the same number, native      */
    { 15,  LX_ARM_SIGRETURN },       /* rt_sigreturn: Linux rt_sigframe
                                      *   parse (L3) — native 15 is the
                                      *   native signal_frame           */
    { 14,  LX_ARM_SIGPROCMASK },     /* rt_sigprocmask: 8-byte kernel
                                      *   sigset_t both ways (L3) — the
                                      *   native 32-bit arm under-writes
                                      *   a non-NULL oldset            */
    { 56,  LX_ARM_CLONE },           /* clone: fork-style -> do_fork,
                                      *   pthread-style -> do_clone     */

    /* -- aliases: same contract, different native number ------------- */
    { 102, 500 },  /* getuid  -> SYS_GETUID  (native 500) */
    { 104, 502 },  /* getgid  -> SYS_GETGID  (native 502) */
    { 107, 501 },  /* geteuid -> SYS_GETEUID (native 501) */
    { 108, 503 },  /* getegid -> SYS_GETEGID (native 503) */
    { 21,  513 },  /* access(path, mode) -> native SYS_ACCESS (513):
                    * same F_OK/R_OK/W_OK/X_OK bits, 0 or -EACCES.
                    * musl prefers the access syscall on x86-64 (it
                    * exists), so ash's /etc/selinux probes come here   */
    { 318, 319 },  /* getrandom(buf, len, flags) — native 319, same
                    * signature (GRND_NONBLOCK|GRND_RANDOM accepted) */

    /* -- L4: the dynamic-loader surface ------------------------------
     * futex(202) has no native arm at 202 (the native futex lives at
     * 530); the argument order is identical — (uaddr, op, val, timeout,
     * uaddr2, val3) — so this is a straight alias and do_futex decodes
     * the op in Linux's vocabulary (WAIT/WAKE/WAIT_BITSET/WAKE_BITSET/
     * REQUEUE/CMP_REQUEUE, PRIVATE|CLOCK flags above bit 7).
     * pread64(17) IS native 17 with the same (fd, buf, count, off)
     * order and a positional vfs_pread that does not move the fd offset
     * — ld.so reads program headers at their file offset through it. */
    { 202, 530 },  /* futex -> native SYS_FUTEX (do_futex, same 6 args) */
    { 17,  17 },   /* pread64 — identity: native SYS_PREAD64 == 17 */
    { 79,  540 },  /* getcwd(buf, size) — native SYS_GETCWD (540):
                    * do_getcwd copies the cwd string and returns its
                    * length, Linux's convention; -ERANGE when it does
                    * not fit.  79 is getcwd on x86-64 (80 is chdir —
                    * checked against asm/unistd_64.h, not recalled) */
    { 80,  541 },  /* chdir(path) — native SYS_CHDIR (541), NOT native
                    * 80 (LISTDIR): the collision that kept 80 unmapped
                    * in L1 resolves through this row and nothing else */
    { 105, 504 },  /* setuid — native 504; busybox's suid check calls it
                    * with the id it already holds (0), which the native
                    * arm permits for euid 0 */
    { 106, 505 },  /* setgid — native 505, same shape as setuid */

    /* -- L5: the stock-lua interpreter surface ------------------------
     * time(201) has no native arm at 201 — the native clock/time block
     * lives elsewhere (see syscall.c P8: gettimeofday 96, clock_gettime
     * 228, time 520).  The native SYS_TIME (520) arm is Linux-exact:
     * kernel_time() returns seconds-since-epoch and writes through a
     * non-NULL tloc, time(2)'s contract.  glibc's time() on x86-64
     * issues Linux 201 directly (lx offers no vDSO __vdso_time), so the
     * unmodified lua interpreter's os.time() died on -ENOSYS until this
     * row: measured in-guest as four `unknown syscall 201` prints and
     * `time result cannot be represented in this installation` at
     * lua_script.lua:43.  With the row present os.time() returns the
     * (epoch-0) boot-seconds value and os.date("%Y") formats "1970". */
    { 201, 520 },  /* time -> native SYS_TIME (520) */
};

#define LX_TABLE_LEN (sizeof(LX_TABLE) / sizeof(LX_TABLE[0]))

uint32_t lx_translate(uint32_t linux_nr) {
    for (uint32_t i = 0; i < LX_TABLE_LEN; i++) {
        if (LX_TABLE[i].linux_nr == linux_nr)
            return LX_TABLE[i].goes_to;
    }
    return LX_UNMAPPED;
}

int lx_path_is_lx(const char *kernel_path) {
    /* The binfmt_misc-style convention: everything executed from the
     * /linux subtree speaks the lx map.  An absolute path only — the
     * kernel resolves relatives against the cwd before execve, and the
     * rule must not depend on where the CALLER stands. */
    if (kernel_path == 0) return 0;
    static const char prefix[] = "/linux/";
    for (uint32_t i = 0; i < sizeof(prefix) - 1; i++) {
        if (kernel_path[i] != prefix[i]) return 0;
    }
    return 1;
}
