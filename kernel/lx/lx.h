#ifndef AURALITE_KERNEL_LX_H
#define AURALITE_KERNEL_LX_H

/*
 * kernel/lx/lx.h — the Linux ("lx") personality (LX_COMPAT_PLAN.md L1).
 *
 * AuraLite's syscall MECHANISM is already Linux's: SYSCALL/SYSRET with
 * the number in RAX and arguments in RDI/RSI/RDX/R10/R8/R9, Linux's
 * errno values returned negated.  What differs is the NUMBER MAP — the
 * native map grew POSIX-ish groups at numbers Linux assigns to other
 * calls (native LISTDIR=80 vs Linux getcwd=80, native DNS=82 vs Linux
 * rename=82, ...), and a handful of structures.
 *
 * A process flagged PERSONA_LX in its TCB gets its syscall numbers
 * passed through lx_translate() at the top of the dispatcher.  Native
 * processes are untouched: the persona flag is the only door, and the
 * native arms behind colliding numbers keep their native semantics.
 *
 * This header (and lx_translate.c) is deliberately freestanding-pure —
 * no kernel includes — so the same source compiles into the host unit
 * test tests/unit/test_lx_translate.c, the w32_pe.c precedent.
 */

#include <stdint.h>

/* lx_translate() result space:
 *   0 .. 0xFFFF      native syscall number (identity entries are pinned
 *                    explicitly — an identity row is a fact about BOTH
 *                    tables, not a default; anything absent is unmapped)
 *   0x10000 ..       lx-only arm: a Linux call with no native equivalent,
 *                    handled by a dedicated case in syscall_dispatch()
 *   0xFFFFFFFF       LX_UNMAPPED: Linux nr this build does not speak;
 *                    the dispatcher's default answers -ENOSYS, loudly.
 * The 0x10000 base cannot collide with either map (both are < 512). */
#define LX_UNMAPPED          0xFFFFFFFFu
#define LX_ARM_BASE          0x10000u

/* Linux x86-64 numbers (none of these has a native arm at this nr).
 * The numbers are checked against asm/unistd_64.h, not recalled:
 * set_tid_address is 218 on x86-64 (96 is gettimeofday — the first
 * draft had them swapped and the L1 gate caught it). */
#define LX_ARM_EXIT_GROUP      (LX_ARM_BASE + 231u)  /* 231 */
#define LX_ARM_SET_TID_ADDRESS (LX_ARM_BASE + 218u)  /* 218 */
#define LX_ARM_UNAME           (LX_ARM_BASE + 63u)   /*  63 */
#define LX_ARM_GETTID          (LX_ARM_BASE + 186u)  /* 186 */
#define LX_ARM_BRK             (LX_ARM_BASE + 12u)   /*  12 */
#define LX_ARM_SET_ROBUST_LIST (LX_ARM_BASE + 273u)  /* 273 */
#define LX_ARM_PRLIMIT64       (LX_ARM_BASE + 302u)  /* 302 */
#define LX_ARM_READLINKAT      (LX_ARM_BASE + 267u)  /* 267 */

/* L2: the stat family and the directory read.  stat(4)/fstat(5)/
 * lstat(6) look like number matches with the native table — and that
 * is exactly why they must NOT be identity rows: the native arms at
 * 5/6/105 fill struct vfs_stat, a layout no Linux binary speaks.  The
 * number equality is a trap; the lx arms marshal into Linux's 144-byte
 * struct stat instead.  getdents64(217) has no native arm at all (the
 * define exists, the dispatcher arm never did — native opendir goes
 * through LISTDIR=80), and newfstatat(262) exists natively but again
 * answers in struct vfs_stat. */
#define LX_ARM_STAT             (LX_ARM_BASE + 4u)    /*   4 */
#define LX_ARM_FSTAT            (LX_ARM_BASE + 5u)    /*   5 */
#define LX_ARM_LSTAT            (LX_ARM_BASE + 6u)    /*   6 */
#define LX_ARM_GETDENTS64       (LX_ARM_BASE + 217u)  /* 217 */
#define LX_ARM_NEWFSTATAT       (LX_ARM_BASE + 262u)  /* 262 */

/* L3: process plumbing — the signal and clone marshals.  rt_sigaction(13)
 * and rt_sigreturn(15) look like number matches with the native table
 * (SYS_SIGACTION=13, SYS_SIGRETURN=15) — and that is exactly why they must
 * NOT be identity rows: the native arms speak the native struct sigaction
 * ({handler, u32 mask, flags, restorer}) and the native signal_frame, neither
 * of which a Linux binary understands.  The lx arms marshal Linux's
 * kernel_sigaction (32 bytes) and the Linux rt_sigframe (pretcode + ucontext
 * + siginfo, ~1072 bytes).  See kernel/lx/lx_sig.h for the layouts.
 *
 * rt_sigprocmask(14) is NOT an identity row either, despite L2 mapping it as
 * one: busybox ash (L3's flagship) passes a NON-NULL oldset in several calls,
 * and the native arm writes only its 32-bit sigset_t (4 bytes) where Linux
 * writes the 8-byte kernel sigset_t — ash would read garbage in the high
 * word (signals 33..64).  The lx arm reads/writes the full 8-byte word and
 * documents that signals above 32 are always clear (AuraLite has NSIG=32).
 *
 * clone(56): the native arm only implements the CLONE_THREAD|CLONE_VM
 * pthread path and answers -ENOSYS for fork-style clone — but musl's fork()
 * IS clone(SIGCHLD, 0).  The lx arm routes fork-style clone to do_fork() and
 * pthread-style clone to do_clone(). */
#define LX_ARM_SIGACTION         (LX_ARM_BASE + 13u)   /*  13 */
#define LX_ARM_SIGRETURN         (LX_ARM_BASE + 15u)   /*  15 */
#define LX_ARM_SIGPROCMASK       (LX_ARM_BASE + 14u)   /*  14 */
#define LX_ARM_CLONE             (LX_ARM_BASE + 56u)   /*  56 */

/* Translate a Linux x86-64 syscall number to the native number (or an
 * LX_ARM_* pseudo-number, or LX_UNMAPPED).  Pure table lookup. */
uint32_t lx_translate(uint32_t linux_nr);

/* The /linux/ path-prefix rule: an execve whose (kernel-resolved) path
 * lies under /linux/ selects PERSONA_LX, anything else resets to
 * PERSONA_NATIVE.  Pure prefix test; also host-tested. */
int lx_path_is_lx(const char *kernel_path);

#endif /* AURALITE_KERNEL_LX_H */
