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
#define KB_MOD_SHIFT   0x01
#define KB_MOD_CTRL    0x02
#define KB_MOD_ALT     0x04
#define KB_MOD_CAPS    0x08
#define KB_MOD_META    0x10   /* Windows/Super/Command */
#define KB_MOD_NUM     0x20
#define KB_MOD_SCROLL  0x40

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
#define KB_KEY_CTRL       0x122   /* left Ctrl / generic Ctrl */
#define KB_KEY_ALT        0x123   /* left Alt / generic Alt */
#define KB_KEY_CAPSLOCK   0x124
#define KB_KEY_RCTRL      0x125
#define KB_KEY_RALT       0x126   /* AltGr on many layouts */
#define KB_KEY_LGUI       0x127   /* Windows/Super/Command */
#define KB_KEY_RGUI       0x128
#define KB_KEY_MENU       0x129   /* Application/context-menu key */
#define KB_KEY_NUMLOCK    0x12A
#define KB_KEY_SCROLLLOCK 0x12B
#define KB_KEY_ESC        0x1B    /* matches ASCII */
#define KB_KEY_ENTER      '\n'    /* matches ASCII */
#define KB_KEY_BACKSPACE  '\b'    /* matches ASCII */
#define KB_KEY_TAB        '\t'    /* matches ASCII */
#define KB_KEY_SPACE      ' '     /* matches ASCII */

/* Backward-compatible left-side aliases. */
#define KB_KEY_LCTRL      KB_KEY_CTRL
#define KB_KEY_LALT       KB_KEY_ALT

/* Keypad block.  Operators are also mirrored to the legacy ASCII queue where
 * possible; numeric keypad digits emit ASCII only while NumLock is active. */
#define KB_KEY_KP_0        0x130
#define KB_KEY_KP_1        0x131
#define KB_KEY_KP_2        0x132
#define KB_KEY_KP_3        0x133
#define KB_KEY_KP_4        0x134
#define KB_KEY_KP_5        0x135
#define KB_KEY_KP_6        0x136
#define KB_KEY_KP_7        0x137
#define KB_KEY_KP_8        0x138
#define KB_KEY_KP_9        0x139
#define KB_KEY_KP_DECIMAL  0x13A
#define KB_KEY_KP_DIVIDE   0x13B
#define KB_KEY_KP_MULTIPLY 0x13C
#define KB_KEY_KP_MINUS    0x13D
#define KB_KEY_KP_PLUS     0x13E
#define KB_KEY_KP_ENTER    0x13F
#define KB_KEY_KP_EQUAL    0x140

/* Extra function keys and common 104-key / ACPI / multimedia keys. */
#define KB_KEY_F13        0x150
#define KB_KEY_F14        0x151
#define KB_KEY_F15        0x152
#define KB_KEY_F16        0x153
#define KB_KEY_F17        0x154
#define KB_KEY_F18        0x155
#define KB_KEY_F19        0x156
#define KB_KEY_F20        0x157
#define KB_KEY_F21        0x158
#define KB_KEY_F22        0x159
#define KB_KEY_F23        0x15A
#define KB_KEY_F24        0x15B
#define KB_KEY_SYSRQ      0x15C
#define KB_KEY_POWER      0x15D
#define KB_KEY_SLEEP      0x15E
#define KB_KEY_WAKE       0x15F
#define KB_KEY_MEDIA_PREV 0x160
#define KB_KEY_MEDIA_NEXT 0x161
#define KB_KEY_MEDIA_PLAY 0x162
#define KB_KEY_MEDIA_STOP 0x163
#define KB_KEY_VOLUME_MUTE 0x164
#define KB_KEY_VOLUME_DOWN 0x165
#define KB_KEY_VOLUME_UP   0x166
#define KB_KEY_BROWSER_HOME 0x167
#define KB_KEY_BROWSER_SEARCH 0x168
#define KB_KEY_BROWSER_FAVORITES 0x169
#define KB_KEY_BROWSER_REFRESH 0x16A
#define KB_KEY_BROWSER_STOP 0x16B
#define KB_KEY_BROWSER_FORWARD 0x16C
#define KB_KEY_BROWSER_BACK 0x16D
#define KB_KEY_MY_COMPUTER 0x16E
#define KB_KEY_MAIL        0x16F
#define KB_KEY_MEDIA_SELECT 0x170
#define KB_KEY_CALCULATOR   0x171
#define KB_KEY_EXECUTE      0x172
#define KB_KEY_HELP         0x173
#define KB_KEY_SELECT       0x174
#define KB_KEY_AGAIN        0x175
#define KB_KEY_UNDO         0x176
#define KB_KEY_CUT          0x177
#define KB_KEY_COPY         0x178
#define KB_KEY_PASTE        0x179
#define KB_KEY_FIND         0x17A
#define KB_KEY_NONUS_BACKSLASH 0x17B

/* A key event delivered to GUI clients. */
typedef struct {
    uint32_t key;       /* ASCII printable OR one of KB_KEY_* */
    uint16_t scancode;  /* raw scan code (set 1; E0/E1/USB encoded in high bits) */
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
