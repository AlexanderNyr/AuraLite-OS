/* irq.c — 8259A Programmable Interrupt Controller + IRQ dispatch table.
 *
 * The legacy PIC maps IRQs 0-15 to CPU vectors 8-15 by default, which collides
 * with exceptions (#DF=8, etc.). We remap master->32 and slave->40 so hardware
 * IRQs land in the free 32-47 range, behind the exception block.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/portio.h"

#define NUM_IRQS 16
#define ICW1_ICW4   0x01   /* ICW4 needed                */
#define ICW1_INIT   0x10   /* initialisation             */
#define ICW4_8086   0x01   /* 8086 mode                  */

static irq_handler_t irq_handlers[NUM_IRQS];

/* Intel 8259A: initialization sequence (ICW1..4), 8259A datasheet. */
void pic_init(void) {
    /* Start the init sequence on both PICs. */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();

    /* ICW2: vector offsets (master 32, slave 40). */
    outb(PIC1_DATA, PIC_OFFSET);            io_wait();
    outb(PIC2_DATA, PIC_OFFSET + 8);        io_wait();

    /* ICW3: master is told the slave hangs off IRQ2 (bit 2); slave gets
       its cascade identity (2). */
    outb(PIC1_DATA, 0x04);                  io_wait();
    outb(PIC2_DATA, 0x02);                  io_wait();

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, ICW4_8086);             io_wait();
    outb(PIC2_DATA, ICW4_8086);             io_wait();

    /* Mask every IRQ until a driver claims it. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(int irq) {
    /* Slave IRQs require an EOI to both PICs. */
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (uint8_t)(1 << (irq % 8)));
}

void pic_unmask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & (uint8_t)~(1 << (irq % 8)));
}

void irq_register_handler(int irq, irq_handler_t handler) {
    if (irq < 0 || irq >= NUM_IRQS) {
        return;
    }
    irq_handlers[irq] = handler;
    pic_unmask(irq);
}

void irq_dispatch(int irq, struct registers *regs) {
    if (irq >= 0 && irq < NUM_IRQS && irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }
    pic_eoi(irq);   /* always acknowledge, even if unhandled */
}
