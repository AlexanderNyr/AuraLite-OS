/* kernel/arch/i386/irq32.h -- 8259A PIC + IRQ dispatch (I386_PLAN I2). */

#ifndef AURALITE_ARCH_I386_IRQ32_H
#define AURALITE_ARCH_I386_IRQ32_H

#include <stdint.h>
#include "kernel/arch/i386/isr.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_OFFSET 32
#define PIC_EOI    0x20

typedef void (*irq32_handler_t)(struct registers32 *regs);

void pic32_init(void);
void irq32_install(int irq, irq32_handler_t handler);
void irq32_unmask(int irq);
void irq32_dispatch(struct registers32 *regs);

/* PIT (8254) channel 0 at the given frequency; the handler bumps a
 * monotonic tick counter read by pit32_ticks(). */
void pit32_init(uint32_t freq_hz);
uint32_t pit32_ticks(void);

#endif /* AURALITE_ARCH_I386_IRQ32_H */
