/* pcid_policy.h — the PCID allocation/switch decision core, pure C
 * (RESIDUE_PLAN R11; HW_PLAN H4's written design D-PCID-1..4, landed).
 *
 * No I/O, no CPU state — the same split as tlb_policy.h, and for the
 * same reason: the sandbox's QEMU TCG does not implement PCID AT ALL
 * (measured this phase: `-cpu qemu64,+pcid` prints "TCG doesn't
 * support requested feature: CPUID.01H:ECX.pcid" and boots pcid=0),
 * so the only executable rig for the DECISIONS is a host unit test
 * (tests/unit/test_pcid_policy.c).  The thin CR3-bit plumbing around
 * them stays gated on runtime detection and first executes on the
 * user's WHPX machine — the D-PCID-5 lane whose `pcid=1` log opened
 * this phase.
 *
 * The representation (one instance per CPU — PCIDs are a per-TLB
 * namespace, exactly D-PCID-1's argument):
 *
 *   slot ∈ [0, PCID_SLOTS): slot 0 is the kernel address space's
 *   (PCID 0 stays the kernel's, D-PCID-1), user address spaces hash
 *   into 1..PCID_SLOTS-1 by PML4 physical address.  Each slot
 *   remembers which PML4 owns it and under which GENERATION it was
 *   claimed.
 *
 * DEVIATION FROM THE WRITTEN DESIGN, NAMED: D-PCID-1 prescribed a
 * bump allocator over the full 1..4095 space.  A bump allocator needs
 * a reverse pml4→pcid lookup on every switch; the honest options are
 * a 4095-entry owner table per CPU (48 KiB × 32 CPUs of static bss)
 * or a search.  The landed allocator is a DETERMINISTIC HASH SLOT in
 * 1..255 with an owner check — 8 effective PCID bits, 3 KiB per CPU.
 * A slot collision costs a PCID-scoped flush (the FRESH path), never
 * correctness, by the eviction fact below.
 *
 * The three facts the correctness argument leans on (all used by the
 * sender filter, pcid_policy_sender_skip):
 *
 *   1. EVICTION FLUSHES.  Claiming a slot loads CR3 with the new
 *      owner's PML4, the slot's PCID, and NOFLUSH=0 — and a MOV CR3
 *      with bit 63 clear invalidates ALL entries tagged with that
 *      PCID (SDM vol 3, 4.10.4.1).  So the moment a PML4 stops being
 *      a slot's owner, its stale entries under that PCID are gone.
 *
 *   2. GENERATION BUMP REVOKES NOFLUSH.  Bumping the CPU's generation
 *      makes every slot's recorded generation stale, so every next
 *      entry takes the FRESH path — a full per-PCID flush BEFORE any
 *      stale entry could be used.  This is how a "full flush" is
 *      delivered to non-resident address spaces without invpcid (the
 *      user's WHPX machine reports invpcid=0): flush the CURRENT PCID
 *      with a CR3 reload, revoke everyone else's NOFLUSH lazily.
 *
 *   3. NON-RESIDENT MEANS UNUSED.  A non-resident address space's TLB
 *      entries cannot be consulted until its PCID is current again
 *      (no global pages in this tree: CR4.PGE is never set), and
 *      becoming current passes through this policy — which flushes
 *      (fact 1 or 2) unless the owner AND generation both match.
 */
#ifndef AURALITE_ARCH_X86_64_PCID_POLICY_H
#define AURALITE_ARCH_X86_64_PCID_POLICY_H

#include <stdint.h>

#define PCID_SLOTS 256u          /* slot 0 = kernel; 1..255 = user hash */

struct pcid_cpu_state {
    uint64_t owner[PCID_SLOTS];  /* PML4 phys that owns the slot (0 = none) */
    uint32_t gen_of[PCID_SLOTS]; /* generation the slot was claimed under   */
    uint32_t cur_gen;            /* bumped to revoke all NOFLUSH rights     */
};

enum pcid_entry_kind {
    PCID_ENTRY_FRESH   = 0,      /* load CR3 with NOFLUSH=0: flush this PCID */
    PCID_ENTRY_NOFLUSH = 1,      /* load CR3 with bit 63: the D-PCID-3 win   */
};

/* Deterministic slot for a PML4 (kernel PML4 is slot 0 by the caller's
 * explicit choice, not by hash). */
static inline uint32_t pcid_policy_slot(uint64_t pml4_phys) {
    return 1u + (uint32_t)((pml4_phys >> 12) % (PCID_SLOTS - 1u));
}

/* The switch decision (D-PCID-3): NOFLUSH iff this PML4 still owns its
 * slot AND the claim is from the current generation.  Otherwise claim
 * the slot fresh.  Mutates the state (the claim); returns the entry
 * kind.  `slot` comes from pcid_policy_slot(), or 0 for the kernel
 * address space. */
static inline enum pcid_entry_kind
pcid_policy_enter(struct pcid_cpu_state *st, uint32_t slot,
                  uint64_t pml4_phys) {
    if (st->owner[slot] == pml4_phys && st->gen_of[slot] == st->cur_gen) {
        return PCID_ENTRY_NOFLUSH;
    }
    st->owner[slot]  = pml4_phys;
    st->gen_of[slot] = st->cur_gen;
    return PCID_ENTRY_FRESH;
}

/* Revoke every NOFLUSH right on this CPU (fact 2).  The caller pairs
 * this with a CR3 reload of the CURRENT PCID; every other PCID's
 * entries die on their owner's next (now necessarily FRESH) entry.
 * Counts as a "generation wrap" in /proc/perf.  The uint32 generation
 * itself wraps after 2^32 bumps — an ABA that would need a slot to
 * sleep through exactly 2^32 revocations while its owner never
 * switches in; accepted and recorded here. */
static inline void pcid_policy_gen_bump(struct pcid_cpu_state *st) {
    st->cur_gen++;
}

/* De-own one address space's slot (the handler-side narrow action for
 * a non-resident victim: cheaper than any flush, forces the victim's
 * next entry FRESH).  No-op if the victim no longer owns the slot. */
static inline void pcid_policy_deown(struct pcid_cpu_state *st,
                                     uint32_t slot, uint64_t pml4_phys) {
    if (st->owner[slot] == pml4_phys) {
        st->owner[slot] = 0;
    }
}

/* Sender-side filter, the D-PCID-4 generalisation of O5's skip: may
 * the sender skip IPI-ing a target CPU about `victim_pml4`?
 *
 *   resident (target's current CR3 addr == victim)  -> must IPI (0)
 *   not the slot's owner                            -> skip (1): fact 1,
 *      eviction already flushed those entries
 *   owner but stale generation                      -> skip (1): fact 2,
 *      the next entry is FRESH and flushes before use
 *   owner, current generation, not resident         -> must IPI (0):
 *      a live NOFLUSH right — the entries would survive re-entry
 *
 * Reading another CPU's state races with that CPU switching; every
 * race resolves conservatively: a concurrent claim/deown makes us see
 * either the old owner (we IPI — harmless) or the new one (we skip —
 * sound, because the transition itself flushed, fact 1). */
static inline int pcid_policy_sender_skip(const struct pcid_cpu_state *st,
                                          uint32_t slot,
                                          uint64_t victim_pml4,
                                          uint64_t target_cr3_addr) {
    if (target_cr3_addr == victim_pml4) return 0;
    if (st->owner[slot] != victim_pml4) return 1;
    if (st->gen_of[slot] != st->cur_gen) return 1;
    return 0;
}

#endif /* AURALITE_ARCH_X86_64_PCID_POLICY_H */
