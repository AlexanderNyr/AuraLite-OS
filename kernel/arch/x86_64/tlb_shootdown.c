/* kernel/arch/x86_64/tlb_shootdown.c — precise TLB shootdown
 * (OPT_PLAN.md O5; the pre-O5 version was Fact 4: a full CR3 reload on
 * every CPU for every shootdown, no payload, no filter).
 *
 * Shape: per-target mailboxes + fire-and-forget IPIs.
 *
 *   - Each CPU owns a mailbox {seq, cr3, va, npages}.  A sender takes
 *     tlb_send_lock, writes the payload, bumps seq (release), sends a
 *     fixed IPI to that CPU — and moves on.
 *
 *   - WHY fire-and-forget and not sender-waits-for-ack: paging_unmap()
 *     sends while holding vm_lock (irqsave).  With an ack protocol,
 *     sender A waiting under vm_lock for CPU B's ack while B spins on
 *     vm_lock with interrupts off is a deadlock, not a hazard — B can
 *     never take the IPI it is being waited on for.  The ack was
 *     traded for a rule: anything a handler cannot reconstruct
 *     degrades to a FULL flush (never the other way), so losing a
 *     payload can cost performance, never correctness.
 *
 *   - Fixed-vector IPIs collapse in the LAPIC (one pending bit per
 *     vector), so a handler may observe seq advance by more than one:
 *     an overwritten payload.  tlb_policy_decide() answers with FULL.
 *     A torn payload read (seq moved underneath the handler) also
 *     degrades to FULL.  The policy core is pure C in tlb_policy.h and
 *     host-tested.
 *
 *   - Sender-side skip (the O5 win): without PCID, loading CR3 flushes
 *     the whole TLB — so a CPU whose current CR3 is not the affected
 *     address space CANNOT hold stale entries for it, architecturally.
 *     paging_switch_to() publishes each CPU's CR3 into cpu_cr3_shadow[];
 *     the sender skips CPUs that fail the filter and counts them into
 *     tlb_ipis_skipped.  The race is benign by the same fact: a CPU
 *     switching TO the affected space mid-send performs a CR3 load,
 *     which flushes, and the sender already updated the PTEs before
 *     calling here.
 *
 * PCID residue (recorded, deferred): with PCID the "CR3 switch means
 * flush" fact dies and both the skip filter and the mailbox need a
 * generation scheme; the mailbox layout keeps room for it.  Real
 * hardware territory per D1 — QEMU TCG timing cannot judge it.
 */

#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/tlb_policy.h"
#include "kernel/arch/x86_64/tlb_shootdown.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/perfstat.h"

#define TLB_MAX_CPUS 32

struct tlb_mailbox {
    volatile uint64_t seq;
    volatile uint64_t cr3;
    volatile uint64_t va;
    volatile uint32_t npages;
};

static struct tlb_mailbox mailboxes[TLB_MAX_CPUS];
static uint64_t last_handled[TLB_MAX_CPUS];      /* owner-CPU private   */
static volatile uint64_t cpu_cr3_shadow[TLB_MAX_CPUS];
static spinlock_t tlb_send_lock = SPINLOCK_UNLOCKED;

uint64_t tlb_current_asid(void) {
    return read_cr3();
}

void tlb_note_cr3(uint32_t cpu_id, uint64_t cr3) {
    if (cpu_id < TLB_MAX_CPUS) {
        cpu_cr3_shadow[cpu_id] = cr3;
    }
}

void tlb_shootdown_range(uint64_t cr3_filter, uint64_t va, uint32_t npages) {
    uint32_t ncpus = smp_get_cpu_count();
    if (ncpus <= 1) {
        return;
    }
    if (ncpus > TLB_MAX_CPUS) {
        ncpus = TLB_MAX_CPUS;
    }

    uint64_t fl = spinlock_acquire_irqsave(&tlb_send_lock);
    uint32_t me = 0;
    if (cpu_local_ready) {
        struct cpu_local *cl = get_cpu_local();
        if (cl) me = (uint32_t)cl->cpu_id;
    }

    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (cpu == me) {
            continue;                   /* caller invlpg'd locally */
        }
        if (!tlb_policy_target_wanted(cr3_filter, cpu_cr3_shadow[cpu])) {
            perfstat_add(PERF_TLB_IPIS_SKIPPED, 1);
            continue;
        }
        struct tlb_mailbox *mb = &mailboxes[cpu];
        mb->cr3    = cr3_filter;
        mb->va     = va;
        mb->npages = npages;
        __atomic_store_n(&mb->seq, mb->seq + 1, __ATOMIC_RELEASE);
        lapic_send_ipi_fixed(smp_get_lapic_id(cpu), IPI_TLB_SHOOTDOWN_VECTOR);
    }
    spinlock_release_irqrestore(&tlb_send_lock, fl);
}

void ipi_tlb_shootdown_handler(void) {
    uint32_t me = 0;
    if (cpu_local_ready) {
        struct cpu_local *cl = get_cpu_local();
        if (cl) me = (uint32_t)cl->cpu_id;
    }
    struct tlb_mailbox *mb = &mailboxes[me % TLB_MAX_CPUS];

    uint64_t seq = __atomic_load_n(&mb->seq, __ATOMIC_ACQUIRE);
    enum tlb_action act = tlb_policy_decide(seq, last_handled[me % TLB_MAX_CPUS],
                                            mb->npages, TLB_INVLPG_MAX);

    if (act != TLB_ACT_SPURIOUS) {
        uint64_t va = mb->va;
        uint32_t n  = mb->npages;

        /* Torn-payload guard: if the sender moved seq while we were
         * reading, the va/n pair may be a mix of two requests. */
        uint64_t seq2 = __atomic_load_n(&mb->seq, __ATOMIC_ACQUIRE);
        if (seq2 != seq) {
            act = TLB_ACT_FULL;
            seq = seq2;
        }

        if (act == TLB_ACT_RANGED) {
            for (uint32_t i = 0; i < n; i++) {
                invlpg(va + (uint64_t)i * 4096ULL);
            }
            perfstat_add(PERF_TLB_SHOOTDOWNS_RANGED, 1);
        } else {
            /* Full flush: reload CR3. */
            uint64_t cr3;
            __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3"
                              : "=r"(cr3) :: "memory");
            perfstat_add(PERF_TLB_SHOOTDOWNS_FULL, 1);
        }
        last_handled[me % TLB_MAX_CPUS] = seq;
    }
    lapic_eoi();
}
