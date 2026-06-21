/* mouse.c — PS/2 mouse driver (8042 auxiliary channel, IRQ 12).
 *
 * The 8042 controller manages two devices: the keyboard (port 0x60, IRQ 1)
 * and the PS/2 mouse (auxiliary, IRQ 12). To send commands to the mouse we
 * write 0xD4 to the command port (0x64), then write the mouse command to
 * the data port (0x60). The mouse sends 3-byte packets on movement:
 *
 *   Byte 0: bit 0=left btn, bit 1=right btn, bit 2=middle btn,
 *           bit 3=1 (always), bit 4=x sign, bit 5=y sign,
 *           bit 6=x overflow, bit 7=y overflow
 *   Byte 1: X delta (-255..255)
 *   Byte 2: Y delta (-255..255, positive = DOWN in screen coords)
 */

#include <stdint.h>
#include "drivers/mouse/mouse.h"
#include "drivers/framebuffer/graphics.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/isr.h"

#define KB_DATA    0x60
#define KB_CMD     0x64
#define KB_STATUS  0x64

/* Mouse state. */
static volatile int32_t  mouse_x = 0;
static volatile int32_t  mouse_y = 0;
static volatile uint8_t  mouse_buttons = 0;
static volatile int      mouse_has_event = 0;

/* Packet assembly state (IRQ context). */
static uint8_t  pkt_buf[3];
static uint8_t  pkt_idx = 0;

/* Wait for the controller's input buffer to be empty (ready to write). */
static void mouse_wait_write(void) {
    int timeout = 100000;
    while ((inb(KB_STATUS) & 0x02) && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
}

/* Wait for the controller's output buffer to be full (data available). */
static void mouse_wait_read(void) {
    int timeout = 100000;
    while (!(inb(KB_STATUS) & 0x01) && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
}

/* Send a command to the PS/2 mouse (aux device). */
static void mouse_write(uint8_t cmd) {
    mouse_wait_write();
    outb(KB_CMD, 0xD4);      /* tell 8042: next byte goes to the aux device */
    mouse_wait_write();
    outb(KB_DATA, cmd);
}

/* Read a byte from the mouse. */
static uint8_t mouse_read(void) {
    mouse_wait_read();
    return inb(KB_DATA);
}

static void mouse_handler(struct registers *regs) {
    (void)regs;
    uint8_t data = inb(KB_DATA);

    /* Assemble the 3-byte packet. The first byte always has bit 3 set. */
    if (pkt_idx == 0 && !(data & 0x08)) {
        return;   /* desynchronised — wait for a valid first byte */
    }

    pkt_buf[pkt_idx++] = data;
    if (pkt_idx < 3) {
        return;
    }
    pkt_idx = 0;

    /* Full packet received — decode movement. */
    uint8_t flags = pkt_buf[0];
    int32_t dx = (int32_t)(int8_t)pkt_buf[1];
    int32_t dy = (int32_t)(int8_t)pkt_buf[2];

    /* Apply sign bits from flags. */
    if (flags & 0x10) dx |= 0xFFFFFF00;   /* negative X */
    if (flags & 0x20) dy |= 0xFFFFFF00;   /* negative Y */

    /* Update buttons. */
    uint8_t new_buttons = flags & 0x07;
    if (new_buttons != mouse_buttons) {
        mouse_has_event = 1;
    }
    mouse_buttons = new_buttons;

    /* Update cursor position (Y is inverted: positive delta = move down). */
    mouse_x += dx;
    mouse_y -= dy;   /* screen Y grows downward, mouse Y grows upward */

    /* Clamp to screen bounds. */
    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();
    if (w == 0 || h == 0) return;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int32_t)w) mouse_x = (int32_t)w - 1;
    if (mouse_y >= (int32_t)h) mouse_y = (int32_t)h - 1;

    if (dx != 0 || dy != 0) {
        mouse_has_event = 1;
    }
}

void mouse_init(void) {
    uint32_t fb_w = gfx_get_width();
    uint32_t fb_h = gfx_get_height();
    mouse_x = (int32_t)(fb_w / 2);
    mouse_y = (int32_t)(fb_h / 2);

    /* Disable both PS/2 ports during init. */
    mouse_wait_write();
    outb(KB_CMD, 0xAD);   /* disable keyboard */
    mouse_wait_write();
    outb(KB_CMD, 0xA7);   /* disable mouse (aux) */

    /* Read the controller configuration byte. */
    mouse_wait_write();
    outb(KB_CMD, 0x20);   /* read config */
    uint8_t config = mouse_read();

    /* Enable aux interrupt (bit 1) and aux clock (bit 5). */
    config |= (1u << 1);
    config |= (1u << 5);

    mouse_wait_write();
    outb(KB_CMD, 0x60);   /* write config */
    mouse_wait_write();
    outb(KB_DATA, config);

    /* Perform the mouse setup sequence. */
    mouse_write(0xFF);    /* reset */
    mouse_read();         /* ACK (0xFA) */
    mouse_read();         /* 0xAA (self-test passed) */
    mouse_read();         /* 0x00 (mouse ID) */

    mouse_write(0xF6);    /* set defaults */
    mouse_read();         /* ACK */

    /* Set sample rate to 60 (via the sample-rate command 0xF3). */
    mouse_write(0xF3);
    mouse_read();
    mouse_write(60);
    mouse_read();

    mouse_write(0xF4);    /* enable data reporting */
    mouse_read();         /* ACK */

    /* Re-enable both ports. */
    mouse_wait_write();
    outb(KB_CMD, 0xA8);   /* enable mouse (aux) */
    mouse_wait_write();
    outb(KB_CMD, 0xAE);   /* enable keyboard */

    /* Register the IRQ 12 handler. */
    irq_register_handler(12, mouse_handler);
}

int mouse_get_position(int *out_x, int *out_y) {
    if (out_x) *out_x = mouse_x;
    if (out_y) *out_y = mouse_y;
    return 1;
}

uint8_t mouse_get_buttons(void) {
    return mouse_buttons;
}

int mouse_poll_event(void) {
    if (mouse_has_event) {
        mouse_has_event = 0;
        return 1;
    }
    return 0;
}
