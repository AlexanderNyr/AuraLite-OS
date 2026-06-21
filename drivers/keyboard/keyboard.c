/* keyboard.c — PS/2 keyboard driver using scan-code set 1.
 *
 * IRQ 1 (vector 33) fires when a byte is available at port 0x60. We translate
 * scan codes to ASCII and store them in a ring buffer. Extended (E0) codes
 * and key-release (0xF0 / high bit) codes are handled but only key-presses
 * produce output.
 */

#include <stdint.h>
#include "drivers/keyboard/keyboard.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/isr.h"

#define KB_DATA   0x60
#define KB_STATUS 0x64

/* Ring buffer. */
static char    kb_buffer[KB_BUF_SIZE];
static volatile uint32_t kb_head = 0;   /* write index (IRQ context) */
static volatile uint32_t kb_tail = 0;   /* read index  (poll context) */

/* State for extended-key handling. */
static int kb_extended = 0;

/* US QWERTY scan-code-set-1 → ASCII (for codes 0x02..0x35, i.e. row keys). */
static const char scancode_map[128] = {
    0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',  /* 0x00-0x0E */
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',     /* 0x0F-0x1C */
    0, 'a','s','d','f','g','h','j','k','l',';','\'', '`',           /* 0x1D-0x29 */
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,            /* 0x2A-0x35 */
    '*', 0, ' ',                                                    /* 0x36-0x39 */
    /* Rest are zeros (function keys, numpad, etc. — not mapped). */
};

static void kb_enqueue(char c) {
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {   /* buffer not full */
        kb_buffer[kb_head] = c;
        kb_head = next;
    }
}

static void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t sc = inb(KB_DATA);

    if (sc == 0xE0) {
        kb_extended = 1;
        return;
    }

    /* Bit 7 set = key release; ignore. */
    if (sc & 0x80) {
        kb_extended = 0;
        return;
    }

    /* Translate. */
    if (!kb_extended && sc < sizeof(scancode_map)) {
        char c = scancode_map[sc];
        if (c) {
            kb_enqueue(c);
        }
    }
    kb_extended = 0;
}

void keyboard_init(void) {
    /* Drain any pending data. */
    while (inb(KB_STATUS) & 0x01) {
        inb(KB_DATA);
    }
    irq_register_handler(1, keyboard_handler);
}

int keyboard_getchar(void) {
    if (kb_head == kb_tail) {
        return -1;   /* empty */
    }
    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}
