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
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/assert.h"

#define TSS_STACK_SIZE (16 * 1024)   /* 16 KiB */
#define MAX_TSS_CPUS   32
#define PER_CPU_GDT_BYTES (sizeof(struct gdt_entry) * GDT_NUM_ENTRIES)

static struct tss_entry tss_entries[MAX_TSS_CPUS];
static void *ist1_stacks[MAX_TSS_CPUS];
static struct gdt_entry per_cpu_gdt[MAX_TSS_CPUS][GDT_NUM_ENTRIES];
static struct gdt_ptr per_cpu_gdtr[MAX_TSS_CPUS];

static int tss_cpu_valid(int cpu_id) {
    return cpu_id >= 0 && cpu_id < MAX_TSS_CPUS;
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
    memset(ist1_stacks, 0, sizeof(ist1_stacks));
    memset(per_cpu_gdt, 0, sizeof(per_cpu_gdt));
    memset(per_cpu_gdtr, 0, sizeof(per_cpu_gdtr));

    for (int cpu = 0; cpu < MAX_TSS_CPUS; cpu++) {
        ist1_stacks[cpu] = kmalloc(TSS_STACK_SIZE);
        if (!ist1_stacks[cpu]) {
            PANIC("OOM allocating TSS IST1 stack");
        }
        uint64_t ist1_top = (uint64_t)ist1_stacks[cpu] + TSS_STACK_SIZE;
        tss_entries[cpu].rsp0_low = 0;
        tss_entries[cpu].rsp0_high = 0;
        tss_entries[cpu].ist1_low = (uint32_t)(ist1_top & 0xFFFFFFFF);
        tss_entries[cpu].ist1_high = (uint32_t)(ist1_top >> 32);
        tss_entries[cpu].iomap_base = sizeof(struct tss_entry);
    }

    tss_load_for_cpu(0);
}

void tss_set_rsp0(uint64_t rsp0) {
    tss_set_rsp0_for_cpu(0, rsp0);
}

void tss_set_rsp0_for_cpu(int cpu_id, uint64_t rsp0) {
    if (!tss_cpu_valid(cpu_id)) {
        kprintf("[tss] WARN: invalid cpu_id=%d for rsp0 update\n", cpu_id);
        return;
    }
    tss_entries[cpu_id].rsp0_low = (uint32_t)(rsp0 & 0xFFFFFFFF);
    tss_entries[cpu_id].rsp0_high = (uint32_t)(rsp0 >> 32);
}
