#ifndef NOVOS_ARCH_X86_64_IRQ_H
#define NOVOS_ARCH_X86_64_IRQ_H

#include <stdint.h>
#include "kernel/arch/x86_64/isr.h"

/*
 * 8259A PIC I/O ports and the End-Of-Interrupt command. IRQ lines are remapped
 * by pic_init() onto vectors PIC_OFFSET .. PIC_OFFSET+15 (32..47).
 */

#define PIC1_CMD      0x20
#define PIC1_DATA     0x21
#define PIC2_CMD      0xA0
#define PIC2_DATA     0xA1

#define PIC_EOI       0x20
#define PIC_OFFSET    0x20

typedef void (*irq_handler_t)(struct registers *regs);

void pic_init(void);
void pic_eoi(int irq);
void pic_mask(int irq);
void pic_unmask(int irq);

void irq_register_handler(int irq, irq_handler_t handler);
void irq_dispatch(int irq, struct registers *regs);

#endif /* NOVOS_ARCH_X86_64_IRQ_H */
