/* guard.c — stack guard-page fault classification. */

#include "kernel/proc/guard.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"

/*
 * User-stack virtual window.  The loader (kernel/proc/process.c) places the
 * user stack just below USER_STACK_TOP with a single unmapped guard page below
 * the lowest mapped byte, applying a few pages of per-exec entropy.  Keep these
 * in sync with process.c.
 */
#define USER_STACK_TOP         0x7FFFF0000000ULL
#define USER_STACK_SIZE        0x10000ULL
#define USER_STACK_GUARD_SIZE  0x1000ULL
#define USER_STACK_ENTROPY     (0x10ULL * 0x1000ULL) /* choose_user_stack_top() slack */

#define PAGE_SIZE 0x1000ULL

enum guard_fault_kind guard_classify_fault(uint64_t cr2, int from_user,
                                           const char **desc_out) {
    const char *desc = 0;
    enum guard_fault_kind kind = GUARD_FAULT_NONE;

    if (from_user) {
        /*
         * A fault in the user-stack window but below the lowest mapped stack
         * byte is a stack overflow.  The window spans the entropy slack plus
         * one guard page below the smallest possible stack base.
         */
        uint64_t stack_low_min =
            USER_STACK_TOP - USER_STACK_ENTROPY - USER_STACK_SIZE;
        uint64_t guard_low = stack_low_min - USER_STACK_GUARD_SIZE;
        if (cr2 >= guard_low && cr2 < USER_STACK_TOP) {
            kind = GUARD_FAULT_USER_STACK;
            desc = "user stack overflow (guard page)";
        }
    } else {
        /*
         * Kernel-stack guard pages bracket the current thread's mapped slot:
         *   [region .. kernel_stack)            -> low guard  (overflow)
         *   [kernel_stack+SIZE .. +guard)       -> high guard (wild access)
         */
        tcb_t *cur = sched_current();
        if (cur && cur->kernel_stack && cur->kernel_stack_region) {
            uint64_t usable = (uint64_t)(uintptr_t)cur->kernel_stack;
            uint64_t region = (uint64_t)(uintptr_t)cur->kernel_stack_region;
            uint64_t guard_bytes = (uint64_t)THREAD_STACK_GUARD_PAGES * PAGE_SIZE;

            uint64_t low_guard_lo = region;            /* == usable - guard_bytes */
            uint64_t low_guard_hi = usable;            /* exclusive */
            uint64_t high_guard_lo = usable + THREAD_STACK_SIZE;
            uint64_t high_guard_hi = high_guard_lo + guard_bytes;

            if (cr2 >= low_guard_lo && cr2 < low_guard_hi) {
                kind = GUARD_FAULT_KERNEL_STACK_LOW;
                desc = "kernel stack overflow (low guard page)";
            } else if (cr2 >= high_guard_lo && cr2 < high_guard_hi) {
                kind = GUARD_FAULT_KERNEL_STACK_HIGH;
                desc = "kernel stack wild access (high guard page)";
            }
        }
    }

    if (desc_out) *desc_out = desc;
    return kind;
}
