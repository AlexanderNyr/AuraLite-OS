/* tss.c — Task State Segment setup for Ring 3 transitions.
 *
 * The TSS is not used for hardware task switching in long mode. Its purpose
 * here is RSP0: the stack pointer the CPU loads on a Ring 3 -> Ring 0
 * transition (interrupt or exception). We also allocate an IST1 stack for the
 * double-fault handler so a kernel-stack overflow cannot escalate to a triple
 * fault.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/tss.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/assert.h"

#define TSS_STACK_SIZE TSS_IST1_STACK_SIZE   /* 16 KiB usable */
#define MAX_TSS_CPUS   32
#define PER_CPU_GDT_BYTES (sizeof(struct gdt_entry) * GDT_NUM_ENTRIES)

/* FIX_R1: each IST1 stack is PMM-backed and explicitly mapped, preceded by
 * an UNMAPPED guard page, so overflowing the IST stack itself is caught by
 * a page fault on the guard instead of silently corrupting a neighbour.
 * Layout per CPU slot: [guard page][16 KiB usable].  The next slot's guard
 * page immediately follows this slot's usable top, so a wild access just
 * above the stack faults too.
 *
 * The region starts right after the guarded thread-stack pool
 * (THREAD_STACK_REGION_BASE + 128 slots * 24 KiB = 0xFFFFFFFF8C300000);
 * keep in sync with thread.c if that layout ever changes. */
#define IST_STACK_PAGES        (TSS_STACK_SIZE / 4096)
#define IST_STACK_GUARD_PAGES  1
#define IST_STACK_SLOT_BYTES   ((IST_STACK_PAGES + IST_STACK_GUARD_PAGES) * 4096ULL)
#define IST_STACK_REGION_BASE  0xFFFFFFFF8C300000ULL

static struct tss_entry tss_entries[MAX_TSS_CPUS];
static struct gdt_entry per_cpu_gdt[MAX_TSS_CPUS][GDT_NUM_ENTRIES];
static struct gdt_ptr per_cpu_gdtr[MAX_TSS_CPUS];

static int tss_cpu_valid(int cpu_id) {
    return cpu_id >= 0 && cpu_id < MAX_TSS_CPUS;
}

/* FIX_R1: back the per-CPU IST1 stack with PMM frames mapped into the
 * kernel PML4, leaving the page below it unmapped as the guard. */
static void ist1_stack_init_for_cpu(int cpu_id) {
    uint64_t region = IST_STACK_REGION_BASE + (uint64_t)cpu_id * IST_STACK_SLOT_BYTES;
    uint64_t usable = region + IST_STACK_GUARD_PAGES * 4096ULL;

    for (int i = 0; i < IST_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            PANIC("OOM allocating TSS IST1 stack");
        }
        paging_map(usable + (uint64_t)i * 4096ULL, phys,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXEC);
    }
}

void tss_load_for_cpu(int cpu_id) {
    if (!tss_cpu_valid(cpu_id)) {
        kprintf("[tss] WARN: invalid cpu_id=%d for load\n", cpu_id);
        return;
    }

    memcpy(per_cpu_gdt[cpu_id], gdt, PER_CPU_GDT_BYTES);
    gdt_set_tss_in(per_cpu_gdt[cpu_id], 5,
                   (uint64_t)(uintptr_t)&tss_entries[cpu_id],
                   sizeof(struct tss_entry) - 1);
    per_cpu_gdtr[cpu_id].limit = (uint16_t)(PER_CPU_GDT_BYTES - 1);
    per_cpu_gdtr[cpu_id].base = (uint64_t)(uintptr_t)&per_cpu_gdt[cpu_id][0];

    extern void gdt_flush(uint64_t gdtr_ptr);
    gdt_flush((uint64_t)(uintptr_t)&per_cpu_gdtr[cpu_id]);
    __asm__ volatile (
        "ltr %0"
        :
        : "r"((uint16_t)GDT_SEL_TSS)
    );
}

