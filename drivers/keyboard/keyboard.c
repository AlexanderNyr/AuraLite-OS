/* keyboard.c — PS/2 + USB keyboard driver with rich event queue for the GUI.
 *
 * Maintains two queues:
 *   1. kb_buffer[]  — printable ASCII characters (legacy, used by the shell).
 *   2. kb_events[]  — kb_event_t structs for the WM/GUI (special keys,
 *                     modifiers, press/release).
 *
 * PS/2 support targets scan-code set 1, including E0 extended keys,
 * PrintScreen's synthetic E0 2A/E0 37 sequence and Pause's E1 sequence.  USB
 * injection covers the HID keyboard/keypad usage page used by boot keyboards.
 */

#include <stdint.h>
#include "drivers/keyboard/keyboard.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/isr.h"

#define KB_DATA   0x60
#define KB_STATUS 0x64

#define SC_E0_ENCODE(sc)  ((uint16_t)(0xE000u | (uint16_t)(sc)))
#define SC_E1_ENCODE(sc)  ((uint16_t)(0xE100u | (uint16_t)(sc)))
#define SC_USB_KEY(u)     ((uint16_t)(0xF100u | (uint16_t)(u)))
#define SC_USB_MOD(m)     ((uint16_t)(0xF000u | (uint16_t)(m)))

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

/* Modifier/lock state. */
static volatile uint8_t mods = 0;

/* Prefix/sequence state. */
static int kb_extended = 0;
static int kb_pause_bytes_left = 0;
static int kb_printscreen_prefix = 0;

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

static uint8_t mod_bit_for_key(uint32_t key) {
    switch (key) {
        case KB_KEY_LSHIFT:
        case KB_KEY_RSHIFT: return KB_MOD_SHIFT;
        case KB_KEY_CTRL:
        case KB_KEY_RCTRL:  return KB_MOD_CTRL;
        case KB_KEY_ALT:
        case KB_KEY_RALT:   return KB_MOD_ALT;
        case KB_KEY_LGUI:
        case KB_KEY_RGUI:   return KB_MOD_META;
        default:            return 0;
    }
}

static void set_mod_key(uint32_t key, int pressed) {
    uint8_t bit = mod_bit_for_key(key);
    if (!bit) return;
    if (pressed) mods |= bit;
    else         mods &= (uint8_t)~bit;
}

