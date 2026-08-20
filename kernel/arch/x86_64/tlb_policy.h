/* tlb_policy.h — the shootdown handler's decision core, pure C
 * (OPT_PLAN.md O5).
 *
 * No I/O, no CPU state: just the arithmetic that decides what a target
 * CPU must do with its mailbox — split out so the host unit test
 * (tests/unit/test_tlb_policy.c) can exercise the sequence-gap and
 * boundary cases without an SMP machine in the room.
 *
 * The protocol this encodes (see tlb_shootdown.c for the whole story):
 * senders never wait.  Each target CPU owns a mailbox; the sender
 * overwrites it and bumps `seq`.  Fixed-vector IPIs COLLAPSE when they
 * pile up (the LAPIC holds one pending bit per vector), so a handler
 * can observe seq jumping by more than one — in which case an earlier
 * request's payload has been overwritten and the only safe action is a
 * full flush.  A torn read (seq moved while the payload was being read)
 * degrades the same way.  Ranged invalidation is a fast path, full
 * flush is the always-correct fallback — never the other way round.
 */
#ifndef AURALITE_ARCH_X86_64_TLB_POLICY_H
#define AURALITE_ARCH_X86_64_TLB_POLICY_H

#include <stdint.h>

/* Above this many pages a loop of invlpg costs more than the reload. */
#define TLB_INVLPG_MAX 32u

enum tlb_action {
    TLB_ACT_SPURIOUS = 0,   /* seq already handled: collapsed IPI, no-op */
    TLB_ACT_RANGED   = 1,   /* invlpg the [va, va + npages) window       */
    TLB_ACT_FULL     = 2,   /* reload CR3                                */
};

/* Decide from the handler's view: the seq it loaded, the last seq it
 * completed, and the payload's page count.  npages == 0 is the sender's
 * explicit "flush everything for this address space" request. */
static inline enum tlb_action tlb_policy_decide(uint64_t seq,
                                                uint64_t last_handled,
                                                uint32_t npages,
                                                uint32_t invlpg_max) {
    if (seq == last_handled) {
        return TLB_ACT_SPURIOUS;
    }
    if (seq - last_handled > 1) {
        /* A request was overwritten before we saw it: its payload is
         * gone, so nothing narrower than a full flush is sound. */
        return TLB_ACT_FULL;
    }
    if (npages == 0 || npages > invlpg_max) {
        return TLB_ACT_FULL;
    }
    return TLB_ACT_RANGED;
}

/* Sender-side filter: does this target CPU need the IPI at all?
 * cpu_cr3 is the shadow of the CR3 the target loaded last (0 = unknown,
 * e.g. before its first address-space switch — treat as "might hold
 * anything").  Without PCID a CR3 load flushes the whole TLB, so a CPU
 * whose current CR3 differs from the target address space CANNOT hold
 * stale entries for it: skipping the IPI is not an optimisation gamble,
 * it is an architectural fact.  cr3_filter == 0 means the range is
 * kernel-shared and every CPU is a target. */
static inline int tlb_policy_target_wanted(uint64_t cr3_filter,
                                           uint64_t cpu_cr3) {
    if (cr3_filter == 0) return 1;      /* kernel range: broadcast   */
    if (cpu_cr3 == 0)    return 1;      /* unknown: assume the worst */
    return cpu_cr3 == cr3_filter;
}

#endif /* AURALITE_ARCH_X86_64_TLB_POLICY_H */
