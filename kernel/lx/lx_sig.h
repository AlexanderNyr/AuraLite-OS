#ifndef AURALITE_KERNEL_LX_SIG_H
#define AURALITE_KERNEL_LX_SIG_H

/*
 * kernel/lx/lx_sig.h — the Linux x86-64 signal structures (LX_COMPAT_PLAN.md
 * L3), the third marshal in the personality: after stat (L2) and the number
 * map (L1), the signal frame and the rt_sigaction struct get their Linux
 * shapes.
 *
 * Every layout below is measured against the Linux x86-64 ABI (asm/sigcontext.h,
 * asm/signal.h, kernel siginfo), not recalled.  The discipline is the same as
 * L2's struct stat: the NATIVE arms at the same syscall numbers speak native
 * layouts (native sigaction is {handler, u32 mask, flags, restorer}; the
 * native signal_frame is ~640 bytes of our own design), so an lx process must
 * never reach them — the number equality (13/15 are rt_sigaction/rt_sigreturn
 * in BOTH tables) is a trap, not a shortcut.
 *
 * This header is freestanding-pure (only <stdint.h>) so the same source
 * compiles into the host unit test tests/unit/test_lx_sig.c, which pins every
 * size and offset below — the same "one definition, host-pinned" pattern the
 * L2 marshal used.
 */

#include <stdint.h>

/* ---- rt_sigaction(13): the kernel-side struct the syscall reads/writes ----
 * glibc and musl both convert their larger libc struct sigaction to this
 * 32-byte shape before the syscall, so this — not the 148/152-byte libc
 * struct — is what an lx process hands us.  Order matters: handler, flags,
 * restorer, mask. */
struct lx_kernel_sigaction {
    uint64_t k_sa_handler;      /* SIG_DFL(0)/SIG_IGN(1)/fn pointer */
    uint64_t sa_flags;          /* SA_* — the shared subset passes through */
    uint64_t sa_restorer;       /* __restore_rt trampoline (SA_RESTORER) */
    uint64_t sa_mask;           /* kernel sigset_t: one 64-bit word */
};

/* ---- rt_sigreturn(15): the frame the restorer expects -------------------- */

/* Linux x86-64 struct sigcontext_64 — 256 bytes, the register payload inside
 * the ucontext.  Note there is no r11 slot (Linux does not save it across a
 * signal handler; it is caller-saved), and cs/gs/fs are 16-bit. */
struct lx_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, gs, fs, __pad0;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;           /* struct _fpstate * — unused here (see below) */
    uint64_t __reserved1[8];
};

/* stack_t, 24 bytes. */
struct lx_stack {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  ss_pad;
    uint64_t ss_size;
};

/* Linux struct ucontext as built inside the rt_sigframe: 936 bytes.
 * uc_sigmask is the kernel sigset_t (8 bytes); the __unused[30] ints pad it
 * out to glibc's 32-int sigset slot; __fpregs_mem is where the kernel would
 * put the _fpstate (512 bytes).  AuraLite does not save/restore FPU state
 * across an lx signal (documented deviation: the L3 ladder apps are terminal
 * programs that never touch the FPU; the native path keeps its fxsave). */
struct lx_ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct lx_stack uc_stack;
    struct lx_sigcontext uc_mcontext;
    uint64_t uc_sigmask;
    int32_t  __unused[30];
    uint64_t __fpregs_mem[64];
};

/* kernel siginfo_t: 128 bytes.  Only the common prefix and the payload the
 * ladder apps actually read (si_pid/si_uid for SI_USER, si_addr for a fault)
 * are filled; the rest stays zero. */
struct lx_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t __pad0;
    uint64_t _sifields[14];     /* 112 bytes of payload (si_pid/si_addr live here) */
};

/* The rt_sigframe: pretcode first (it doubles as the handler's return
 * address — the handler's `ret` pops it and lands in the restorer), then the
 * ucontext, then the siginfo.  musl's __restore_rt does `mov $15,%eax;
 * syscall` without touching RSP, so rt_sigreturn finds the frame at RSP-8 and
 * the ucontext exactly at RSP. */
struct lx_rt_sigframe {
    uint64_t pretcode;
    struct lx_ucontext uc;
    struct lx_siginfo info;
};

#define LX_SIGFRAME_PRETCODE_OFF  0
#define LX_SIGFRAME_UC_OFF        8
#define LX_SIGFRAME_INFO_OFF      (LX_SIGFRAME_UC_OFF + 936)
#define LX_SIGFRAME_SIZE          (LX_SIGFRAME_INFO_OFF + 128)   /* 1072 */

#endif /* AURALITE_KERNEL_LX_SIG_H */
