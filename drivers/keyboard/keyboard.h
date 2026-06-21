#ifndef AURALITE_DRIVERS_KEYBOARD_KEYBOARD_H
#define AURALITE_DRIVERS_KEYBOARD_KEYBOARD_H

#include <stdint.h>

/*
 * PS/2 keyboard driver (8042 controller, IRQ 1 → vector 33).
 *
 * Uses scan-code set 1 (the default). Provides a ring buffer of decoded
 * ASCII characters that other code can poll. The IRQ handler reads the
 * scancode from port 0x60 and translates it to ASCII.
 */

/* Buffer size for the decoded-key ring (power of 2 for fast modulo). */
#define KB_BUF_SIZE 128

/* Initialise: register the IRQ 1 handler and unmask IRQ 1. */
void keyboard_init(void);

/*
 * Poll for a key. Returns the next decoded ASCII character, or -1 if the
 * buffer is empty.
 */
int keyboard_getchar(void);

#endif /* AURALITE_DRIVERS_KEYBOARD_KEYBOARD_H */
