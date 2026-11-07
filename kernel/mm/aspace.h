/* aspace.h — refcounted user address spaces (RESIDUE3 C2).
 *
 * The kernel already keeps a per-thread `pml4_phys`; what it did NOT keep is
 * how many threads point at one PML4.  Two mechanisms in the tree create
 * sharing and both were unaccounted for:
 *
 *   - clone(CLONE_VM) (kernel/proc/clone.c) copies the parent's pml4_phys
 *     into the child, so one address space has N threads;
 *   - execve (kernel/proc/process.c) replaces the caller's address space and
 *     used to free the OLD one immediately, pulling it out from under any
 *     CLONE_VM sibling still running on it.
 *
 * The reap side had the mirror-image defect: thread_reap_zombies() skipped
 * `is_pthread` zombies entirely, so a thread-group address space was freed
 * exactly when the LAST thread's TCB happened to be the non-pthread one —
 * which is the common case, not a guarantee.
 *
 * This module is the missing counter: a small, lock-light table keyed by the
 * physical PML4 root.  It owns no page tables and calls no paging code, so the
 * host unit test (tests/unit/test_aspace.c) compiles this file UNMODIFIED and
 * drives the real transition logic.
 *
 * Semantics:
 *   aspace_get(pml4)  — record one more owner; returns the new count
 *                       (0 when pml4 is 0 / the kernel space — untracked,
 *                       and the caller must keep the old behaviour);
 *   aspace_put(pml4)  — drop one owner; returns 1 only when THIS call removed
 *                       the last one, i.e. the caller may now free the page
 *                       tables.  Returns 0 otherwise (still shared, or the
 *                       space was never tracked).
 *
 * A space is registered by its FIRST aspace_get() after the pml4 is created.
 * A put on an unregistered pml4 returns 1 (it frees, exactly as before) so
 * that an unmigrated call site cannot leak a whole address space.
 */
#ifndef AURALITE_MM_ASPACE_H
#define AURALITE_MM_ASPACE_H

#include <stdint.h>

/* Open addressing, 8-probe window: the live population is one entry per
 * process, and a probe miss only costs accounting precision (the fallback
 * below frees on the first put, which is the pre-C2 behaviour). */
#define ASPACE_SLOTS 512

uint64_t aspace_get(uint64_t pml4_phys);
int      aspace_put(uint64_t pml4_phys);

/* Diagnostics (receipts): current owner count of a space, how many spaces are
 * shared by more than one thread, how many puts fell off the table, and how
 * many free decisions this module has authorised. */
int      aspace_refs(uint64_t pml4_phys);
uint64_t aspace_shared_spaces(void);
uint64_t aspace_untracked_puts(void);
uint64_t aspace_last_puts(void);
/* Puts refused because the root was already released (a double release). */
uint64_t aspace_double_puts(void);
uint64_t aspace_live_spaces(void);

/* Test hook: wipe the table (host tests and the pre-boot init only). */
void     aspace_reset(void);

#endif /* AURALITE_MM_ASPACE_H */
