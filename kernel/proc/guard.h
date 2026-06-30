#ifndef AURALITE_PROC_GUARD_H
#define AURALITE_PROC_GUARD_H

#include <stdint.h>

/*
 * Stack guard-page classification.
 *
 * Both kernel and user stacks are allocated with unmapped guard page(s) so a
 * stack overflow (or a wild access just past the stack) takes a page fault on
 * the guard instead of silently corrupting an adjacent allocation.  This module
 * turns such a fault into an explicit, human-readable diagnosis so the #PF
 * handler can report "stack overflow" rather than a generic page fault.
 *
 *   - Kernel stacks: every thread slot is laid out as
 *       [low guard page][THREAD_STACK_SIZE usable][high guard page].
 *     Overflow grows down into the low guard; the high guard catches wild
 *     accesses just above the stack.  Detection is exact, using the current
 *     thread's mapped slot.
 *
 *   - User stacks: the stack lives at a fixed high virtual window with one
 *     unmapped guard page just below the lowest mapped byte.  Detection is by
 *     address window (the stack VA range is fixed by the loader).
 */

enum guard_fault_kind {
    GUARD_FAULT_NONE = 0,           /* not a guard-page fault */
    GUARD_FAULT_KERNEL_STACK_LOW,   /* overflow off the bottom of a kstack */
    GUARD_FAULT_KERNEL_STACK_HIGH,  /* wild access above a kstack */
    GUARD_FAULT_USER_STACK,         /* user stack overflow / underflow */
};

/*
 * Classify a page-fault address.  @from_user is non-zero when the fault came
 * from Ring 3.  Returns the guard kind; if @desc_out is non-NULL it is set to a
 * static human-readable description (valid forever).
 */
enum guard_fault_kind guard_classify_fault(uint64_t cr2, int from_user,
                                           const char **desc_out);

#endif /* AURALITE_PROC_GUARD_H */
