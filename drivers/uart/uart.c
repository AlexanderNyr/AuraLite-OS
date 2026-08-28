/* uart.c — 16550 UART (COM1) serial driver.
 *
 * OPT_PLAN.md O3: TX is ring-buffered and interrupt-driven once the IRQ
 * layer is up.  The shape:
 *
 *   - Before uart_tx_ring_enable() (called from kmain right after the
 *     IDT/PIC are live and interrupts are on), every byte takes the
 *     historical synchronous path: busy-wait on LSR.THRE, one outb.
 *     That path never goes away — it is also where panic bytes and
 *     ring-full spill land, and PERF_UART_TX_SYNC_BYTES counts exactly
 *     those, so /proc/perf can prove the ring is doing the carrying.
 *
 *   - After enable, uart_putchar() enqueues into a 16 KiB ring and then
 *     OPPORTUNISTICALLY drains: if the FIFO is empty it writes up to 16
 *     bytes immediately (no waiting — LSR is checked, never spun on).
 *     When the FIFO is busy, the THRE interrupt (IRQ 4) drains the rest,
 *     16 bytes per fire.  The THRE interrupt is enabled only while the
 *     ring is non-empty, so an idle UART raises no interrupts at all.
 *
 *   - Ring full: the caller drains synchronously until there is room
 *     (D3: a kernel log NEVER drops bytes — the ring changes who waits,
 *     not whether bytes arrive).  Those bytes count as sync.
 *
 *   - uart_flush() (kernel_halt, D4): latch back to synchronous mode so
 *     every subsequent byte — the dying words — goes straight out, then
 *     drain whatever the ring still holds.  The lock acquire there is
 *     BOUNDED: if the lock holder died mid-drain, torn output beats
 *     silence.
 *
 * Registration note: the IRQ-4 handler is registered by kmain through a
 * thunk in kernel.c, not here — irq_register_handler lives in
 * kernel/arch/x86_64/irq.h and this file deliberately does not include
 * it (the I6 include ratchet holds portable files at their current
 * count of direct x86_64 includes; kernel.c already pays that toll).
 * IRQ 4 works in both delivery eras: PIC (pic_unmask at registration)
 * and, after ioapic_init(), the identity-mapped GSI 4 redirection entry.
 */

#include "kernel/arch/arch.h"
#include "drivers/uart/uart.h"
#include "drivers/uart/uart_ring.h"
#include "kernel/lib/perfstat.h"

/* SELFHOST SH5c: the GCC-style atomic spellings this file uses are tcc
 * builtins only in their non-_n forms.  tcc's <stdatomic.h> supplies
 * __atomic_store_n/__atomic_load_n macros and the __ATOMIC_* orders, but
 * has no __atomic_exchange_n — map it onto the 4-argument __atomic_exchange
 * that both tcc and clang/gcc implement natively (result delivered through
 * the third argument on gcc/clang; tcc also returns it, which we ignore). */
#include <stdatomic.h>
#ifdef __TINYC__
#define __atomic_exchange_n(ptr, val, mo)                                 \
    ({ __typeof__(*(ptr)) __v = (val), __r;                               \
       __atomic_exchange((ptr), &__v, &__r, (mo));                        \
       __r; })
#endif

#define UART_TX_RING_SIZE 16384u   /* power of two; uart_ring.h requires it */
#define UART_FIFO_DEPTH   16       /* 16550 TX FIFO */

static uint8_t     tx_buf[UART_TX_RING_SIZE];
static uart_ring_t tx_ring;                /* zero-initialised: empty */
static volatile int tx_ring_mode = 0;      /* 0 = sync (early/panic), 1 = ring */

/* One flat byte-lock, private to the TX path.  Not the generic spinlock:
 * uart_flush() needs a BOUNDED acquire (see header comment), and the
 * generic API deliberately has no trylock. */
static volatile uint8_t tx_lock;

static void tx_lock_acquire(void) {
    while (__atomic_exchange_n(&tx_lock, 1, __ATOMIC_ACQUIRE)) {
        arch_cpu_relax();
    }
}

static void tx_lock_release(void) {
    __atomic_store_n(&tx_lock, 0, __ATOMIC_RELEASE);
}

void uart_init(void) {
    const uint16_t base = UART_COM1;

    outb(base + UART_IER, 0x00);           /* disable all interrupts */
    outb(base + UART_LCR, 0x80);           /* enable DLAB (set baud divisor) */
    outb(base + UART_DL_LO, UART_BAUD_DIVISOR & 0xFF);
    outb(base + UART_DL_HI, (UART_BAUD_DIVISOR >> 8) & 0xFF);
    outb(base + UART_LCR, 0x03);           /* 8 bits, no parity, 1 stop; DLAB off */
    outb(base + UART_FCR, 0xC7);           /* enable + clear FIFO, 14-byte threshold */
    outb(base + UART_MCR, 0x0B);           /* RTS/DSR set, OUT2 (IRQs routed) */
}

