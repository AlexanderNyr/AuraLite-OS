/* kernel/arch/riscv64/uart_rv.c -- 16550 RX over the PLIC
 * (RISCV_PLAN V7).  kbd32's ring discipline, third console: the IRQ
 * handler pushes, the reader pops, and the blocking wait sleeps on
 * wfi with interrupts ON -- the I7 cleared-IF deadlock has an
 * sstatus.SIE twin, and cons_rv_readline's wait carries the lineage
 * comment.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/uart_rv.h"
#include "kernel/arch/riscv64/plic.h"
#include "kernel/arch/riscv64/sbi.h"

/* 16550 registers, byte offsets from the MMIO base. */
#define R_RBR 0   /* RX buffer (read) */
#define R_IER 1   /* interrupt enable */
#define R_IIR 2   /* interrupt id (read) */
#define R_FCR 2   /* FIFO control (write) */
#define R_LCR 3   /* line control */
#define R_LSR 5   /* line status */

#define IER_RDA  0x01   /* received-data-available */
#define FCR_EN   0x01
#define FCR_CLR  0x06
#define LSR_DR   0x01

static volatile uint8_t *uart;

#define RING_SIZE 256
static volatile uint8_t  ring[RING_SIZE];
static volatile uint32_t ring_head, ring_tail;  /* head = write side */
static volatile uint64_t rx_count;

static void ring_push(uint8_t c)
{
    uint32_t next = (ring_head + 1) % RING_SIZE;
    if (next == ring_tail)
        return;                     /* full: drop (same as kbd32) */
    ring[ring_head] = c;
    ring_head = next;
}

static void uart_rv_irq(uint32_t irq)
{
    (void)irq;
    /* Drain everything pending: one PLIC claim can cover several
     * queued bytes, and a level line left half-drained re-fires
     * forever (the V2 completion lesson's RX spelling). */
    while (uart[R_LSR] & LSR_DR) {
        ring_push(uart[R_RBR]);
        rx_count++;
    }
}

void uart_rv_init(uint64_t base_hhdm_va, uint32_t irq)
{
    uart = (volatile uint8_t *)base_hhdm_va;

    /* FIFOs on and cleared, RX interrupt enabled.  Line parameters
     * (115200 8n1) are left as OpenSBI programmed them -- it already
     * printed a banner through this UART; re-programming a working
     * line is how bring-ups eat their own console. */
    uart[R_FCR] = FCR_EN | FCR_CLR;
    uart[R_IER] = IER_RDA;

    plic_enable(irq, uart_rv_irq);
    sbi_puts("[uart] 16550 RX armed: IRQ through the PLIC into the cons ring\n");
}

int uart_rv_getc(void)
{
    if (ring_tail == ring_head)
        return -1;
    uint8_t c = ring[ring_tail];
    ring_tail = (ring_tail + 1) % RING_SIZE;
    return c;
}

uint64_t uart_rv_rx_count(void)
{
    return rx_count;
}
