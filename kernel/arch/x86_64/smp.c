/* smp.c — wake application processors via Limine's MP request.
 *
 * Each AP loads the shared GDT/IDT, switches to its own stack, reports online,
 * and enters an idle loop. Full scheduler integration (per-CPU run queues,
 * work stealing) is a follow-up; for now APs idle (hlt) which lets the BSP
 * run the kernel and shell uninterrupted.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "kernel/limine_requests.h"
#include "limine/limine.h"

#define AP_STACK_SIZE  (16 * 1024)
#define MAX_CPUS       32

/* Extern: implemented in gdt_flush.asm */
extern void gdt_flush(uint64_t gdtr_ptr);

/* Per-CPU AP stacks, pre-allocated before the APs start. */
static void *ap_stacks[MAX_CPUS];

/* Atomic counter: how many CPUs are online (BSP = 1 at boot). */
static volatile uint32_t cpus_online = 1;

/* ---- AP entry point ----
 * Called by Limine with RDI = pointer to this CPU's limine_mp_info.
 * extra_argument holds the CPU index we assigned in smp_init(). */
static void ap_entry(struct limine_mp_info *info) {
    uint64_t cpu_index = info->extra_argument;

    /* Switch to our own stack (Limine's stack is temporary). */
    __asm__ volatile (
        "mov %0, %%rsp\n"
        "xor %%rbp, %%rbp\n"
        :: "r"((uint64_t)ap_stacks[cpu_index] + AP_STACK_SIZE)
    );

    /* Load the kernel's GDT/IDT and a per-CPU TSS. */
    gdt_flush((uint64_t)(uintptr_t)&gdtr);
    __asm__ volatile ("lidt %0" :: "m"(idtp));
    tss_load_for_cpu((int)(cpu_index + 1));

    kprintf("[smp] AP #%llu online (lapic_id=%u, processor_id=%u)\n",
            (unsigned long long)cpu_index, info->lapic_id, info->processor_id);

    cpu_local_init(cpu_index + 1);
    lapic_enable();

    /* Atomically report online. */
    __sync_add_and_fetch(&cpus_online, 1);

    sched_idle();
}

void smp_init(void) {
    cpu_local_init(0);
    lapic_enable();
    uint64_t cpu_count = 0;
    uint32_t bsp_lapic_id = 0;
    struct limine_mp_info *cpus = limine_get_smp_info(&cpu_count, &bsp_lapic_id);

    if (cpus == NULL || cpu_count <= 1) {
        kprintf("[smp] single-CPU system (no APs to wake)\n");
        return;
    }

    kprintf("[smp] BSP lapic_id=%u, %llu total CPUs detected\n",
            bsp_lapic_id, (unsigned long long)cpu_count);

    /* The Limine MP response's cpus[] array includes the BSP, so we must
     * skip the entry whose lapic_id matches bsp_lapic_id. */
    volatile struct limine_mp_info *vcpus = (volatile struct limine_mp_info *)cpus;
    uint64_t ap_count = 0;
    for (uint64_t i = 0; i < cpu_count; i++) {
        if (vcpus[i].lapic_id == bsp_lapic_id) {
            continue;   /* skip the BSP */
        }
        if (ap_count >= MAX_CPUS) {
            break;
        }
        ap_stacks[ap_count] = kmalloc(AP_STACK_SIZE);
        if (ap_stacks[ap_count] == NULL) {
            kprintf("[smp] FATAL: OOM allocating AP stack %llu\n",
                    (unsigned long long)ap_count);
            return;
        }
        vcpus[i].extra_argument = ap_count;
        vcpus[i].goto_address   = (limine_goto_address)ap_entry;
        __asm__ volatile ("mfence" ::: "memory");
        kprintf("[smp]   AP %llu: lapic_id=%u -> goto set\n",
                (unsigned long long)ap_count, vcpus[i].lapic_id);
        ap_count++;
    }

    /* Wait for all APs to report online (BSP already counted as 1). */
    uint64_t target = ap_count + 1;
    kprintf("[smp] waiting for %llu APs to come online...\n",
            (unsigned long long)ap_count);

    /* Spin with hlt (interrupts enabled on BSP so the timer keeps ticking). */
    uint64_t timeout = 50000000;
    while ((uint64_t)cpus_online < target && timeout-- > 0) {
        __asm__ volatile ("pause");
    }

    if ((uint64_t)cpus_online >= target) {
        kprintf("[smp] all %llu CPUs online\n", (unsigned long long)cpus_online);
    } else {
        kprintf("[smp] TIMEOUT: only %u/%llu CPUs online\n",
                cpus_online, (unsigned long long)target);
    }
}

uint32_t smp_get_cpu_count(void) {
    return cpus_online;
}

void smp_self_test(void) {
    kprintf("[smp] self-test: %u CPU(s) online\n", cpus_online);
    if (cpus_online > 1) {
        kprintf("[smp] PASS: multi-core system detected\n");
    } else {
        kprintf("[smp] PASS: single-core system (use -smp N to test SMP)\n");
    }
}
