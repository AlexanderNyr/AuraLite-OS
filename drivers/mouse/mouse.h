#ifndef AURALITE_DRIVERS_MOUSE_MOUSE_H
#define AURALITE_DRIVERS_MOUSE_MOUSE_H

#include <stdint.h>

/*
 * PS/2 mouse driver (8042 auxiliary channel, IRQ 12).
 *
 * Tries to enable IntelliMouse (scroll wheel) 4-byte packet mode.  Falls back
 * to 3-byte packets if the device doesn't accept the magic 200/100/80 sample-
 * rate sequence.  Reports an absolute cursor position clamped to the screen
 * and a separate event queue that includes scroll-wheel ticks.
 */

#define MOUSE_BTN_LEFT    0x01
#define MOUSE_BTN_RIGHT   0x02
#define MOUSE_BTN_MIDDLE  0x04

typedef struct {
    int16_t  dx;          /* relative motion since previous event */
    int16_t  dy;
    int16_t  abs_x;       /* current absolute position */
    int16_t  abs_y;
    int8_t   wheel;       /* scroll: -ve = up, +ve = down */
    uint8_t  buttons;     /* MOUSE_BTN_* bitmask */
    uint8_t  pressed;     /* bits that changed 0->1 in this event */
    uint8_t  released;    /* bits that changed 1->0 in this event */
} mouse_event_t;

void mouse_init(void);
int  mouse_get_position(int *out_x, int *out_y);
uint8_t mouse_get_buttons(void);

/* Legacy edge poll (used by the old WM). */
int mouse_poll_event(void);

/* Rich event queue.  Returns 1 + fills *out if available. */
int mouse_get_event(mouse_event_t *out);

/* Double-click time window (in PIT ticks @ 100 Hz). */
#define MOUSE_DBLCLICK_TICKS 40

#endif /* AURALITE_DRIVERS_MOUSE_MOUSE_H */
