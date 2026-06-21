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

#define TSS_STACK_SIZE (16 * 1024)   /* 16 KiB */

static struct tss_entry tss;
static void *ist1_stack = NULL;

void tss_init(void) {
    memset(&tss, 0, sizeof(tss));

    /* Allocate a dedicated IST1 stack for the double-fault (#DF) handler. */
    ist1_stack = kmalloc(TSS_STACK_SIZE);
    uint64_t ist1_top = (uint64_t)ist1_stack + TSS_STACK_SIZE;

    tss.rsp0_low  = (uint32_t)(0);
    tss.rsp0_high = (uint32_t)(0);
    tss.ist1_low  = (uint32_t)(ist1_top & 0xFFFFFFFF);
    tss.ist1_high = (uint32_t)(ist1_top >> 32);
    tss.iomap_base = sizeof(tss);

    /* Install the 16-byte TSS descriptor at GDT index 5. */
    gdt_set_tss(5, (uint64_t)(uintptr_t)&tss, sizeof(tss) - 1);

    /* Load the Task Register (LTR). */
    __asm__ volatile (
        "ltr %0"
        :
        : "r"((uint16_t)GDT_SEL_TSS)
    );
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0_low  = (uint32_t)(rsp0 & 0xFFFFFFFF);
    tss.rsp0_high = (uint32_t)(rsp0 >> 32);
}
