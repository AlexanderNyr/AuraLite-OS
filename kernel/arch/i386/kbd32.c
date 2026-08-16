/* kernel/arch/i386/kbd32.c -- PS/2 keyboard + console input
 * (I386_PLAN I7).
 *
 * The scancode tables mirror drivers/keyboard/keymap.c's US layout at
 * set-1 bring-up scope (the 64-bit driver's richer key-event ring,
 * layout selection and E0 handling are I8 residue -- the ratchet
 * tracks the include when the shared driver ports).  Shift is the one
 * modifier a shell cannot live without, so shift is the one modifier
 * implemented.
 */

#include <stdint.h>

#include "kernel/arch/i386/kbd32.h"
#include "kernel/arch/i386/irq32.h"
#include "kernel/arch/i386/isr.h"
#include "kernel/arch/i386/portio.h"
#include "kernel/arch/i386/kprintf32.h"

#define KBD_DATA 0x60

/* Scancode set 1, US layout, press codes 0x00..0x39. */
static const char map_lo[0x3A] = {
      0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
   '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
      0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
      0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
      0, '*', 0, ' '
};

static const char map_hi[0x3A] = {
      0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
   '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
      0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
      0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
      0, '*', 0, ' '
};

#define LSHIFT_DOWN 0x2A
#define RSHIFT_DOWN 0x36
#define RELEASE_BIT 0x80

/* ---- the input ring ----------------------------------------------------- */

#define RING_SIZE 256
static volatile uint8_t  ring[RING_SIZE];
static volatile uint32_t ring_head, ring_tail;   /* head = write side */

static void ring_push(uint8_t c)
{
    uint32_t next = (ring_head + 1) % RING_SIZE;
    if (next == ring_tail)
        return;                     /* full: drop, never block an IRQ */
    ring[ring_head] = c;
    ring_head = next;
}

int cons32_getc(void)
{
    /* Opportunistically drain the polled UART into the same ring so
     * serial and PS/2 input interleave in arrival order. */
    while (uart32_has_byte())
        ring_push(uart32_read_byte());

    if (ring_tail == ring_head)
        return -1;
    uint8_t c = ring[ring_tail];
    ring_tail = (ring_tail + 1) % RING_SIZE;
    return c;
}

/* ---- the PS/2 side ------------------------------------------------------ */

static int shift_down;

static void kbd32_irq(struct registers32 *regs)
{
    (void)regs;
    uint8_t sc = inb(KBD_DATA);

    if (sc == LSHIFT_DOWN || sc == RSHIFT_DOWN) {
        shift_down = 1;
        return;
    }
    if (sc == (LSHIFT_DOWN | RELEASE_BIT) || sc == (RSHIFT_DOWN | RELEASE_BIT)) {
        shift_down = 0;
        return;
    }
    if (sc & RELEASE_BIT)
        return;                     /* other releases: ignore */
    if (sc >= sizeof(map_lo))
        return;                     /* E0 extensions etc.: I8 residue */

    char c = shift_down ? map_hi[sc] : map_lo[sc];
    if (c)
        ring_push((uint8_t)c);
}

void kbd32_init(void)
{
    /* Flush anything stale in the output buffer. */
    while (inb(0x64) & 1)
        (void)inb(KBD_DATA);

    irq32_install(1, kbd32_irq);
    irq32_unmask(1);
    kprintf32("[boot] PS/2 keyboard on IRQ 1 (set 1, US map, shift)\n");
}

/* ---- cooked-line read ---------------------------------------------------- */

uint32_t cons32_readline(char *buf, uint32_t cap)
{
    uint32_t n = 0;

    for (;;) {
        int ci = cons32_getc();
        if (ci < 0) {
            /* Nothing pending: sleep until the next interrupt (PIT or
             * keyboard).  `sti` is load-bearing, not decoration: this
             * runs inside an int 0x80 INTERRUPT gate, which cleared
             * IF on entry -- a bare `hlt` here deadlocks the machine
             * with a prompt on screen (measured: the first I7 boot
             * did exactly that).  sti;hlt re-enables interrupts for
             * the sleep; the CPU takes the wake interrupt after hlt
             * retires, and cli restores the gate's contract before
             * the ring is re-examined. */
            __asm__ volatile("sti; hlt; cli");
            continue;
        }

        char c = (char)ci;
        if (c == '\r')
            c = '\n';

        if (c == '\b' || c == 0x7F) {
            if (n > 0) {
                n--;
                kputs32("\b \b");
            }
            continue;
        }

        if (c == '\n') {
            kputc32('\n');
            if (n < cap)
                buf[n++] = '\n';
            return n;
        }

        if (n + 1 < cap) {          /* leave room for the newline */
            buf[n++] = c;
            kputc32(c);             /* echo */
        }
    }
}
