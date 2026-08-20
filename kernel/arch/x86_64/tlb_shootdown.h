/* tlb_shootdown.h — precise TLB shootdown (OPT_PLAN.md O5). */
#ifndef AURALITE_ARCH_X86_64_TLB_SHOOTDOWN_H
#define AURALITE_ARCH_X86_64_TLB_SHOOTDOWN_H

#include <stdint.h>

/* Ask every OTHER cpu that could hold translations of the given address
 * space to invalidate [va, va + npages * 4 KiB).
 *
 *   cr3_filter — physical PML4 of the affected address space, or 0 for
 *                a kernel-shared range (broadcast to everyone).
 *   npages     — 0 means "flush everything for that address space"
 *                (the sender touched too many scattered pages to list).
 *
 * Fire-and-forget by design: see tlb_shootdown.c for why an ack
 * protocol would deadlock against vm_lock, and how the per-CPU seq
 * mailboxes stay correct without one. */
void tlb_shootdown_range(uint64_t cr3_filter, uint64_t va, uint32_t npages);

/* Record that a CPU has loaded a new CR3 (called from paging_switch_to
 * and the boot-time adopts) — the sender-side skip filter reads this. */
void tlb_note_cr3(uint32_t cpu_id, uint64_t cr3);

/* The current CPU's address-space id (CR3) — a hook rather than a raw
 * read_cr3() at the call sites, so host-side unit tests that compile
 * kernel code (test_mprotect.c) can stub it instead of executing a
 * privileged register read in user mode. */
uint64_t tlb_current_asid(void);

#endif /* AURALITE_ARCH_X86_64_TLB_SHOOTDOWN_H */
