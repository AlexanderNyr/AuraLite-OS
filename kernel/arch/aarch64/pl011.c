/* kernel/arch/aarch64/pl011.c -- the PL011 console (ARM64_PLAN A0
 * TX; A7 grew the ring TX [AMEND-3] and the IRQ-driven RX).
 *
 * TX: bytes go through drivers/uart/uart_ring.h -- the OPT O3 pure
 * index core, host-tested across wrap/full/empty and the 2^32
 * counter crossing (75 checks) BEFORE it ever saw a UART.  "Same
 * shape, different registers" was the O3 assessment of this port,
 * and the index arithmetic is the part that is identical: this file
 * adds only the PL011 spelling of `FIFO has room` (FR.TXFF) and
 * `interrupt me when it drains` (IMSC.TXIM).  Until
 * pl011_tx_ring_enable() the path is the A0 synchronous poll --
 * early boot and panic prints must not depend on interrupts working
 * (uart.c's rule, fourth arch).
 *
 * RX: uart_rv.c's discipline at the fourth tenant -- the IRQ handler
 * pushes, the reader pops, and one GIC claim drains EVERYTHING
 * pending (a level line left half-drained re-fires forever; the V2
 * completion lesson's RX spelling).  pl011_try_getc() falls back to
 * the polled A5c read until the IRQ is armed, so the boot sequence
 * before gic_init keeps a console.
 *
 * The ring manipulation runs under arch_irq_save/restore -- the A6
 * DAIF backend, consumed here the way x86's uart.c consumes its own
 * backend (the TX IRQ preempting a half-done pop would double-pop;
 * masked, it cannot).
 *
 * The base address is the virt board's (plan Fact 3, dumped DTB:
 * pl011@9000000).
 */

#include <stdint.h>

#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/gic.h"
#include "kernel/arch/aarch64/irqflags.h"
#include "drivers/uart/uart_ring.h"

/* HHDM view since A3: the kernel runs higher-half with TTBR0 dropped
 * after paging_a64_init, and the early TTBR1 window already covers
 * PA 0..4G -- so the UART is reachable at HHDM+0x09000000 from the
 * first higher-half instruction, and a bare physical 0x09000000
 * would fault the moment the MMU turns on (measured: the first A3
 * boot hung exactly there, silently -- the banner's own printer was
 * the unmapped address). */
#define PL011_BASE (0xFFFFFFC000000000UL + 0x09000000UL)

#define PL011_DR   0x00u
#define PL011_FR   0x18u
#define PL011_IMSC 0x38u   /* interrupt mask set/clear */
#define PL011_MIS  0x40u   /* masked interrupt status */
#define PL011_ICR  0x44u   /* interrupt clear */

#define PL011_FR_RXFE (1u << 4)
#define PL011_FR_TXFF (1u << 5)

#define PL011_INT_RX (1u << 4)   /* RXIM / RXIC */
#define PL011_INT_TX (1u << 5)   /* TXIM / TXIC */
#define PL011_INT_RT (1u << 6)   /* receive timeout */

static volatile uint32_t *reg32(uint32_t off)
{
    return (volatile uint32_t *)(PL011_BASE + off);
}

/* ---- TX: the O3 ring core under the PL011 registers [AMEND-3] ---- */

#define TX_RING_SIZE 4096u          /* power of two; uart_ring.h requires it */
static uint8_t     tx_buf[TX_RING_SIZE];
static uart_ring_t tx_ring;             /* zero-initialised: empty */
static volatile int tx_ring_mode;       /* 0 = sync (early/panic), 1 = ring */

static void tx_sync_byte(uint8_t c)
{
    while (*reg32(PL011_FR) & PL011_FR_TXFF)
        ;
    *reg32(PL011_DR) = c;
}

/* Pump ring bytes into the FIFO while there is room; arm TXIM for
 * whatever remains (interrupt-driven tail drain).  Caller holds the
 * IRQ mask. */
static void tx_pump_locked(void)
{
    while (!uring_empty(&tx_ring) && !(*reg32(PL011_FR) & PL011_FR_TXFF))
        *reg32(PL011_DR) = uring_pop(&tx_ring, tx_buf, TX_RING_SIZE);

    uint32_t imsc = *reg32(PL011_IMSC);
    if (!uring_empty(&tx_ring))
        *reg32(PL011_IMSC) = imsc | PL011_INT_TX;
    else
        *reg32(PL011_IMSC) = imsc & ~PL011_INT_TX;
}

void pl011_putc(char c)
{
    if (!tx_ring_mode) {
        tx_sync_byte((uint8_t)c);
        return;
    }

    arch_irqflags_t f = arch_irq_save();
    if (uring_full(&tx_ring, TX_RING_SIZE)) {
        /* Full ring: drain one byte synchronously rather than drop
         * or spin unbounded (uart.c's overflow rule verbatim). */
        tx_sync_byte(uring_pop(&tx_ring, tx_buf, TX_RING_SIZE));
    }
    uring_push(&tx_ring, tx_buf, TX_RING_SIZE, (uint8_t)c);
    tx_pump_locked();
    arch_irq_restore(f);
}