static void toggle_lock_key(uint32_t key) {
    switch (key) {
        case KB_KEY_CAPSLOCK:   mods ^= KB_MOD_CAPS;   break;
        case KB_KEY_NUMLOCK:    mods ^= KB_MOD_NUM;    break;
        case KB_KEY_SCROLLLOCK: mods ^= KB_MOD_SCROLL; break;
        default: break;
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

static uint32_t fn_key(uint8_t sc) {
    if (sc >= 0x3B && sc <= 0x44) return KB_KEY_F1 + (sc - 0x3B);
    if (sc == 0x57) return KB_KEY_F11;
    if (sc == 0x58) return KB_KEY_F12;
    return 0;
}

static uint32_t normal_modifier_or_lock(uint8_t sc) {
    switch (sc) {
        case 0x2A: return KB_KEY_LSHIFT;
        case 0x36: return KB_KEY_RSHIFT;
        case 0x1D: return KB_KEY_CTRL;
        case 0x38: return KB_KEY_ALT;
        case 0x3A: return KB_KEY_CAPSLOCK;
        case 0x45: return KB_KEY_NUMLOCK;
        case 0x46: return KB_KEY_SCROLLLOCK;
        default:   return 0;
    }
}

static uint32_t keypad_key(uint8_t sc) {
    switch (sc) {
        case 0x52: return KB_KEY_KP_0;
        case 0x4F: return KB_KEY_KP_1;
        case 0x50: return KB_KEY_KP_2;
        case 0x51: return KB_KEY_KP_3;
        case 0x4B: return KB_KEY_KP_4;
        case 0x4C: return KB_KEY_KP_5;
        case 0x4D: return KB_KEY_KP_6;
        case 0x47: return KB_KEY_KP_7;
        case 0x48: return KB_KEY_KP_8;
        case 0x49: return KB_KEY_KP_9;
        case 0x53: return KB_KEY_KP_DECIMAL;
        case 0x37: return KB_KEY_KP_MULTIPLY;
        case 0x4A: return KB_KEY_KP_MINUS;
        case 0x4E: return KB_KEY_KP_PLUS;
        default:   return 0;
    }
}

static char keypad_ascii(uint32_t key) {
    if (!(mods & KB_MOD_NUM)) {
        switch (key) {
            case KB_KEY_KP_MULTIPLY: return '*';
            case KB_KEY_KP_MINUS:    return '-';
            case KB_KEY_KP_PLUS:     return '+';
            default: return 0;
        }
    }
    if (key >= KB_KEY_KP_0 && key <= KB_KEY_KP_9) return (char)('0' + (key - KB_KEY_KP_0));
    if (key == KB_KEY_KP_DECIMAL) return '.';
    if (key == KB_KEY_KP_MULTIPLY) return '*';
    if (key == KB_KEY_KP_MINUS) return '-';
    if (key == KB_KEY_KP_PLUS) return '+';
    return 0;
}

/* Translate an extended (E0-prefixed) scan code into a key constant or ASCII. */
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
        case 0x1C: return KB_KEY_KP_ENTER;
        case 0x35: return KB_KEY_KP_DIVIDE;
        case 0x1D: return KB_KEY_RCTRL;
        case 0x38: return KB_KEY_RALT;
        case 0x5B: return KB_KEY_LGUI;
        case 0x5C: return KB_KEY_RGUI;
        case 0x5D: return KB_KEY_MENU;
        case 0x37: return KB_KEY_PRINTSCR;
        case 0x5E: return KB_KEY_POWER;
        case 0x5F: return KB_KEY_SLEEP;
        case 0x63: return KB_KEY_WAKE;
        case 0x10: return KB_KEY_MEDIA_PREV;
        case 0x19: return KB_KEY_MEDIA_NEXT;
        case 0x22: return KB_KEY_MEDIA_PLAY;
        case 0x24: return KB_KEY_MEDIA_STOP;
        case 0x20: return KB_KEY_VOLUME_MUTE;
        case 0x2E: return KB_KEY_VOLUME_DOWN;
        case 0x30: return KB_KEY_VOLUME_UP;
        case 0x32: return KB_KEY_BROWSER_HOME;
        case 0x65: return KB_KEY_BROWSER_SEARCH;
        case 0x66: return KB_KEY_BROWSER_FAVORITES;
        case 0x67: return KB_KEY_BROWSER_REFRESH;
        case 0x68: return KB_KEY_BROWSER_STOP;
        case 0x69: return KB_KEY_BROWSER_FORWARD;
        case 0x6A: return KB_KEY_BROWSER_BACK;
        case 0x6B: return KB_KEY_MY_COMPUTER;
        case 0x6C: return KB_KEY_MAIL;
        case 0x6D: return KB_KEY_MEDIA_SELECT;
        case 0x21: return KB_KEY_CALCULATOR;
        default:   return 0;
    }
}

static uint32_t keypad_nav_for_numlock_off(uint32_t key) {
    switch (key) {
        case KB_KEY_KP_0:       return KB_KEY_INSERT;
        case KB_KEY_KP_1:       return KB_KEY_END;
        case KB_KEY_KP_2:       return KB_KEY_DOWN;
        case KB_KEY_KP_3:       return KB_KEY_PGDN;
        case KB_KEY_KP_4:       return KB_KEY_LEFT;
        case KB_KEY_KP_6:       return KB_KEY_RIGHT;
        case KB_KEY_KP_7:       return KB_KEY_HOME;
        case KB_KEY_KP_8:       return KB_KEY_UP;
        case KB_KEY_KP_9:       return KB_KEY_PGUP;
        case KB_KEY_KP_DECIMAL: return KB_KEY_DELETE;
        default:                return key;
    }
}

