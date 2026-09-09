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
 * works" — the collisions (80 getcwd, 82 rename, 293 rseq, ...) stay
 * unmapped until the phase that owns them measures the arm (L2: the
 * *at family and getdents64; L4: futex bitsets, rseq).
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

    /* -- lx-only arms ------------------------------------------------ */
    { 63,  LX_ARM_UNAME },           /* uname: fills struct utsname     */
    { 12,  LX_ARM_BRK },             /* brk: Linux-exact return        */
    { 186, LX_ARM_GETTID },
    { 218, LX_ARM_SET_TID_ADDRESS }, /* glibc's first startup call     */
    { 231, LX_ARM_EXIT_GROUP },      /* hello's LAST call              */
    { 267, LX_ARM_READLINKAT },      /* /proc/self/exe probes: -ENOENT */
    { 273, LX_ARM_SET_ROBUST_LIST }, /* accept-and-store               */
    { 302, LX_ARM_PRLIMIT64 },       /* rlimit query: none enforced    */

    /* -- aliases: same contract, different native number ------------- */
    { 102, 500 },  /* getuid  -> SYS_GETUID  (native 500) */
    { 104, 502 },  /* getgid  -> SYS_GETGID  (native 502) */
    { 107, 501 },  /* geteuid -> SYS_GETEUID (native 501) */
    { 108, 503 },  /* getegid -> SYS_GETEGID (native 503) */
    { 318, 319 },  /* getrandom(buf, len, flags) — native 319, same
                    * signature (GRND_NONBLOCK|GRND_RANDOM accepted) */
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