void pl011_tx_ring_enable(void)
{
    tx_ring_mode = 1;
}

void pl011_tx_flush(void)
{
    arch_irqflags_t f = arch_irq_save();
    while (!uring_empty(&tx_ring))
        tx_sync_byte(uring_pop(&tx_ring, tx_buf, TX_RING_SIZE));
    arch_irq_restore(f);
}

/* ---- RX: IRQ-fed cons ring (SPI 1 -> INTID 33 via the DTB) ---- */

#define RX_RING_SIZE 256u
static uint8_t     rx_buf[RX_RING_SIZE];
static uart_ring_t rx_ring;
static volatile int rx_armed;
static volatile uint64_t rx_count;
static volatile uint64_t rx_polled_count;   /* lost-edge recoveries */

static void pl011_irq(uint32_t intid)
{
    (void)intid;
    uint32_t mis = *reg32(PL011_MIS);

    if (mis & (PL011_INT_RX | PL011_INT_RT)) {
        /* Drain everything pending: one claim can cover several
         * queued bytes, and a level line left half-drained re-fires
         * forever (the V2 completion lesson's RX spelling). */
        while (!(*reg32(PL011_FR) & PL011_FR_RXFE)) {
            if (!uring_full(&rx_ring, RX_RING_SIZE)) {
                uring_push(&rx_ring, rx_buf, RX_RING_SIZE,
                           (uint8_t)(*reg32(PL011_DR) & 0xFF));
                rx_count++;
            } else {
                (void)(*reg32(PL011_DR));   /* full: drop (kbd32's rule) */
            }
        }
        *reg32(PL011_ICR) = PL011_INT_RX | PL011_INT_RT;
    }

    if (mis & PL011_INT_TX) {
        tx_pump_locked();                   /* IRQs already masked here */
        *reg32(PL011_ICR) = PL011_INT_TX;
    }
}

void pl011_rx_init(uint32_t intid)
{
    /* RX + receive-timeout interrupts on; line parameters are left
     * as QEMU's reset state set them -- it has been printing through
     * this UART since A0, and re-programming a working line is how
     * bring-ups eat their own console (the 16550 lesson, verbatim). */
    *reg32(PL011_IMSC) = *reg32(PL011_IMSC) | PL011_INT_RX | PL011_INT_RT;
    gic_enable(intid, pl011_irq);
    rx_armed = 1;
    pl011_puts("[uart] pl011 rx armed: IRQ through the GIC into the "
               "cons ring\n");
}

int pl011_try_getc(void)
{
    if (rx_armed) {
        int c = -1;
        arch_irqflags_t f = arch_irq_save();
        if (!uring_empty(&rx_ring)) {
            c = uring_pop(&rx_ring, rx_buf, RX_RING_SIZE);
        } else if (!(*reg32(PL011_FR) & PL011_FR_RXFE)) {
            /* LOST-EDGE RECOVERY (measured on the first CI matrix
             * runs, QEMU 8.2: the drivers-smoke session stalled at
             * `auralite# un` -- byte-for-byte identical logs at 60 s
             * AND 180 s timeouts, so not timing but a dropped RX
             * interrupt edge with bytes queued in the chardev during
             * a long boot).  A byte sitting in the pl011 with no IRQ
             * ever coming would hang the shell forever; the 100 Hz
             * timer already wakes the read loop's wfi, so polling FR
             * here turns that hang into a <=10 ms hiccup.  Counted
             * separately -- the IRQ receipt the smokes assert must
             * stay a receipt for IRQs. */
            c = (int)(*reg32(PL011_DR) & 0xFF);
            rx_polled_count++;
        }
        arch_irq_restore(f);
        return c;
    }

    /* A5c's polled fallback, kept for the window before gic_init.
     * FR bit 4 (RXFE) high means the RX FIFO is empty; DR low byte
     * is the character. */
    if (*reg32(PL011_FR) & PL011_FR_RXFE)
        return -1;
    return (int)(*reg32(PL011_DR) & 0xFF);
}

uint64_t pl011_rx_count(void)
{
    return rx_count;
}

uint64_t pl011_rx_polled_count(void)
{
    return rx_polled_count;
}

/* ---- the A0 printers, unchanged above the putc seam ---- */

void pl011_puts(const char *s)
{
    while (*s)
        pl011_putc(*s++);
}

void pl011_puthex64(uint64_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    pl011_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        pl011_putc(digits[(v >> shift) & 0xF]);
}

void pl011_putdec64(uint64_t v)
{
    char buf[21];
    int  i = 20;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    pl011_puts(&buf[i]);
}
