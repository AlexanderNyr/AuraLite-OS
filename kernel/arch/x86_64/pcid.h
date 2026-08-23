/* pcid.h — PCID plumbing around pcid_policy.h (RESIDUE_PLAN R11,
 * HW_PLAN H4 D-PCID-1..4 landed; D-PCID-5's executable lane is the
 * user's WHPX machine — pcid=1 invpcid=0 in their boot log).
 *
 * Everything here is inert unless CR4.PCIDE was actually enabled at
 * boot (CPUID.01H:ECX.17): on every TCG lane pcid_active() is 0 and
 * paging/tlb code takes its historical paths byte-identically.
 */
#ifndef AURALITE_ARCH_X86_64_PCID_H
#define AURALITE_ARCH_X86_64_PCID_H

#include <stdint.h>

/* CR3 layout bits used when PCIDE is on. */
#define PCID_CR3_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PCID_CR3_NOFLUSH   (1ULL << 63)

/* 1 iff CR4.PCIDE was enabled on this boot (set once by
 * paging_cpu_features_init on the BSP; APs mirror the BSP decision). */
int pcid_active(void);

/* Called by paging_cpu_features_init with this CPU's CPUID pcid bit;
 * decides (BSP) or mirrors (AP) the PCIDE enable and returns the CR4
 * bit to OR in (0 when staying off). */
uint64_t pcid_cr4_bit(int cpuid_has_pcid, int is_bsp);

/* Record the kernel PML4 (slot 0 / PCID 0 owner, per D-PCID-1). */
void pcid_set_kernel_pml4(uint64_t pml4_phys);

/* Full CR3 value for switching to pml4_phys on the calling CPU:
 * pml4 | pcid | (NOFLUSH when the policy grants re-entry).  Counts
 * PERF_CR3_NOFLUSH_SWITCHES on the granted path.  Falls back to the
 * bare pml4 (PCID 0, full-flush semantics) when inactive or before
 * cpu_local is up.  Must run with interrupts disabled OR tolerate the
 * cost of its own irq_save (it does the latter: the shootdown handler
 * mutates the same per-CPU table from IRQ context). */
uint64_t pcid_cr3_for(uint64_t pml4_phys);

/* Revoke every NOFLUSH right on the calling CPU (generation bump,
 * D-PCID-2's lazy full flush) and count PERF_PCID_GENERATION_WRAPS.
 * The caller pairs it with a CR3 reload when the CURRENT pcid's
 * entries must die too. */
void pcid_local_gen_bump(void);

/* Narrow handler-side action for a non-resident victim address space:
 * de-own its slot so its next entry is FRESH.  IRQ-context safe (only
 * touches the calling CPU's table). */
void pcid_local_deown(uint64_t victim_pml4);

/* Sender-side D-PCID-4 filter: may the sender skip IPI-ing `cpu`
 * about victim_pml4?  target_cr3_addr is the (already normalised)
 * cpu_cr3_shadow entry.  Races documented in pcid_policy.h. */
int pcid_sender_may_skip(uint32_t cpu, uint64_t victim_pml4,
                         uint64_t target_cr3_addr);

#endif /* AURALITE_ARCH_X86_64_PCID_H */
