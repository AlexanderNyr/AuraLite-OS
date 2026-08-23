/* pcid.c — PCID plumbing (RESIDUE_PLAN R11; HW_PLAN H4's design,
 * implemented).  The decisions live in pcid_policy.h (pure C,
 * host-tested by tests/unit/test_pcid_policy.c); this file owns the
 * per-CPU tables, the counters and the CR4/CR3 bit mechanics.
 *
 * Runtime truth, measured:
 *   - sandbox/CI TCG: `-cpu qemu64,+pcid` → "TCG doesn't support
 *     requested feature: CPUID.01H:ECX.pcid" — every CI lane boots
 *     pcid=0 and this file stays inert (pcid_active() == 0, no CR4
 *     write, no CR3 bit, counters pinned at 0 BY the feature gate).
 *   - the user's WHPX machine: pcid=1 invpcid=0 — the D-PCID-5 lane.
 *     No invpcid anywhere here: full flushes are CR3 reloads plus the
 *     generation bump (the "CR3-toggle fallback" of the R11 row).
 */

#include "kernel/arch/x86_64/pcid.h"
#include "kernel/arch/x86_64/pcid_policy.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/lib/perfstat.h"

/* Mirrors tlb_shootdown.c's TLB_MAX_CPUS (the O5 mailbox bound). */
#define PCID_MAX_CPUS 32

static struct pcid_cpu_state pcid_state[PCID_MAX_CPUS];
static int      pcid_on;           /* BSP decision, mirrored by APs   */
static uint64_t pcid_kernel_pml4;  /* slot 0's owner                  */

int pcid_active(void) {
    return pcid_on;
}

uint64_t pcid_cr4_bit(int cpuid_has_pcid, int is_bsp) {
    /* CR4.PCIDE (bit 17).  Legal only in long mode with CR3[11:0]==0 —
     * both hold at the call site (paging_cpu_features_init runs on the
     * kernel PML4, which is page-aligned).  The BSP decides; APs follow
     * the recorded decision so a hypothetically asymmetric core cannot
     * split the boot into mixed PCIDE states. */
    if (is_bsp) {
        pcid_on = cpuid_has_pcid ? 1 : 0;
    }
    return pcid_on ? (1ULL << 17) : 0;
}

void pcid_set_kernel_pml4(uint64_t pml4_phys) {
    pcid_kernel_pml4 = pml4_phys & PCID_CR3_ADDR_MASK;
}

static inline uint32_t this_cpu(void) {
    if (cpu_local_ready) {
        struct cpu_local *cl = get_cpu_local();
        if (cl) return (uint32_t)cl->cpu_id % PCID_MAX_CPUS;
    }
    return 0;
}

uint64_t pcid_cr3_for(uint64_t pml4_phys) {
    if (!pcid_on || !cpu_local_ready) {
        return pml4_phys;
    }
    uint64_t addr = pml4_phys & PCID_CR3_ADDR_MASK;
    uint32_t slot = (addr == pcid_kernel_pml4)
                        ? 0u : pcid_policy_slot(addr);

    /* The shootdown IPI handler de-owns slots in this same table; a
     * decision read torn by that handler could grant NOFLUSH on a
     * just-revoked right.  irq_save is the whole synchronisation:
     * the table is strictly per-CPU. */
    uint64_t fl = irq_save();
    enum pcid_entry_kind kind =
        pcid_policy_enter(&pcid_state[this_cpu()], slot, addr);
    irq_restore(fl);

    uint64_t cr3 = addr | slot;
    if (kind == PCID_ENTRY_NOFLUSH) {
        cr3 |= PCID_CR3_NOFLUSH;
        perfstat_add(PERF_CR3_NOFLUSH_SWITCHES, 1);
    }
    return cr3;
}

void pcid_local_gen_bump(void) {
    if (!pcid_on) {
        return;
    }
    uint64_t fl = irq_save();
    pcid_policy_gen_bump(&pcid_state[this_cpu()]);
    irq_restore(fl);
    perfstat_add(PERF_PCID_GENERATION_WRAPS, 1);
}

void pcid_local_deown(uint64_t victim_pml4) {
    if (!pcid_on) {
        return;
    }
    uint64_t addr = victim_pml4 & PCID_CR3_ADDR_MASK;
    uint32_t slot = (addr == pcid_kernel_pml4)
                        ? 0u : pcid_policy_slot(addr);
    /* Callers are the IPI handler (interrupts already off) and locals
     * under irq_save; the extra save here keeps the contract local. */
    uint64_t fl = irq_save();
    pcid_policy_deown(&pcid_state[this_cpu()], slot, addr);
    irq_restore(fl);
}

int pcid_sender_may_skip(uint32_t cpu, uint64_t victim_pml4,
                         uint64_t target_cr3_addr) {
    if (!pcid_on) {
        return 0;
    }
    uint64_t addr = victim_pml4 & PCID_CR3_ADDR_MASK;
    uint32_t slot = (addr == pcid_kernel_pml4)
                        ? 0u : pcid_policy_slot(addr);
    return pcid_policy_sender_skip(&pcid_state[cpu % PCID_MAX_CPUS],
                                   slot, addr, target_cr3_addr);
}
