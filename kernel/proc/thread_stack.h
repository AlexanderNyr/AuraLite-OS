/* thread_stack.h — the kernel-thread stack geometry, single-sourced.
 *
 * SELFHOST SH5c: these constants used to live in thread.c (with tss.c
 * hardcoding the downstream IST base as "128 slots * 24 KiB").  Growing
 * THREAD_STACK_SIZE to 32 KiB silently overran that hardcoded IST base --
 * the thread-stack pool is 5 MiB now, and the first thread's stack memset
 * wiped the #DF IST1 stacks (deterministic early-boot deadlock).  The
 * geometry now lives in one place; tss.c derives its region base from
 * THREAD_STACK_REGION_END, so the two can never drift again.
 */
#ifndef KERNEL_PROC_THREAD_STACK_H
#define KERNEL_PROC_THREAD_STACK_H

#include <stdint.h>

/* SELFHOST SH5c: 32 KiB.  Was 16 KiB, which the clang build fits with 2.5x
 * headroom (deepest frame chain measured ~6 KiB).  The tcc-built kernel
 * overflows it: tcc does not overlap stack slots of locals from disjoint
 * switch cases, so syscall_dispatch's many per-case buffers sum to a
 * 19 968-byte frame (clang's is 4 616) and the first Ring-3 syscall landed
 * in the guard page (#PF -> #DF, IST1 halt).  32 KiB fits the tcc chain
 * (19.5 KiB dispatch + nested handler frames) with the same margin the
 * clang build had; costs +16 KiB physical per thread. */
#define THREAD_STACK_SIZE        (32 * 1024)   /* 32 KiB usable per kernel thread */
#define THREAD_STACK_GUARD_PAGES 1
#define THREAD_STACK_PAGES       (THREAD_STACK_SIZE / 4096)
#define THREAD_STACK_SLOT_SIZE   ((THREAD_STACK_PAGES + 2 * THREAD_STACK_GUARD_PAGES) * 4096ULL)

/* The guarded kernel-stack pool: thread stacks are mapped into fixed
 * per-slot windows in this region (thread.c).  Placed 64 MiB after
 * KHEAP_BASE (kernel/mm/kheap.h) -- i.e. exactly at the kernel heap's
 * current ceiling (KHEAP_LIMIT) -- so kheap growth can never run into the
 * thread kernel-stack region.  Keep those two constants in sync if either
 * region's size changes. */
#define THREAD_STACK_REGION_BASE 0xFFFFFFFF8C000000ULL
#define THREAD_STACK_MAX_SLOTS   128
#define THREAD_STACK_REGION_END  (THREAD_STACK_REGION_BASE + (uintptr_t)THREAD_STACK_MAX_SLOTS * THREAD_STACK_SLOT_SIZE)

#endif /* KERNEL_PROC_THREAD_STACK_H */
