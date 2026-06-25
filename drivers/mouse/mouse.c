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
#include "kernel/lib/kprintf.h"

#define KB_DATA    0x60
#define KB_CMD     0x64
#define KB_STATUS  0x64

#define PS2_ST_OUT_FULL 0x01
#define PS2_ST_IN_FULL  0x02
#define PS2_ST_AUX_DATA 0x20

#define PS2_ACK    0xFA
#define PS2_RESEND 0xFE

static volatile int32_t  mouse_x = 0;
static volatile int32_t  mouse_y = 0;
static volatile uint8_t  mouse_buttons = 0;
static volatile int      mouse_has_event = 0;
static volatile uint32_t mouse_packet_drops = 0;
static volatile uint32_t mouse_event_drops = 0;

static uint8_t  pkt_buf[4];
static uint8_t  pkt_idx = 0;
static int      pkt_len = 3;        /* 3 by default, 4 if IntelliMouse */
static int      mouse_ready = 0;

/* Event ring. */
#define MOUSE_EVT_RING 128
static mouse_event_t mouse_events[MOUSE_EVT_RING];
static volatile uint32_t evt_head = 0;
static volatile uint32_t evt_tail = 0;

static int ps2_wait_write(void) {
    int timeout = 100000;
    while ((inb(KB_STATUS) & PS2_ST_IN_FULL) && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
    return timeout > 0;
}

static int ps2_wait_read(void) {
    int timeout = 100000;
    while (!(inb(KB_STATUS) & PS2_ST_OUT_FULL) && timeout-- > 0) {
        __asm__ volatile ("pause");
    }
    return timeout > 0;
}

static int ps2_read_byte(uint8_t *out) {
    if (!ps2_wait_read()) return 0;
    if (out) *out = inb(KB_DATA);
    else (void)inb(KB_DATA);
    return 1;
}

static int ps2_write_cmd(uint8_t cmd) {
    if (!ps2_wait_write()) return 0;
    outb(KB_CMD, cmd);
    return 1;
}

static int ps2_write_data(uint8_t data) {
    if (!ps2_wait_write()) return 0;
    outb(KB_DATA, data);
    return 1;
}

static void ps2_flush_output(void) {
    int guard = 32;
    while ((inb(KB_STATUS) & PS2_ST_OUT_FULL) && guard-- > 0) {
        (void)inb(KB_DATA);
    }
}

static int mouse_write_raw(uint8_t byte) {
    if (!ps2_write_cmd(0xD4)) return 0;
    return ps2_write_data(byte);
}

static int mouse_send(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t resp = 0;
        if (!mouse_write_raw(cmd)) return 0;
        if (!ps2_read_byte(&resp)) return 0;
        if (resp == PS2_ACK) return 1;
        if (resp != PS2_RESEND) return 0;
    }
    return 0;
}

static int mouse_set_sample_rate(uint8_t rate) {
    return mouse_send(0xF3) && mouse_send(rate);
}

static void evt_push(const mouse_event_t *e) {
    uint32_t next = (evt_head + 1) % MOUSE_EVT_RING;
    if (next == evt_tail) {
        mouse_event_drops++;
        return;
    }
    mouse_events[evt_head] = *e;
    evt_head = next;
}

static void mouse_handler(struct registers *regs) {
    (void)regs;
    uint8_t status = inb(KB_STATUS);
    if (!(status & PS2_ST_OUT_FULL)) return;
    uint8_t data = inb(KB_DATA);

    /* If the controller tells us this byte did not come from the auxiliary
     * device, drop it rather than poisoning the packet stream.  Some virtual
     * controllers do not set AUX_DATA reliably, so only use the bit as a guard
     * when it is explicitly absent during packet start. */
    if (pkt_idx == 0 && !(data & 0x08)) {
        mouse_packet_drops++;
        return;
    }

    pkt_buf[pkt_idx++] = data;
    if (pkt_idx < pkt_len) return;
    pkt_idx = 0;

    uint8_t flags = pkt_buf[0];
    int32_t dx = (int32_t)(int8_t)pkt_buf[1];
    int32_t dy = (int32_t)(int8_t)pkt_buf[2];
    int8_t  wheel = 0;
    if (pkt_len == 4) {
        /* Byte 3: low nibble = signed wheel delta. */
        int8_t w = (int8_t)(pkt_buf[3] & 0x0F);
        if (w & 0x08) w |= (int8_t)0xF0;
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
    mouse_buttons = 0;
    mouse_has_event = 0;
    mouse_packet_drops = 0;
    mouse_event_drops = 0;
    evt_head = evt_tail = 0;
    pkt_idx = 0;
    pkt_len = 3;
    mouse_ready = 0;

    /* Disable ports while changing the controller config. */
    ps2_write_cmd(0xAD); /* disable first PS/2 port */
    ps2_write_cmd(0xA7); /* disable second PS/2 port */
    ps2_flush_output();

    uint8_t config = 0;
    if (ps2_write_cmd(0x20) && ps2_read_byte(&config)) {
        config |= (1u << 0);        /* enable IRQ1 (keyboard) */
        config |= (1u << 1);        /* enable IRQ12 (mouse) */
        config &= (uint8_t)~(1u << 5); /* enable second-port clock */
        ps2_write_cmd(0x60);
        ps2_write_data(config);
    } else {
        kprintf("[mouse] WARNING: could not read PS/2 controller config\n");
    }

    /* Enable aux before sending device commands; then reset/default the mouse. */
    ps2_write_cmd(0xA8);

    if (mouse_send(0xFF)) {
        uint8_t bat = 0, id = 0;
        if (ps2_read_byte(&bat) && ps2_read_byte(&id)) {
            (void)id;
            if (bat != 0xAA) {
                kprintf("[mouse] WARNING: reset BAT returned 0x%x\n", bat);
            }
        }
    } else {
        kprintf("[mouse] WARNING: reset command not acknowledged\n");
    }

    if (!mouse_send(0xF6)) {
        kprintf("[mouse] WARNING: set-defaults command not acknowledged\n");
    }

    /* IntelliMouse knock: set sample rate 200, 100, 80, then read ID. */
    if (mouse_set_sample_rate(200) && mouse_set_sample_rate(100) &&
        mouse_set_sample_rate(80) && mouse_send(0xF2)) {
        uint8_t id = 0;
        if (ps2_read_byte(&id) && id == 0x03) {
            pkt_len = 4;   /* scroll wheel supported */
            kprintf("[mouse] IntelliMouse wheel mode enabled\n");
        }
    }

    mouse_set_sample_rate(60);
    if (mouse_send(0xF4)) {
        mouse_ready = 1;
    } else {
        kprintf("[mouse] WARNING: enable-data-reporting not acknowledged\n");
    }

    /* Re-enable keyboard port too: keyboard_init() ran earlier. */
    ps2_write_cmd(0xAE);

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
int mouse_is_ready(void) { return mouse_ready; }
uint32_t mouse_get_packet_drops(void) { return mouse_packet_drops; }
uint32_t mouse_get_event_drops(void) { return mouse_event_drops; }
