/* idt.c — build the 256-entry Interrupt Descriptor Table and load it (LIDT). */

#include <stdint.h>
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/lapic.h"

/* 256 handler entry-point addresses, laid out in isr.asm (.rodata). */
extern uint64_t isr_table[IDT_ENTRIES];

/* Non-static so smp.c can reload the IDT on application processors. */
struct idt_entry idt[IDT_ENTRIES] __attribute__((aligned(16)));
struct idt_ptr   idtp;

static inline void lidt_load(const struct idt_ptr *p) {
    /* Intel SDM Vol.2, LGDT/LIDT: load the IDTR from a 10-byte pseudo-descriptor. */
    __asm__ volatile ("lidt %0" : : "m"(*p));
}

void idt_set_gate(int n, uint64_t handler, uint8_t flags) {
    idt[n].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[n].selector    = KERNEL_CODE_SELECTOR;       /* 0x08 */
    idt[n].ist         = 0;                          /* no IST for now */
    idt[n].type_attr   = flags;
    idt[n].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[n].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[n].zero        = 0;
}

void idt_init(void) {
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, isr_table[i], IDT_GATE_INTERRUPT);
    }

    lidt_load(&idtp);

    /* Register TLB Shootdown IPI (vector 0xF0). */
    extern void ipi_tlb_shootdown_handler(void);
    idt_set_gate(0xF0, (uint64_t)ipi_tlb_shootdown_handler, IDT_GATE_INTERRUPT);
}
