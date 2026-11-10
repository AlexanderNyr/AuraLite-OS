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
    const char dead[128];   /* unshifted DEAD keys (accents); 0 = not dead */
    const char *name;       /* short id ("us", "de") for keyboard_set_layout */

    /* OFFICE_PLAN OF1: the Unicode layer.
     *
     * A layout whose characters do not fit in a byte carries its three layers
     * here as codepoints; keymap_unicode() is what the GUI reads, and the byte
     * tables above stay the CONSOLE's view.  Two views, one authority: a
     * modern face with 66 Cyrillic letters cannot be expressed in CP437 at
     * all (the VGA font has no Cyrillic — that is the whole reason OF1 exists),
     * while /dev/tty0 and the shell's line reader must keep receiving exactly
     * the bytes they receive today, because every existing integration test
     * pins them.
     *
     * NULL means "derive from the byte tables": for us and de the codepoint of
     * a key is cp437_to_unicode(byte), which is a table lookup, so those
     * layouts need no second copy of their data to drift from the first.
     */
    const uint32_t *ulo;
    const uint32_t *uhi;
    const uint32_t *ualtgr;
};

/*
 * RESIDUE2 T4: dead keys (accents).  A layout marks a scancode in
 * `dead[]` with one of the KB_DEAD_* accents; pressing that key unshifted
 * (no Shift, no AltGr — Shift+´ must keep emitting ` directly, AltGr stays
 * a third-layer key) arms the pending accent instead of emitting a byte.
 * The next printable byte composes (´ + a -> CP437 0xA0 á), a control
 * byte cancels the accent, and an undefined pair emits the accent's
 * spacing form followed by the base letter so nothing is lost or
 * reordered.  The state machine lives in keyboard.c (it is driver state);
 * everything here stays pure so the host-side unit test
 * (tests/unit/test_deadkey.c) links keymap.c directly and checks the
 * same compose tables the kernel ships.
 */
#define KB_DEAD_NONE        0
#define KB_DEAD_ACUTE       1   /* ´ — the German key right of ß  (sc 0x0D) */
#define KB_DEAD_CIRCUMFLEX  2   /* ^  — the German key left of 1   (sc 0x29) */

/* The layouts the kernel ships. */
extern const struct keymap keymap_us;
extern const struct keymap keymap_de;
extern const struct keymap keymap_ru;   /* OFFICE_PLAN OF1: ЙЦУКЕН */

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

/*
 * OFFICE_PLAN OF1: the same decode, in Unicode.
 *
 * Returns the codepoint the key produces under @mods, or 0 when it produces
 * none (control keys included: a caller that wants Enter gets the byte from
 * keymap_lookup(), and the GUI keeps the key constant it always had).  This
 * is the function a modern application reads, because «П» is not a byte.
 */
uint32_t keymap_unicode(const struct keymap *km, uint8_t sc, uint8_t mods);

/* CP437 byte -> Unicode codepoint (0x00..0xFF, the VGA font's own repertoire).
 * The identity for ASCII; the table for the rest.  Bytes with no Unicode
 * meaning in CP437 (none, by definition) map to themselves. */
uint32_t cp437_to_unicode(uint8_t b);

/*
 * RESIDUE2 T4 dead keys — pure helpers, host-tested by
 * tests/unit/test_deadkey.c.
 *
 * keymap_dead_of(): the KB_DEAD_* accent this press arms, or KB_DEAD_NONE.
 * A dead key fires only on the plain unshifted layer: Shift and AltGr keep
 * their direct bytes (hi[0x0D] == '`' is pinned by test_keymap.c).
 */
uint8_t keymap_dead_of(const struct keymap *km, uint8_t sc, uint8_t mods);

/*
 * The accent's stand-alone (spacing) byte: what gets emitted when the
 * accent cannot compose — acute has no CP437 code point, so it falls back
 * to the apostrophe (documented substitution); grave/circumflex are ASCII.
 */
uint8_t keymap_dead_spacing(uint8_t accent);

/*
 * Compose @accent with @base.  Returns the number of bytes written to
 * out (which must hold 2): 1 when the pair composes to a single CP437
 * byte, 2 when it does not (spacing form + the base byte, so the stream
 * keeps both characters and their order).  base == ' ' composes to the
 * spacing form alone (the standard "accent + space" rule).
 */
int keymap_dead_compose(uint8_t accent, uint8_t base, uint8_t out[2]);

#endif /* AURALITE_DRIVERS_KEYBOARD_KEYMAP_H */
