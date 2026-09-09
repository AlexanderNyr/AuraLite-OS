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

/* Translate a Linux x86-64 syscall number to the native number (or an
 * LX_ARM_* pseudo-number, or LX_UNMAPPED).  Pure table lookup. */
uint32_t lx_translate(uint32_t linux_nr);

/* The /linux/ path-prefix rule: an execve whose (kernel-resolved) path
 * lies under /linux/ selects PERSONA_LX, anything else resets to
 * PERSONA_NATIVE.  Pure prefix test; also host-tested. */
int lx_path_is_lx(const char *kernel_path);

#endif /* AURALITE_KERNEL_LX_H */
