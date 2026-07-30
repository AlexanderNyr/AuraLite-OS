/* irq.c — 8259A Programmable Interrupt Controller + IRQ dispatch table.
 *
 * The legacy PIC maps IRQs 0-15 to CPU vectors 8-15 by default, which collides
 * with exceptions (#DF=8, etc.). We remap master->32 and slave->40 so hardware
 * IRQs land in the free 32-47 range, behind the exception block.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/lapic.h"
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

    /* Force IMCR into PIC mode so the 8259 cascade is actually wired to the
     * BSP's INTR line. On real hardware the firmware frequently leaves this
     * switched to APIC mode (or an MP-compliant chipset defaults to it),
     * which makes every legacy IRQ (timer, keyboard, mouse, ...) silently
     * vanish: drivers register/unmask cleanly, but their handlers are never
     * invoked because the interrupt never reaches the CPU core. Writing the
     * IMCR is a no-op / harmless on chipsets that don't implement it
     * (single-CPU systems without an MP config table), so this is safe to
     * do unconditionally. See kernel/arch/x86_64/irq.h for background. */
    outb(IMCR_ADDR_PORT, IMCR_SELECT);
    outb(IMCR_DATA_PORT, IMCR_MODE_PIC);
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
    /* Slave-PIC IRQs (8-15) are wired through the master PIC's cascade input
     * on IRQ2.  If that line stays masked, no slave-PIC interrupt (including
     * IRQ12 for the PS/2 mouse and IRQ14/15 for legacy IDE) ever reaches the
     * CPU, even though pic_unmask() above correctly clears the bit on PIC2.
     * Without this, the mouse driver initialises successfully and reports
     * "ready", but mouse_handler() is never invoked, so the GUI cursor and
     * anything else depending on slave-PIC IRQs silently never updates. */
    if (irq >= 8) {
        pic_unmask(2);
    }
}

void irq_dispatch(int irq, struct registers *regs) {
    /* Acknowledge the interrupt BEFORE running the handler.  This is essential
     * for preemptive scheduling: the timer handler may context-switch away,
     * and the PIC must be free to deliver the next tick when we eventually
     * return.  For edge-triggered IRQs (like the PIT) early EOI is safe. */
    pic_eoi(irq);
    /* Also signal EOI to the LOCAL APIC.  Two delivery paths share this
     * dispatcher:
     *   - BSP legacy IRQs arrive via LINT0 in ExtINT mode; the 8259's vector
     *     comes from the INTA bus cycle and never enters the LAPIC's
     *     in-service register, so writing EOI there is a harmless no-op.
     *   - Each AP's own LAPIC timer (vector 32 == IRQ 0's slot, started by
     *     ap_entry() in smp.c) DOES set the in-service bit, and withholding
     *     EOI blocks every later interrupt at or below that priority --
     *     including all subsequent timer ticks, freezing that CPU's
     *     scheduler after the very first preemption.
     * lapic_eoi() itself is guarded (no-op until lapic_enable() mapped the
     * MMIO page), so this is safe in early boot too. */
    lapic_eoi();
    if (irq >= 0 && irq < NUM_IRQS && irq_handlers[irq] != NULL) {
        irq_handlers[irq](regs);
    }
}