/* Enable/disable the THRE (transmitter-empty) interrupt, IER bit 1.  RX
 * interrupts stay off — the TTY polls, as before. */
static void ier_thre(int on) {
    uint8_t v = inb(UART_COM1 + UART_IER);
    uint8_t nv = on ? (uint8_t)(v | 0x02) : (uint8_t)(v & ~0x02);
    if (nv != v) {
        outb(UART_COM1 + UART_IER, nv);
    }
}

/* One synchronous byte: the historical path, and the only place that
 * busy-waits on THRE.  Counts into the sync perfstat. */
static void tx_sync_byte(uint8_t c) {
    while ((inb(UART_COM1 + UART_LSR) & UART_LSR_THRE) == 0) {
        /* spin */
    }
    outb(UART_COM1 + UART_THR, c);
    perfstat_add(PERF_UART_TX_SYNC_BYTES, 1);
}

/* Drain ring -> FIFO while the FIFO has room.  NEVER waits: LSR.THRE is
 * checked, and if the FIFO is busy we leave the rest to the interrupt.
 * Caller holds tx_lock. */
static void hw_drain_locked(void) {
    while (!uring_empty(&tx_ring)) {
        if ((inb(UART_COM1 + UART_LSR) & UART_LSR_THRE) == 0) {
            return;                        /* FIFO busy: IRQ takes over */
        }
        for (int i = 0; i < UART_FIFO_DEPTH && !uring_empty(&tx_ring); i++) {
            outb(UART_COM1 + UART_THR,
                 uring_pop(&tx_ring, tx_buf, UART_TX_RING_SIZE));
            perfstat_add(PERF_UART_TX_RING_BYTES, 1);
        }
    }
}

void uart_putchar(char c) {
    if (!tx_ring_mode) {
        tx_sync_byte((uint8_t)c);
        return;
    }

    uint64_t fl = arch_irq_save();
    tx_lock_acquire();

    /* Ring full: drain synchronously until there is room.  Never drop —
     * the log's byte-fidelity is what every integration grep stands on. */
    while (uring_full(&tx_ring, UART_TX_RING_SIZE)) {
        tx_sync_byte(uring_pop(&tx_ring, tx_buf, UART_TX_RING_SIZE));
    }

    uring_push(&tx_ring, tx_buf, UART_TX_RING_SIZE, (uint8_t)c);
    hw_drain_locked();
    ier_thre(!uring_empty(&tx_ring));

    tx_lock_release();
    arch_irq_restore(fl);
}

/* THRE interrupt body (IRQ 4).  Registered via the kernel.c thunk;
 * interrupts are already off in IRQ context. */
void uart_tx_irq(void) {
    tx_lock_acquire();
    hw_drain_locked();
    ier_thre(!uring_empty(&tx_ring));
    tx_lock_release();
}

/* Flip TX into ring mode.  Called once by kmain after the IRQ-4 handler
 * is registered. */
void uart_tx_ring_enable(void) {
    tx_ring_mode = 1;
}

/* Synchronous drain for the death paths (kernel_halt) and anything that
 * must not lose bytes across a world change.  D4: after this call every
 * future byte is synchronous too. */
void uart_flush(void) {
    tx_ring_mode = 0;                      /* dying words go straight out */

    /* Bounded acquire: the legitimate holder drains at most one FIFO
     * burst with IRQs off, so a million relaxed spins is geological time
     * — if it still holds after that, it died mid-drain and we proceed
     * unlocked (torn output beats silence at halt). */
    int got = 0;
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (!__atomic_exchange_n(&tx_lock, 1, __ATOMIC_ACQUIRE)) {
            got = 1;
            break;
        }
        arch_cpu_relax();
    }

    while (!uring_empty(&tx_ring)) {
        tx_sync_byte(uring_pop(&tx_ring, tx_buf, UART_TX_RING_SIZE));
    }
    ier_thre(0);

    if (got) {
        tx_lock_release();
    }
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putchar(*s++);
    }
}

int uart_has_data(void) {
    return (inb(UART_COM1 + UART_LSR) & UART_LSR_DR) != 0;
}

char uart_getchar(void) {
    /* RBR and THR share port 0x3F8; reading it returns the received byte. */
    return (char)inb(UART_COM1 + UART_THR);
}
