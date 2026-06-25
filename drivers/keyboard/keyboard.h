#ifndef AURALITE_DRIVERS_KEYBOARD_KEYBOARD_H
#define AURALITE_DRIVERS_KEYBOARD_KEYBOARD_H

#include <stdint.h>

/*
 * PS/2 keyboard driver (8042 controller, IRQ 1 → vector 33).
 *
 * Scan-code set 1.  Provides:
 *   - A ring buffer of decoded printable characters (kept for the existing
 *     shell input path).
 *   - A richer ring buffer of KEY EVENTS (key + modifiers + press/release)
 *     used by the window manager and GUI applications.
 */

#define KB_BUF_SIZE 128

/* Modifier bitmask. */
#define KB_MOD_SHIFT  0x01
#define KB_MOD_CTRL   0x02
#define KB_MOD_ALT    0x04
#define KB_MOD_CAPS   0x08

/* Special key codes (kept disjoint from 0x20..0x7E printable ASCII).
 * We use values above 0xE0 so they coexist with extended-set scancodes
 * conceptually but never collide with ASCII. */
#define KB_KEY_LEFT       0x100
#define KB_KEY_RIGHT      0x101
#define KB_KEY_UP         0x102
#define KB_KEY_DOWN       0x103
#define KB_KEY_HOME       0x104
#define KB_KEY_END        0x105
#define KB_KEY_PGUP       0x106
#define KB_KEY_PGDN       0x107
#define KB_KEY_INSERT     0x108
#define KB_KEY_DELETE     0x109
#define KB_KEY_F1         0x110
#define KB_KEY_F2         0x111
#define KB_KEY_F3         0x112
#define KB_KEY_F4         0x113
#define KB_KEY_F5         0x114
#define KB_KEY_F6         0x115
#define KB_KEY_F7         0x116
#define KB_KEY_F8         0x117
#define KB_KEY_F9         0x118
#define KB_KEY_F10        0x119
#define KB_KEY_F11        0x11A
#define KB_KEY_F12        0x11B
#define KB_KEY_PRINTSCR   0x11C
#define KB_KEY_PAUSE      0x11D
#define KB_KEY_LSHIFT     0x120
#define KB_KEY_RSHIFT     0x121
#define KB_KEY_CTRL       0x122
#define KB_KEY_ALT        0x123
#define KB_KEY_CAPSLOCK   0x124
#define KB_KEY_ESC        0x1B   /* matches ASCII */

/* A key event delivered to GUI clients. */
typedef struct {
    uint32_t key;       /* ASCII printable OR one of KB_KEY_* */
    uint16_t scancode;  /* raw scan code (set 1; E0/E1 encoded in high bits) */
    uint8_t  mods;      /* KB_MOD_* bitmask at time of event */
    uint8_t  pressed;   /* 1 = key down, 0 = key release */
} kb_event_t;

void keyboard_init(void);

/* Legacy printable-ASCII poll (used by the shell). */
int  keyboard_getchar(void);

/* Rich event queue: returns 1 + fills `out` if an event is available,
 * 0 if none.  Non-destructive peek not provided. */
int  keyboard_get_event(kb_event_t *out);

/* Current modifier state (snapshot). */
uint8_t keyboard_get_mods(void);

/* Diagnostics: number of dropped entries because the corresponding ring was
 * full.  Useful when tuning GUI/input polling frequency. */
uint32_t keyboard_get_ascii_drops(void);
uint32_t keyboard_get_event_drops(void);

/* Injection API used by USB HID and future non-PS/2 input backends.  `usb_mods`
 * is the USB boot-keyboard modifier byte. */
void keyboard_inject_usb_modifier(uint8_t usb_mod_bit, uint8_t pressed,
                                  uint8_t usb_mods);
void keyboard_inject_usb_key(uint8_t usage, uint8_t pressed, uint8_t usb_mods);

#endif /* AURALITE_DRIVERS_KEYBOARD_KEYBOARD_H */
