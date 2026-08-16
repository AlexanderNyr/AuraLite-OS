/* kernel/arch/i386/idt.h -- 32-bit IDT layout (I386_PLAN I2). */

#ifndef AURALITE_ARCH_I386_IDT_H
#define AURALITE_ARCH_I386_IDT_H

#include <stdint.h>
#include "kernel/arch/i386/gdt.h"

#define IDT_ENTRIES 256

/* type_attr: P | DPL | 0 | gate type.
 * 0x8E = present, DPL0, 32-bit interrupt gate (IF cleared on entry).
 * 0xEE = present, DPL3, 32-bit interrupt gate -- for int 0x80 in I4. */
#define IDT_GATE_INTERRUPT      0x8E
#define IDT_GATE_INTERRUPT_USER 0xEE

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

void idt_init(void);
void idt_set_gate(int n, uint32_t handler, uint8_t flags);

#endif /* AURALITE_ARCH_I386_IDT_H */