static void handle_key_event(uint32_t key, uint16_t encoded_sc, uint8_t pressed, int ascii_ok) {
    if (key == 0) return;

    if (key == KB_KEY_CAPSLOCK || key == KB_KEY_NUMLOCK || key == KB_KEY_SCROLLLOCK) {
        if (pressed) toggle_lock_key(key);
        evt_enqueue(key, encoded_sc, pressed);
        return;
    }

    uint8_t mod_bit = mod_bit_for_key(key);
    if (mod_bit) {
        set_mod_key(key, pressed);
        evt_enqueue(key, encoded_sc, pressed);
        return;
    }

    if (pressed && ascii_ok) {
        if (key == KB_KEY_KP_ENTER) kb_enqueue('\n');
        else if (key == KB_KEY_KP_DIVIDE) kb_enqueue('/');
        else if (key == KB_KEY_KP_EQUAL) kb_enqueue('=');
        else if (key < 0x100 && key != 0) kb_enqueue((char)key);
    }
    evt_enqueue(key, encoded_sc, pressed);
}

static void keyboard_handler(struct registers *regs) {
    (void)regs;
    uint8_t raw = inb(KB_DATA);

    /* Pause/Break: E1 1D 45 E1 9D C5 in set 1.  Treat as a press-only key. */
    if (kb_pause_bytes_left > 0) {
        kb_pause_bytes_left--;
        return;
    }
    if (raw == 0xE1) {
        evt_enqueue(KB_KEY_PAUSE, SC_E1_ENCODE(raw), 1);
        kb_pause_bytes_left = 5;
        kb_extended = 0;
        kb_printscreen_prefix = 0;
        return;
    }

    if (raw == 0xE0) {
        kb_extended = 1;
        return;
    }

    uint8_t pressed = (raw & 0x80) ? 0 : 1;
    uint8_t sc = raw & 0x7F;

    if (kb_extended) {
        kb_extended = 0;

        /* PrintScreen sends E0 2A E0 37 on press and E0 B7 E0 AA on release.
         * Swallow the fake Shift bytes so users see one PrintScreen event. */
        if (sc == 0x2A) {
            if (pressed) kb_printscreen_prefix = 1;
            return;
        }
        if (sc == 0x37) {
            handle_key_event(KB_KEY_PRINTSCR, SC_E0_ENCODE(raw), pressed, 0);
            kb_printscreen_prefix = 0;
            return;
        }
        if (sc == 0x54) { /* SysRq on some controllers. */
            handle_key_event(KB_KEY_SYSRQ, SC_E0_ENCODE(raw), pressed, 0);
            kb_printscreen_prefix = 0;
            return;
        }
        if (sc == 0x2A && !pressed) { kb_printscreen_prefix = 0; return; }
        if (sc == 0xAA) { kb_printscreen_prefix = 0; return; }
        (void)kb_printscreen_prefix;

        uint32_t key = ext_key(sc);
        handle_key_event(key, SC_E0_ENCODE(raw), pressed, 1);
        return;
    }

    uint32_t key = normal_modifier_or_lock(sc);
    if (key) {
        handle_key_event(key, raw, pressed, 0);
        return;
    }

    key = fn_key(sc);
    if (key) {
        handle_key_event(key, raw, pressed, 0);
        return;
    }

    key = keypad_key(sc);
    if (key) {
        uint32_t event_key = (mods & KB_MOD_NUM) ? key : keypad_nav_for_numlock_off(key);
        char c = keypad_ascii(key);
        if (pressed && c) kb_enqueue(c);
        evt_enqueue(event_key, raw, pressed);
        return;
    }

    char c = ascii_for_scancode(sc);
    if (c) {
        handle_key_event((uint32_t)(uint8_t)c, raw, pressed, 1);
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
    kb_printscreen_prefix = 0;
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

/* ---- Non-PS/2 input injection (USB HID boot keyboard) ---- */

static void apply_usb_mods(uint8_t usb_mods) {
    uint8_t keep_locks = mods & (KB_MOD_CAPS | KB_MOD_NUM | KB_MOD_SCROLL);
    uint8_t new_mods = keep_locks;
    if (usb_mods & (0x02 | 0x20)) new_mods |= KB_MOD_SHIFT;
    if (usb_mods & (0x01 | 0x10)) new_mods |= KB_MOD_CTRL;
    if (usb_mods & (0x04 | 0x40)) new_mods |= KB_MOD_ALT;
    if (usb_mods & (0x08 | 0x80)) new_mods |= KB_MOD_META;
    mods = new_mods;
}

static uint32_t usb_modifier_key(uint8_t usb_mod_bit) {
    switch (usb_mod_bit) {
        case 0x01: return KB_KEY_CTRL;
        case 0x02: return KB_KEY_LSHIFT;
        case 0x04: return KB_KEY_ALT;
        case 0x08: return KB_KEY_LGUI;
        case 0x10: return KB_KEY_RCTRL;
        case 0x20: return KB_KEY_RSHIFT;
        case 0x40: return KB_KEY_RALT;
        case 0x80: return KB_KEY_RGUI;
        default: return 0;
    }
}

static uint32_t usb_usage_special(uint8_t u) {
    if (u >= 0x3A && u <= 0x45) return KB_KEY_F1 + (u - 0x3A);
    if (u >= 0x68 && u <= 0x73) return KB_KEY_F13 + (u - 0x68);
    switch (u) {
        case 0x28: return KB_KEY_ENTER;
        case 0x29: return KB_KEY_ESC;
        case 0x2A: return KB_KEY_BACKSPACE;
        case 0x2B: return KB_KEY_TAB;
        case 0x32: return KB_KEY_NONUS_BACKSLASH; /* US: non-US #/~ */
        case 0x39: return KB_KEY_CAPSLOCK;
        case 0x46: return KB_KEY_PRINTSCR;
        case 0x47: return KB_KEY_SCROLLLOCK;
        case 0x48: return KB_KEY_PAUSE;
        case 0x49: return KB_KEY_INSERT;
        case 0x4A: return KB_KEY_HOME;
        case 0x4B: return KB_KEY_PGUP;
        case 0x4C: return KB_KEY_DELETE;
        case 0x4D: return KB_KEY_END;
        case 0x4E: return KB_KEY_PGDN;
        case 0x4F: return KB_KEY_RIGHT;
        case 0x50: return KB_KEY_LEFT;
        case 0x51: return KB_KEY_DOWN;
        case 0x52: return KB_KEY_UP;
        case 0x53: return KB_KEY_NUMLOCK;
        case 0x54: return KB_KEY_KP_DIVIDE;
        case 0x55: return KB_KEY_KP_MULTIPLY;
        case 0x56: return KB_KEY_KP_MINUS;
        case 0x57: return KB_KEY_KP_PLUS;
        case 0x58: return KB_KEY_KP_ENTER;
        case 0x59: return KB_KEY_KP_1;
        case 0x5A: return KB_KEY_KP_2;
        case 0x5B: return KB_KEY_KP_3;
        case 0x5C: return KB_KEY_KP_4;
        case 0x5D: return KB_KEY_KP_5;
        case 0x5E: return KB_KEY_KP_6;
        case 0x5F: return KB_KEY_KP_7;
        case 0x60: return KB_KEY_KP_8;
        case 0x61: return KB_KEY_KP_9;
        case 0x62: return KB_KEY_KP_0;
        case 0x63: return KB_KEY_KP_DECIMAL;
        case 0x64: return KB_KEY_NONUS_BACKSLASH;
        case 0x65: return KB_KEY_MENU;
        case 0x66: return KB_KEY_POWER;
        case 0x67: return KB_KEY_KP_EQUAL;
        case 0x74: return KB_KEY_EXECUTE;
        case 0x75: return KB_KEY_HELP;
        case 0x77: return KB_KEY_SELECT;
        case 0x79: return KB_KEY_AGAIN;
        case 0x7A: return KB_KEY_UNDO;
        case 0x7B: return KB_KEY_CUT;
        case 0x7C: return KB_KEY_COPY;
        case 0x7D: return KB_KEY_PASTE;
        case 0x7E: return KB_KEY_FIND;
        case 0x7F: return KB_KEY_VOLUME_MUTE;
        case 0x80: return KB_KEY_VOLUME_UP;
        case 0x81: return KB_KEY_VOLUME_DOWN;
        default: return 0;
    }
}

static char usb_usage_ascii(uint8_t u) {
    static const char num_lo[] = "1234567890";
    static const char num_hi[] = "!@#$%^&*()";
    int shift = (mods & KB_MOD_SHIFT) ? 1 : 0;

    if (u >= 0x04 && u <= 0x1D) {
        char c = (char)('a' + (u - 0x04));
        int upper = shift ? 1 : 0;
        if (mods & KB_MOD_CAPS) upper ^= 1;
        if (upper) c = (char)(c - 'a' + 'A');
        return c;
    }
    if (u >= 0x1E && u <= 0x27) {
        int idx = (u == 0x27) ? 9 : (int)(u - 0x1E);
        return shift ? num_hi[idx] : num_lo[idx];
    }

    switch (u) {
        case 0x28: return '\n';
        case 0x2A: return '\b';
        case 0x2B: return '\t';
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x32: return shift ? '~' : '#';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x54: return '/';
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x58: return '\n';
        case 0x59: return (mods & KB_MOD_NUM) ? '1' : 0;
        case 0x5A: return (mods & KB_MOD_NUM) ? '2' : 0;
        case 0x5B: return (mods & KB_MOD_NUM) ? '3' : 0;
        case 0x5C: return (mods & KB_MOD_NUM) ? '4' : 0;
        case 0x5D: return (mods & KB_MOD_NUM) ? '5' : 0;
        case 0x5E: return (mods & KB_MOD_NUM) ? '6' : 0;
        case 0x5F: return (mods & KB_MOD_NUM) ? '7' : 0;
        case 0x60: return (mods & KB_MOD_NUM) ? '8' : 0;
        case 0x61: return (mods & KB_MOD_NUM) ? '9' : 0;
        case 0x62: return (mods & KB_MOD_NUM) ? '0' : 0;
        case 0x63: return (mods & KB_MOD_NUM) ? '.' : 0;
        case 0x64: return shift ? '|' : '\\';
        case 0x67: return '=';
        default: return 0;
    }
}

void keyboard_inject_usb_modifier(uint8_t usb_mod_bit, uint8_t pressed,
                                  uint8_t usb_mods) {
    apply_usb_mods(usb_mods);
    uint32_t key = usb_modifier_key(usb_mod_bit);
    if (key) evt_enqueue(key, SC_USB_MOD(usb_mod_bit), pressed ? 1 : 0);
}

void keyboard_inject_usb_key(uint8_t usage, uint8_t pressed, uint8_t usb_mods) {
    apply_usb_mods(usb_mods);

    uint32_t key = usb_usage_special(usage);
    if (key == KB_KEY_CAPSLOCK || key == KB_KEY_NUMLOCK || key == KB_KEY_SCROLLLOCK) {
        if (pressed) toggle_lock_key(key);
        evt_enqueue(key, SC_USB_KEY(usage), pressed ? 1 : 0);
        return;
    }

    char c = usb_usage_ascii(usage);
    if (c) {
        if (pressed) kb_enqueue(c);
        evt_enqueue((uint32_t)(uint8_t)c, SC_USB_KEY(usage), pressed ? 1 : 0);
        return;
    }

    if (key) {
        if (pressed) {
            if (key == KB_KEY_KP_ENTER) kb_enqueue('\n');
            else if (key == KB_KEY_KP_DIVIDE) kb_enqueue('/');
            else if (key == KB_KEY_KP_EQUAL) kb_enqueue('=');
        }
        evt_enqueue(key, SC_USB_KEY(usage), pressed ? 1 : 0);
    }
}
