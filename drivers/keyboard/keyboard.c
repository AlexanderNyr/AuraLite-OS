/* keyboard.c — PS/2 keyboard driver with rich event queue for the GUI.
 *
 * Maintains two queues:
 *   1. kb_buffer[]  — printable ASCII characters (legacy, used by the shell).
 *   2. kb_events[]  — kb_event_t structs for the WM/GUI (special keys,
 *                     modifiers, press/release).
 *
 * Modifier handling: tracks Shift, Ctrl, Alt, CapsLock.  Extended (E0) keys
 * are mapped to KB_KEY_* constants.  The driver is deliberately small but now
 * handles common E0 keys such as keypad Enter, keypad '/', right Ctrl/Alt and
 * PrintScreen, and swallows the multi-byte Pause sequence cleanly.
 */

#include <stdint.h>
#include "drivers/keyboard/keyboard.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/isr.h"

#define KB_DATA   0x60
#define KB_STATUS 0x64

#define SC_E0_ENCODE(sc) ((uint16_t)(0xE000u | (uint16_t)(sc)))
#define SC_E1_ENCODE(sc) ((uint16_t)(0xE100u | (uint16_t)(sc)))

/* Printable-ASCII ring (legacy). */
static char     kb_buffer[KB_BUF_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;
static volatile uint32_t kb_ascii_drops = 0;

/* Rich event ring (GUI). */
#define KB_EVT_RING 128
static kb_event_t kb_events[KB_EVT_RING];
static volatile uint32_t evt_head = 0;
static volatile uint32_t evt_tail = 0;
static volatile uint32_t kb_event_drops = 0;

/* Modifier state. */
static volatile uint8_t mods = 0;

/* Prefix/sequence state. */
static int kb_extended = 0;
static int kb_pause_bytes_left = 0;

/* US QWERTY scan-code-set-1 → ASCII (lowercase / unshifted). */
static const char map_lo[128] = {
    0, 0x1B,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'', '`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
/* Shifted variants. */
static const char map_hi[128] = {
    0, 0x1B,'!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"', '~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

static void kb_enqueue(char c) {
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {
        kb_buffer[kb_head] = c;
        kb_head = next;
    } else {
        kb_ascii_drops++;
    }
}

static void evt_enqueue(uint32_t key, uint16_t sc, uint8_t pressed) {
    uint32_t next = (evt_head + 1) % KB_EVT_RING;
    if (next == evt_tail) {
        kb_event_drops++;
        return; /* full — drop newest so client sees older input in order */
    }
    kb_events[evt_head].key      = key;
    kb_events[evt_head].scancode = sc;
    kb_events[evt_head].mods     = mods;
    kb_events[evt_head].pressed  = pressed;
    evt_head = next;
}

/* Translate an extended (E0-prefixed) scan code into a key constant or ASCII.
 * Modifier state is handled by the caller so press/release paths stay
 * symmetric. Returns 0 if no mapping. */
static uint32_t ext_key(uint8_t sc) {
    switch (sc) {
        case 0x48: return KB_KEY_UP;
        case 0x50: return KB_KEY_DOWN;
        case 0x4B: return KB_KEY_LEFT;
        case 0x4D: return KB_KEY_RIGHT;
        case 0x47: return KB_KEY_HOME;
        case 0x4F: return KB_KEY_END;
        case 0x49: return KB_KEY_PGUP;
        case 0x51: return KB_KEY_PGDN;
        case 0x52: return KB_KEY_INSERT;
        case 0x53: return KB_KEY_DELETE;
        case 0x1C: return '\n';          /* keypad Enter */
        case 0x35: return '/';           /* keypad slash */
        case 0x37: return KB_KEY_PRINTSCR;
        default:   return 0;
    }
}

/* Translate non-extended scan codes 0x3B..0x44 into F1..F10 and 0x57/0x58
 * into F11/F12. */
static uint32_t fn_key(uint8_t sc) {
    if (sc >= 0x3B && sc <= 0x44) return KB_KEY_F1 + (sc - 0x3B);
    if (sc == 0x57) return KB_KEY_F11;
    if (sc == 0x58) return KB_KEY_F12;
    return 0;
}

static uint32_t modifier_key(uint8_t sc) {
    switch (sc) {
        case 0x2A: return KB_KEY_LSHIFT;
        case 0x36: return KB_KEY_RSHIFT;
        case 0x1D: return KB_KEY_CTRL;
        case 0x38: return KB_KEY_ALT;
        case 0x3A: return KB_KEY_CAPSLOCK;
        default:   return 0;
    }
}

static void set_modifier(uint8_t sc, int extended, int pressed) {
    (void)extended;
    switch (sc) {
        case 0x2A: case 0x36:
            if (pressed) mods |= KB_MOD_SHIFT; else mods &= (uint8_t)~KB_MOD_SHIFT;
            break;
        case 0x1D:
            if (pressed) mods |= KB_MOD_CTRL;  else mods &= (uint8_t)~KB_MOD_CTRL;
            break;
        case 0x38:
            if (pressed) mods |= KB_MOD_ALT;   else mods &= (uint8_t)~KB_MOD_ALT;
            break;
        case 0x3A:
            if (pressed) mods ^= KB_MOD_CAPS;  /* lock toggles on press only */
            break;
    }
}

static char ascii_for_scancode(uint8_t sc) {
    if (sc >= sizeof(map_lo)) return 0;
    char c = (mods & KB_MOD_SHIFT) ? map_hi[sc] : map_lo[sc];
    /* CapsLock affects letters only and XORs with Shift. */
    if (c >= 'a' && c <= 'z' && (mods & KB_MOD_CAPS)) c -= 32;
    else if (c >= 'A' && c <= 'Z' && (mods & KB_MOD_CAPS) && (mods & KB_MOD_SHIFT)) c += 32;
    return c;
}

static void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t sc = inb(KB_DATA);

    /* Pause/Break is the awkward E1 1D 45 E1 9D C5 sequence in set 1.  Treat
     * it as a press-only special key and consume the remaining bytes. */
    if (kb_pause_bytes_left > 0) {
        kb_pause_bytes_left--;
        return;
    }
    if (sc == 0xE1) {
        evt_enqueue(KB_KEY_PAUSE, SC_E1_ENCODE(sc), 1);
        kb_pause_bytes_left = 5;
        kb_extended = 0;
        return;
    }

    if (sc == 0xE0) { kb_extended = 1; return; }

    /* Release */
    if (sc & 0x80) {
        uint8_t rel = sc & 0x7F;
        if (kb_extended) {
            if (rel == 0x1D || rel == 0x38) {
                uint32_t mk = (rel == 0x1D) ? KB_KEY_CTRL : KB_KEY_ALT;
                set_modifier(rel, 1, 0);
                evt_enqueue(mk, SC_E0_ENCODE(sc), 0);
            } else {
                uint32_t k = ext_key(rel);
                if (k) evt_enqueue(k, SC_E0_ENCODE(sc), 0);
            }
            kb_extended = 0;
            return;
        }

        uint32_t mk = modifier_key(rel);
        if (mk && rel != 0x3A) { /* CapsLock has no meaningful release state. */
            set_modifier(rel, 0, 0);
            evt_enqueue(mk, sc, 0);
            return;
        }

        /* Compute key value for release event for non-modifier keys. */
        uint32_t k = fn_key(rel);
        if (!k) k = (uint32_t)(uint8_t)ascii_for_scancode(rel);
        if (k) evt_enqueue(k, sc, 0);
        return;
    }

    /* Press */
    if (kb_extended) {
        if (sc == 0x1D || sc == 0x38) {
            uint32_t mk = (sc == 0x1D) ? KB_KEY_CTRL : KB_KEY_ALT;
            set_modifier(sc, 1, 1);
            evt_enqueue(mk, SC_E0_ENCODE(sc), 1);
        } else {
            uint32_t k = ext_key(sc);
            if (k) {
                if (k == '\n' || k == '/') kb_enqueue((char)k);
                evt_enqueue(k, SC_E0_ENCODE(sc), 1);
            }
        }
        kb_extended = 0;
        return;
    }

    uint32_t mk = modifier_key(sc);
    if (mk) {
        set_modifier(sc, 0, 1);
        evt_enqueue(mk, sc, 1);
        return;
    }

    /* F-keys. */
    uint32_t fk = fn_key(sc);
    if (fk) { evt_enqueue(fk, sc, 1); return; }

    /* Printable ASCII. */
    char c = ascii_for_scancode(sc);
    if (c) {
        kb_enqueue(c);
        evt_enqueue((uint32_t)(uint8_t)c, sc, 1);
    }
}

void keyboard_init(void) {
    while (inb(KB_STATUS) & 0x01) inb(KB_DATA);
    kb_head = kb_tail = 0;
    evt_head = evt_tail = 0;
    kb_ascii_drops = kb_event_drops = 0;
    mods = 0;
    kb_extended = 0;
    kb_pause_bytes_left = 0;
    irq_register_handler(1, keyboard_handler);
}

int keyboard_getchar(void) {
    if (kb_head == kb_tail) return -1;
    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

int keyboard_get_event(kb_event_t *out) {
    if (evt_head == evt_tail) return 0;
    *out = kb_events[evt_tail];
    evt_tail = (evt_tail + 1) % KB_EVT_RING;
    return 1;
}

uint8_t keyboard_get_mods(void) { return mods; }
uint32_t keyboard_get_ascii_drops(void) { return kb_ascii_drops; }
uint32_t keyboard_get_event_drops(void) { return kb_event_drops; }
