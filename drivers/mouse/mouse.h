#ifndef AURALITE_DRIVERS_MOUSE_MOUSE_H
#define AURALITE_DRIVERS_MOUSE_MOUSE_H

#include <stdint.h>

/*
 * PS/2 mouse driver (8042 auxiliary channel, IRQ 12 → vector 44).
 *
 * Parses 3-byte relative-movement packets and maintains an absolute cursor
 * position clamped to the framebuffer bounds. The IRQ handler reads bytes
 * from port 0x60 as they arrive; every 3 bytes completes one mouse sample.
 */

/* Initialise the PS/2 mouse: enable the aux channel, set defaults, register IRQ 12. */
void mouse_init(void);

/* Get the current cursor position. Returns 0 if the mouse is unavailable. */
int mouse_get_position(int *out_x, int *out_y);

/* Get mouse button states (bit 0 = left, bit 1 = right, bit 2 = middle). */
uint8_t mouse_get_buttons(void);

/* Check if a mouse movement or click event has occurred since the last call.
 * Returns 1 if there was an event, 0 otherwise. Clears the flag. */
int mouse_poll_event(void);

#endif /* AURALITE_DRIVERS_MOUSE_MOUSE_H */
