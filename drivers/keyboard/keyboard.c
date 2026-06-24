/* keyboard.c — PS/2 keyboard driver with rich event queue for the GUI.
 *
 * Maintains two queues:
 *   1. kb_buffer[]  — printable ASCII characters (legacy, used by the shell).
 *   2. kb_events[]  — kb_event_t structs for the WM/GUI (special keys,
 *                     modifiers, press/release).
 *
 * Modifier handling: tracks Shift, Ctrl, Alt, CapsLock.  Extended (E0) keys
 * are mapped to KB_KEY_* constants.
 */

#include <stdint.h>
#include "drivers/keyboard/keyboard.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/isr.h"

#define KB_DATA   0x60
#define KB_STATUS 0x64

/* Printable-ASCII ring (legacy). */
static char     kb_buffer[KB_BUF_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;

/* Rich event ring (GUI). */
#define KB_EVT_RING 128
static kb_event_t kb_events[KB_EVT_RING];
static volatile uint32_t evt_head = 0;
static volatile uint32_t evt_tail = 0;

/* Modifier state. */
static volatile uint8_t mods = 0;

/* Extended-prefix state. */
static int kb_extended = 0;

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
    }
}

static void evt_enqueue(uint32_t key, uint16_t sc, uint8_t pressed) {
    uint32_t next = (evt_head + 1) % KB_EVT_RING;
    if (next == evt_tail) return; /* full — drop */
    kb_events[evt_head].key      = key;
    kb_events[evt_head].scancode = sc;
    kb_events[evt_head].mods     = mods;
    kb_events[evt_head].pressed  = pressed;
    evt_head = next;
}

/* Translate an extended (E0-prefixed) scan code into a KB_KEY_* constant.
 * Returns 0 if no mapping. */
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
        case 0x1D: /* right ctrl */ mods |= KB_MOD_CTRL; return 0;
        case 0x38: /* right alt  */ mods |= KB_MOD_ALT;  return 0;
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

static void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t sc = inb(KB_DATA);

    if (sc == 0xE0) { kb_extended = 1; return; }

    /* Release */
    if (sc & 0x80) {
        uint8_t rel = sc & 0x7F;
        if (kb_extended) {
            uint32_t k = ext_key(rel);
            /* Modifier release for right ctrl/alt. */
            if (rel == 0x1D) mods &= ~KB_MOD_CTRL;
            if (rel == 0x38) mods &= ~KB_MOD_ALT;
            if (k) evt_enqueue(k, sc, 0);
            kb_extended = 0;
            return;
        }
        switch (rel) {
            case 0x2A: case 0x36: mods &= ~KB_MOD_SHIFT; break;
            case 0x1D: mods &= ~KB_MOD_CTRL; break;
            case 0x38: mods &= ~KB_MOD_ALT;  break;
            default: {
                /* Compute key value for release event for non-modifier keys. */
                uint32_t k = fn_key(rel);
                if (!k && rel < sizeof(map_lo)) {
                    k = (mods & KB_MOD_SHIFT) ? map_hi[rel] : map_lo[rel];
                }
                if (k) evt_enqueue(k, sc, 0);
                break;
            }
        }
        return;
    }

    /* Press */
    if (kb_extended) {
        uint32_t k = ext_key(sc);
        if (k) evt_enqueue(k, sc, 1);
        kb_extended = 0;
        return;
    }

    switch (sc) {
        case 0x2A: case 0x36: mods |= KB_MOD_SHIFT; return;
        case 0x1D: mods |= KB_MOD_CTRL;            return;
        case 0x38: mods |= KB_MOD_ALT;             return;
        case 0x3A: mods ^= KB_MOD_CAPS;            return; /* CapsLock toggle */
    }

    /* F-keys. */
    uint32_t fk = fn_key(sc);
    if (fk) { evt_enqueue(fk, sc, 1); return; }

    /* Printable ASCII. */
    if (sc < sizeof(map_lo)) {
        char c = (mods & KB_MOD_SHIFT) ? map_hi[sc] : map_lo[sc];
        /* CapsLock affects only A-Z. */
        if ((mods & KB_MOD_CAPS) && c >= 'a' && c <= 'z') c -= 32;
        else if ((mods & KB_MOD_CAPS) && c >= 'A' && c <= 'Z' && (mods & KB_MOD_SHIFT)) c += 32;
        if (c) {
            kb_enqueue(c);
            evt_enqueue((uint32_t)(uint8_t)c, sc, 1);
        }
    }
}

void keyboard_init(void) {
    while (inb(KB_STATUS) & 0x01) inb(KB_DATA);
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
