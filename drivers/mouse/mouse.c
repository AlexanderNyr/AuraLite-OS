/* mouse.c — PS/2 mouse with optional IntelliMouse (4-byte/scroll) extension.
 *
 * The IntelliMouse "magic knock" sequence (set sample rate 200, then 100,
 * then 80, then read mouse ID) makes the device switch into 4-byte packet
 * mode where byte 3 is the signed scroll-wheel delta.  If the device returns
 * ID 0x03 after the knock, we use 4-byte packets; otherwise we fall back to
 * the classic 3-byte format.
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

static volatile int32_t  mouse_x = 0;
static volatile int32_t  mouse_y = 0;
static volatile uint8_t  mouse_buttons = 0;
static volatile int      mouse_has_event = 0;

static uint8_t  pkt_buf[4];
static uint8_t  pkt_idx = 0;
static int      pkt_len = 3;        /* 3 by default, 4 if IntelliMouse */

/* Event ring. */
#define MOUSE_EVT_RING 128
static mouse_event_t mouse_events[MOUSE_EVT_RING];
static volatile uint32_t evt_head = 0;
static volatile uint32_t evt_tail = 0;

static void mouse_wait_write(void) {
    int timeout = 100000;
    while ((inb(KB_STATUS) & 0x02) && timeout-- > 0) __asm__ volatile ("pause");
}
static void mouse_wait_read(void) {
    int timeout = 100000;
    while (!(inb(KB_STATUS) & 0x01) && timeout-- > 0) __asm__ volatile ("pause");
}

static void mouse_write(uint8_t cmd) {
    mouse_wait_write();
    outb(KB_CMD, 0xD4);
    mouse_wait_write();
    outb(KB_DATA, cmd);
}
static uint8_t mouse_read(void) {
    mouse_wait_read();
    return inb(KB_DATA);
}

static void evt_push(const mouse_event_t *e) {
    uint32_t next = (evt_head + 1) % MOUSE_EVT_RING;
    if (next == evt_tail) return;
    mouse_events[evt_head] = *e;
    evt_head = next;
}

static void mouse_handler(struct registers *regs) {
    (void)regs;
    uint8_t data = inb(KB_DATA);

    /* Re-sync on the first byte (bit 3 always 1). */
    if (pkt_idx == 0 && !(data & 0x08)) return;
    pkt_buf[pkt_idx++] = data;
    if (pkt_idx < pkt_len) return;
    pkt_idx = 0;

    uint8_t flags = pkt_buf[0];
    int32_t dx = (int32_t)(int8_t)pkt_buf[1];
    int32_t dy = (int32_t)(int8_t)pkt_buf[2];
    int8_t  wheel = 0;
    if (pkt_len == 4) {
        /* Byte 3: low nibble = signed wheel delta. */
        int8_t w = (int8_t)pkt_buf[3];
        /* Sign-extend a 4-bit value: 0xF -> -1 etc. */
        if (w & 0x08) w |= 0xF0;
        wheel = w;
    }

    if (flags & 0x10) dx |= 0xFFFFFF00;
    if (flags & 0x20) dy |= 0xFFFFFF00;

    uint8_t old_btn = mouse_buttons;
    uint8_t new_btn = flags & 0x07;
    mouse_buttons = new_btn;

    mouse_x += dx;
    mouse_y -= dy;
    uint32_t w = gfx_get_width();
    uint32_t h = gfx_get_height();
    if (w && h) {
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= (int32_t)w) mouse_x = (int32_t)w - 1;
        if (mouse_y >= (int32_t)h) mouse_y = (int32_t)h - 1;
    }

    if (dx || dy || wheel || (old_btn != new_btn)) {
        mouse_event_t e = {0};
        e.dx = (int16_t)dx;
        e.dy = (int16_t)(-dy);   /* screen Y down */
        e.abs_x = (int16_t)mouse_x;
        e.abs_y = (int16_t)mouse_y;
        e.wheel = wheel;
        e.buttons = new_btn;
        e.pressed  = (uint8_t)((~old_btn) & new_btn);
        e.released = (uint8_t)(old_btn & (~new_btn));
        evt_push(&e);
        mouse_has_event = 1;
    }
}

void mouse_init(void) {
    uint32_t fb_w = gfx_get_width();
    uint32_t fb_h = gfx_get_height();
    mouse_x = (int32_t)(fb_w / 2);
    mouse_y = (int32_t)(fb_h / 2);

    mouse_wait_write(); outb(KB_CMD, 0xAD);
    mouse_wait_write(); outb(KB_CMD, 0xA7);

    mouse_wait_write(); outb(KB_CMD, 0x20);
    uint8_t config = mouse_read();
    config |= (1u << 1);
    config |= (1u << 5);
    mouse_wait_write(); outb(KB_CMD, 0x60);
    mouse_wait_write(); outb(KB_DATA, config);

    mouse_write(0xFF); mouse_read(); mouse_read(); mouse_read();
    mouse_write(0xF6); mouse_read();

    /* IntelliMouse knock: set sample rate 200, 100, 80, then read ID. */
    mouse_write(0xF3); mouse_read(); mouse_write(200); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(100); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(80);  mouse_read();
    mouse_write(0xF2); mouse_read();
    uint8_t id = mouse_read();
    if (id == 0x03) {
        pkt_len = 4;   /* scroll wheel supported */
    }

    mouse_write(0xF3); mouse_read(); mouse_write(60); mouse_read();
    mouse_write(0xF4); mouse_read();

    mouse_wait_write(); outb(KB_CMD, 0xA8);
    mouse_wait_write(); outb(KB_CMD, 0xAE);

    irq_register_handler(12, mouse_handler);
}

int mouse_get_position(int *out_x, int *out_y) {
    if (out_x) *out_x = mouse_x;
    if (out_y) *out_y = mouse_y;
    return 1;
}
uint8_t mouse_get_buttons(void) { return mouse_buttons; }
int mouse_poll_event(void) {
    if (mouse_has_event) { mouse_has_event = 0; return 1; }
    return 0;
}
int mouse_get_event(mouse_event_t *out) {
    if (evt_head == evt_tail) return 0;
    *out = mouse_events[evt_tail];
    evt_tail = (evt_tail + 1) % MOUSE_EVT_RING;
    return 1;
}
