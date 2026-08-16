/* kernel/arch/i386/idt.c -- build the 256-entry IDT and load it
 * (I386_PLAN I2).  Sibling of kernel/arch/x86_64/idt.c, minus the two
 * things 32-bit mode does not have: the IST field (i386 gates have no
 * stack-table index; the double-fault story is a task gate, deferred
 * with an honest note in I386_PLAN §6) and the 64-bit offset half.
 */

#include <stdint.h>
#include "kernel/arch/i386/idt.h"

/* 256 handler entry-point addresses, laid out in isr_stubs32.asm. */
extern uint32_t isr_table32[IDT_ENTRIES];

static struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(16)));
static struct idt_ptr   idtp;

static inline void lidt_load(const struct idt_ptr *p)
{
    __asm__ volatile("lidt %0" : : "m"(*p));
}

void idt_set_gate(int n, uint32_t handler, uint8_t flags)
{
    idt[n].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[n].selector    = KERNEL_CODE_SELECTOR;
    idt[n].zero        = 0;
    idt[n].type_attr   = flags;
    idt[n].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
}

void idt_init(void)
{
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint32_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(i, isr_table32[i], IDT_GATE_INTERRUPT);

    lidt_load(&idtp);
}
