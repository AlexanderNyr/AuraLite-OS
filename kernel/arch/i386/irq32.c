/* kernel/arch/i386/irq32.c -- 8259A PIC remap, IRQ dispatch and the PIT
 * (I386_PLAN I2).  The PIC/PIT halves mirror kernel/arch/x86_64/irq.c
 * and drivers/timer/pit.c at bring-up scope: same ICW sequence, same
 * remap (master->32, slave->40), same mode-3 square wave.  The
 * scheduler hook arrives with the scheduler in I4.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/irq32.h"
#include "kernel/arch/i386/portio.h"
#include "kernel/arch/i386/kprintf32.h"
#include "kernel/arch/i386/thread32.h"

#define NUM_IRQS   16
#define ICW1_ICW4  0x01
#define ICW1_INIT  0x10
#define ICW4_8086  0x01

static irq32_handler_t irq_handlers[NUM_IRQS];

void pic32_init(void)
{
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);  io_wait();

    outb(PIC1_DATA, PIC_OFFSET);            io_wait();  /* master -> 32 */
    outb(PIC2_DATA, PIC_OFFSET + 8);        io_wait();  /* slave  -> 40 */

    outb(PIC1_DATA, 0x04);                  io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02);                  io_wait();

    outb(PIC1_DATA, ICW4_8086);             io_wait();
    outb(PIC2_DATA, ICW4_8086);             io_wait();

    /* Mask everything; drivers unmask their own line. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void irq32_install(int irq, irq32_handler_t handler)
{
    if (irq >= 0 && irq < NUM_IRQS)
        irq_handlers[irq] = handler;
}

void irq32_unmask(int irq)
{
    if (irq < 0 || irq >= NUM_IRQS)
        return;
    if (irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1 << irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) & (uint8_t)~(1 << (irq - 8)));
        /* cascade line must be open for any slave IRQ */
        outb(PIC1_DATA, inb(PIC1_DATA) & (uint8_t)~(1 << 2));
    }
}

void irq32_dispatch(struct registers32 *regs)
{
    int irq = (int)regs->vector - PIC_OFFSET;

    if (irq >= 0 && irq < NUM_IRQS && irq_handlers[irq])
        irq_handlers[irq](regs);

    /* EOI: slave first when the line came through it. */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    /* Preemption point (I4).  Deliberately AFTER the EOI: a context
     * switch with the PIC un-acknowledged freezes IRQ0 for every
     * thread except the interrupted one.  The switched-out thread
     * parks inside sched32_maybe_preempt and finishes this dispatch
     * (frame unwind + iret) when it is next scheduled. */
    if (irq == 0)
        sched32_maybe_preempt();
}

/* ---- PIT ------------------------------------------------------------- */

#define PIT_BASE_HZ              1193182u
/* Mode 2, not mode 3: the x86_64 tree measured QEMU 10 delivering an
 * interrupt on BOTH square-wave transitions (200 Hz from a 100 Hz
 * divisor, wall clock 2x fast — see drivers/timer/pit.c for the
 * numbers).  The i386 kernel carried the same 0x36; same fix. */
#define PIT_CMD_CHAN0_LOHI_MODE2 0x34

static volatile uint32_t timer_ticks;

static void pit32_irq(struct registers32 *regs)
{
    (void)regs;
    timer_ticks++;
    sched32_tick();      /* quantum bookkeeping; the switch happens
                          * post-EOI in irq32_dispatch */
}

uint32_t pit32_ticks(void)
{
    return timer_ticks;
}

void pit32_init(uint32_t freq_hz)
{
    uint32_t divisor = PIT_BASE_HZ / freq_hz;

    outb(0x43, PIT_CMD_CHAN0_LOHI_MODE2);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    irq32_install(0, pit32_irq);
    irq32_unmask(0);

    kprintf32("[timer] PIT programmed: %u Hz (divisor %u)\n",
              freq_hz, divisor);
}