void tss_init(void) {
    memset(tss_entries, 0, sizeof(tss_entries));
    memset(per_cpu_gdt, 0, sizeof(per_cpu_gdt));
    memset(per_cpu_gdtr, 0, sizeof(per_cpu_gdtr));

    for (int cpu = 0; cpu < MAX_TSS_CPUS; cpu++) {
        ist1_stack_init_for_cpu(cpu);
        uint64_t region = IST_STACK_REGION_BASE + (uint64_t)cpu * IST_STACK_SLOT_BYTES;
        uint64_t ist1_top = region + IST_STACK_GUARD_PAGES * 4096ULL + TSS_STACK_SIZE;
        tss_entries[cpu].rsp0_low = 0;
        tss_entries[cpu].rsp0_high = 0;
        tss_entries[cpu].ist1_low = (uint32_t)(ist1_top & 0xFFFFFFFF);
        tss_entries[cpu].ist1_high = (uint32_t)(ist1_top >> 32);
        tss_entries[cpu].iomap_base = sizeof(struct tss_entry);
    }

    tss_load_for_cpu(0);

    /* FIX_R1: arm the IST.  The choice of vectors, stated (deliberately not
     * "all three by reflex" — each IST slot is a separate stack that must be
     * sized and never re-entered):
     *
     *   #DF (8)  -> IST1: ARMED.  The whole point of R1: a kernel fault on a
     *             bad stack (overflow, guard-page hit with RSP already dead)
     *             escalates to #DF; with ist=0 the CPU cannot push the #DF
     *             frame either and triple-faults with no output.  On IST1 the
     *             handler runs on a known-good stack, prints the FIX_R0 dump
     *             and halts.
     *
     *   NMI (2)  -> not armed.  Nothing in this tree sources NMIs (no
     *             watchdog, no perf, LINT1 unused); an IST slot for a case
     *             that cannot occur buys nothing.
     *
     *   #MC (18) -> not armed.  There is no MCE machinery at all (no MCG_CAP,
     *             no bank decode), so a dedicated stack would not make a
     *             machine check more survivable or more diagnosable than the
     *             generic dump it already gets.
     *
     * The gate is armed only now, AFTER the IST1 tops above are programmed
     * and the BSP's TSS is in TR — a #DF before this point behaves exactly
     * as it did before R1 (a reset), which is acceptable: until the stacks
     * exist there is nothing better to do.
     *
     * AURALITE_UNARM_IST is a deliberate A/B knob for the FIX_R1 test gate:
     * building with -DAURALITE_UNARM_IST turns the stack-overflow trigger
     * back into a silent reset, proving the gate measures the IST wiring. */
#ifndef AURALITE_UNARM_IST
    idt_set_ist(8, 1);
#else
    kprintf("[tss] WARN: AURALITE_UNARM_IST set -- #DF gate left on ist=0 (test knob)\n");
#endif
}

void tss_set_rsp0(uint64_t rsp0) {
    /* Route to the CALLING cpu's own TSS.  This used to hard-wire cpu 0,
     * which is why a thread's first-run stack programming (clone.c,
     * process.c, user.c -- all invoke this via their entry trampolines)
     * only worked as long as every thread ran on the BSP; with real SMP it
     * must target the per-CPU GDT/TSS of whichever cpu the thread actually
     * runs on.  Early boot (before smp_init() publishes cpu_local state)
     * still lands on cpu 0 via the guard. */
    int cpu_id = 0;
    if (cpu_local_ready) {
        struct cpu_local *c = get_cpu_local();
        if (c) cpu_id = (int)c->cpu_id;
    }
    tss_set_rsp0_for_cpu(cpu_id, rsp0);
}

void tss_set_rsp0_for_cpu(int cpu_id, uint64_t rsp0) {
    if (!tss_cpu_valid(cpu_id)) {
        kprintf("[tss] WARN: invalid cpu_id=%d for rsp0 update\n", cpu_id);
        return;
    }
    tss_entries[cpu_id].rsp0_low = (uint32_t)(rsp0 & 0xFFFFFFFF);
    tss_entries[cpu_id].rsp0_high = (uint32_t)(rsp0 >> 32);
}

uint64_t tss_get_ist1_top_for_cpu(int cpu_id) {
    if (!tss_cpu_valid(cpu_id)) {
        return 0;
    }
    return ((uint64_t)tss_entries[cpu_id].ist1_high << 32) |
           tss_entries[cpu_id].ist1_low;
}
