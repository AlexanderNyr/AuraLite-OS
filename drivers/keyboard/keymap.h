#ifndef AURALITE_DRIVERS_KEYBOARD_KEYMAP_H
#define AURALITE_DRIVERS_KEYBOARD_KEYMAP_H

#include <stdint.h>

/*
 * keymap.h — selectable keyboard layouts (FIXES_PLAN.md R8).
 *
 * A struct keymap translates one scan-code-set-1 code (0x00..0x7F) into the
 * byte the console should receive on each layer:
 *   lo    — unmodified key,
 *   hi    — Shift held,
 *   altgr — AltGr (right Alt) held; a 0 entry falls back to lo/hi.
 *
 * FIXES_PLAN.md sketched the struct as { lo, hi, name }; the altgr table is
 * appended because the same phase also demands AltGr support, which has
 * nowhere to live otherwise (a German user's {[]}|~ lives behind AltGr).
 *
 * Bytes are the console's single-byte encoding (CP437), matching the VGA
 * font: printable ASCII (0x20..0x7E) always works; extended bytes (umlauts,
 * ß, §, °, µ) reach /dev/tty0 readers and the GUI event queue, but the
 * legacy fd-0 line reader in syscall.c only accepts ASCII and drops them.
 */

struct keymap {
    const char lo[128];     /* unshifted */
    const char hi[128];     /* Shift held */
    const char altgr[128];  /* AltGr (right Alt) held; 0 = no AltGr mapping */
    const char *name;       /* short id ("us", "de") for keyboard_set_layout */
};

/* The layouts the kernel ships. */
extern const struct keymap keymap_us;
extern const struct keymap keymap_de;

/* Every layout keyboard_set_layout() accepts; NULL-terminated. */
extern const struct keymap *const keymap_registry[];

/* Look up a layout by name; NULL if unknown. */
const struct keymap *keymap_find(const char *name);

/*
 * Decode one scan code under @km with the KB_MOD_* modifier mask from
 * keyboard.h (SHIFT / CAPS / ALTGR).  An AltGr entry wins over both the
 * Shift layer and CapsLock and is used verbatim; with no AltGr entry the
 * lo/hi layer is used with the historical CapsLock rule (affects ASCII
 * letters only, XORs with Shift).  Returns 0 when the key has no printable
 * character on the active layer.
 *
 * Pure function: the host-side unit test (tests/unit/test_keymap.c) links
 * keymap.c directly and asserts the table contents through this.
 */
char keymap_lookup(const struct keymap *km, uint8_t sc, uint8_t mods);

#endif /* AURALITE_DRIVERS_KEYBOARD_KEYMAP_H */
