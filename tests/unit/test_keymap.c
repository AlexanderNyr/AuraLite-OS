/*
 * test_keymap.c — host-side unit test for FIXES_PLAN R8 (selectable keyboard
 * layouts).
 *
 * Gate sentence from the plan: "Each shipped layout maps a scancode set to
 * its expected characters, tested on the host against the table."
 *
 * This links the REAL drivers/keyboard/keymap.c (never a copy) and checks,
 * for every shipped layout:
 *   - the exact bytes of the tables (US must be the driver's old map_lo /
 *     map_hi moved verbatim; DE is DIN 2137 T1 with a CP437 extension row);
 *   - the decode semantics of keymap_lookup(): Shift selection, the
 *     CapsLock XOR rule for ASCII letters, AltGr precedence and fallback;
 *   - the registry contract keyboard_set_layout() relies on: findable,
 *     uniquely named, NULL-terminated.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */
#include <stdio.h>
#include <stdint.h>
#include "drivers/keyboard/keymap.h"
#include "drivers/keyboard/keyboard.h"   /* KB_MOD_* mask bits */

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* The exact tables the driver shipped before R8 — keymap_us must be these
 * two arrays, verbatim, or the default layout stops being "the current
 * behaviour" and every existing user's keyboard changes under them. */
static const char us_lo_ref[64] = {
    0, 0x1B,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'', '`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
static const char us_hi_ref[64] = {
    0, 0x1B,'!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"', '~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

static void test_us_verbatim(void) {
    int lo_ok = 1, hi_ok = 1;
    for (int i = 0; i < 64; i++) {
        if (keymap_us.lo[i] != us_lo_ref[i]) lo_ok = 0;
        if (keymap_us.hi[i] != us_hi_ref[i]) hi_ok = 0;
    }
    CK(lo_ok);   /* keymap_us.lo is the old map_lo, byte for byte */
    CK(hi_ok);   /* keymap_us.hi is the old map_hi, byte for byte */
    int altgr_empty = 1;
    for (int i = 0; i < 128; i++) if (keymap_us.altgr[i]) altgr_empty = 0;
    CK(altgr_empty);   /* US has no third layer */
    CK(keymap_us.name[0] == 'u' && keymap_us.name[1] == 's' &&
       keymap_us.name[2] == '\0');
}

static void test_de_table(void) {
    const struct keymap *d = &keymap_de;
    CK(d->name[0] == 'd' && d->name[1] == 'e' && d->name[2] == '\0');

    /* The y/z swap is the single most-typed difference. */
    CK(d->lo[0x15] == 'z' && d->hi[0x15] == 'Z');   /* US 'y' key */
    CK(d->lo[0x2C] == 'y' && d->hi[0x2C] == 'Y');   /* US 'z' key */

    /* Digit row: digits stay, the symbol layer is the German one. */
    CK(d->lo[0x02] == '1' && d->lo[0x0B] == '0');
    CK(d->hi[0x03] == '"');                 /* Shift+2, US gives '@'  */
    CK(d->hi[0x07] == '&' && d->hi[0x08] == '/');/* Shift+6/7      */
    CK(d->hi[0x09] == '(' && d->hi[0x0A] == ')' && d->hi[0x0B] == '=');

    /* Keys where German needs a CP437 byte (dropped by the fd-0 reader,
     * delivered to /dev/tty0 + the GUI — see keymap.h). */
    CK((unsigned char)d->lo[0x0C] == 0xE1); /* ß   */
    CK(d->hi[0x0C] == '?');
    CK((unsigned char)d->lo[0x1A] == 0x81); /* ü   */
    CK((unsigned char)d->lo[0x27] == 0x94); /* ö   */
    CK((unsigned char)d->lo[0x28] == 0x84); /* ä   */
    CK((unsigned char)d->hi[0x28] == 0x8E); /* Ä   */
    CK((unsigned char)d->hi[0x04] == 0x15); /* §   */

    /* ASCII-visible punctuation moves. */
    CK(d->lo[0x0D] == 0 && d->hi[0x0D] == '`');   /* dead ´: none / ` */
    CK(d->lo[0x29] == '^');                       /* dead ^ as ASCII  */
    CK(d->lo[0x2B] == '#' && d->hi[0x2B] == '\'');
    CK(d->lo[0x35] == '-' && d->hi[0x35] == '_'); /* US '/' key       */
    CK(d->lo[0x56] == '<' && d->hi[0x56] == '>'); /* 102nd key        */

    /* Control bytes must stay put so Enter/BKSP/Tab/Space work unchanged. */
    CK(d->lo[0x0E] == '\b' && d->hi[0x0E] == '\b');
    CK(d->lo[0x0F] == '\t' && d->lo[0x1C] == '\n' && d->lo[0x39] == ' ');
}

static void test_de_altgr_table(void) {
    const char *g = keymap_de.altgr;
    CK(g[0x08] == '{' && g[0x09] == '[' && g[0x0A] == ']' && g[0x0B] == '}');
    CK(g[0x0C] == '\\');                          /* AltGr+ß */
    CK(g[0x10] == '@');                           /* AltGr+Q */
    CK(g[0x1B] == '~');                           /* AltGr++ */
    CK((unsigned char)g[0x32] == 0xE6);           /* AltGr+M = µ    */
    CK(g[0x56] == '|');                           /* AltGr+<        */
    CK(g[0x12] == 0);                             /* AltGr+E (€) unmapped */
    /* Nothing but those nine entries: AltGr is not a shift-everything layer. */
    int count = 0;
    for (int i = 0; i < 128; i++) if (g[i]) count++;
    CK(count == 9);
}

static void test_lookup_semantics(void) {
    /* Plain + Shift selection on the US table. */
    CK(keymap_lookup(&keymap_us, 0x10, 0) == 'q');
    CK(keymap_lookup(&keymap_us, 0x10, KB_MOD_SHIFT) == 'Q');
    CK(keymap_lookup(&keymap_us, 0x03, KB_MOD_SHIFT) == '@');

    /* CapsLock XORs ASCII letters only, exactly like the pre-R8 driver. */
    CK(keymap_lookup(&keymap_us, 0x10, KB_MOD_CAPS) == 'Q');
    CK(keymap_lookup(&keymap_us, 0x10, KB_MOD_CAPS | KB_MOD_SHIFT) == 'q');
    CK(keymap_lookup(&keymap_us, 0x02, KB_MOD_CAPS) == '1');   /* digits immune */

    /* AltGr on US changes nothing (empty third layer falls back). */
    CK(keymap_lookup(&keymap_us, 0x10, KB_MOD_ALTGR) == 'q');

    /* AltGr on DE wins over the base layer... */
    CK(keymap_lookup(&keymap_de, 0x09, KB_MOD_ALTGR) == '[');
    CK(keymap_lookup(&keymap_de, 0x10, KB_MOD_ALTGR) == '@');
    /* ...is verbatim (Shift/CapsLock do not touch it)... */
    CK(keymap_lookup(&keymap_de, 0x09, KB_MOD_ALTGR | KB_MOD_SHIFT) == '[');
    CK(keymap_lookup(&keymap_de, 0x10, KB_MOD_ALTGR | KB_MOD_CAPS) == '@');
    /* ...and falls back to the plain/Shift layer where no AltGr entry
     * exists, so unmapped letters keep working under a lazy right Alt. */
    CK(keymap_lookup(&keymap_de, 0x1E, KB_MOD_ALTGR) == 'a');
    CK(keymap_lookup(&keymap_de, 0x1E, KB_MOD_ALTGR | KB_MOD_SHIFT) == 'A');

    /* The y/z swap flows through the same decode. */
    CK(keymap_lookup(&keymap_de, 0x15, 0) == 'z');
    CK(keymap_lookup(&keymap_de, 0x2C, KB_MOD_SHIFT) == 'Y');

    /* Out-of-range scan codes and empty cells decode to 0. */
    CK(keymap_lookup(&keymap_us, 0x80, 0) == 0);
    CK(keymap_lookup(&keymap_us, 0x40, 0) == 0);
    CK(keymap_lookup(0, 0x10, 0) == 0);
}

static void test_registry(void) {
    CK(keymap_find("us") == &keymap_us);
    CK(keymap_find("de") == &keymap_de);
    CK(keymap_find("xx") == 0);
    CK(keymap_find("") == 0);
    CK(keymap_find(0) == 0);
    /* Registry is NULL-terminated and names are unique. */
    int n = 0;
    for (; keymap_registry[n]; n++) {
        for (int j = n + 1; keymap_registry[j]; j++) {
            const char *a = keymap_registry[n]->name;
            const char *b = keymap_registry[j]->name;
            int same = 1;
            for (int k = 0; a[k] || b[k]; k++) if (a[k] != b[k]) same = 0;
            CK(!same);
        }
    }
    CK(n == 3);   /* exactly us + de + ru ship (ru is OFFICE_PLAN OF1) */
}

/* ---- OFFICE_PLAN OF1: the Russian layout and the Unicode layer ----------
 *
 * The gate sentence: "ru produces йцукен, ЙЦУКЕН, punctuation, and leaves
 * control bytes untouched".  Every one of those is a case below, checked
 * through keymap_unicode(), which is the function a GUI application reads.
 */
static void test_ru_letters(void) {
    /* ЙЦУКЕН: the six keys of the top letter row, left to right. */
    static const uint32_t want[] = { 0x0439, 0x0446, 0x0443, 0x043A, 0x0435, 0x043D };
    int ok = 1;
    for (int i = 0; i < 6; i++)
        if (keymap_unicode(&keymap_ru, (uint8_t)(0x10 + i), 0) != want[i]) ok = 0;
    CK(ok);   /* unshifted йцукен */

    ok = 1;
    for (int i = 0; i < 6; i++)
        if (keymap_unicode(&keymap_ru, (uint8_t)(0x10 + i), KB_MOD_SHIFT) !=
            want[i] - 0x20) ok = 0;
    CK(ok);   /* Shift gives ЙЦУКЕН */

    /* ФЫВА on the home row, ЯЧС on the bottom row: what a Russian user's hands
     * expect, not a transliteration of QWERTY. */
    CK(keymap_unicode(&keymap_ru, 0x1E, 0) == 0x0444);   /* ф */
    CK(keymap_unicode(&keymap_ru, 0x1F, 0) == 0x044B);   /* ы */
    CK(keymap_unicode(&keymap_ru, 0x20, 0) == 0x0432);   /* в */
    CK(keymap_unicode(&keymap_ru, 0x21, 0) == 0x0430);   /* а */
    CK(keymap_unicode(&keymap_ru, 0x2C, 0) == 0x044F);   /* я */
    CK(keymap_unicode(&keymap_ru, 0x2D, 0) == 0x0447);   /* ч */
    CK(keymap_unicode(&keymap_ru, 0x2E, 0) == 0x0441);   /* с */

    /* ё lives where it lives on a real keyboard: left of 1. */
    CK(keymap_unicode(&keymap_ru, 0x29, 0) == 0x0451);
    CK(keymap_unicode(&keymap_ru, 0x29, KB_MOD_SHIFT) == 0x0401);

    /* CapsLock is a letter lock and it XORs with Shift, exactly as the ASCII
     * rule has always done -- including for Ё, the one letter outside the
     * straight -0x20 case mapping. */
    CK(keymap_unicode(&keymap_ru, 0x10, KB_MOD_CAPS) == 0x0419);
    CK(keymap_unicode(&keymap_ru, 0x10, KB_MOD_CAPS | KB_MOD_SHIFT) == 0x0439);
    CK(keymap_unicode(&keymap_ru, 0x29, KB_MOD_CAPS) == 0x0401);
    /* ... and it must not touch punctuation. */
    CK(keymap_unicode(&keymap_ru, 0x35, KB_MOD_CAPS) == '.');
}

static void test_ru_punctuation(void) {
    CK(keymap_unicode(&keymap_ru, 0x35, 0) == '.');          /* bottom right */
    CK(keymap_unicode(&keymap_ru, 0x35, KB_MOD_SHIFT) == ',');
    CK(keymap_unicode(&keymap_ru, 0x04, KB_MOD_SHIFT) == 0x2116);  /* No. */
    CK(keymap_unicode(&keymap_ru, 0x05, KB_MOD_SHIFT) == ';');
    CK(keymap_unicode(&keymap_ru, 0x08, KB_MOD_SHIFT) == '?');
    CK(keymap_unicode(&keymap_ru, 0x0C, 0) == '-');
    CK(keymap_unicode(&keymap_ru, 0x0D, KB_MOD_SHIFT) == '+');
    CK(keymap_unicode(&keymap_ru, 0x39, 0) == ' ');          /* space */
}

static void test_ru_control_bytes(void) {
    /* Control keys keep the bytes every layout has always produced.  An
     * application that reads keymap_lookup() (the console, the shell) must see
     * no change at all when the layout is ru. */
    CK(keymap_lookup(&keymap_ru, 0x0E, 0) == '\b');
    CK(keymap_lookup(&keymap_ru, 0x0F, 0) == '\t');
    CK(keymap_lookup(&keymap_ru, 0x1C, 0) == '\n');
    CK(keymap_lookup(&keymap_ru, 0x01, 0) == 0x1B);
    CK(keymap_unicode(&keymap_ru, 0x1C, 0) == 0x0A);
    /* A Cyrillic letter has NO console byte: the VGA font cannot draw it, and
     * emitting a wrong glyph would be worse than emitting nothing. */
    CK(keymap_lookup(&keymap_ru, 0x10, 0) == 0);
    CK(keymap_lookup(&keymap_ru, 0x2C, 0) == 0);
    /* ASCII keys still work in the console under ru. */
    CK(keymap_lookup(&keymap_ru, 0x02, 0) == '1');
    CK(keymap_lookup(&keymap_ru, 0x35, 0) == '.');
}

static void test_unicode_layer(void) {
    /* us/de carry no tables of their own: keymap_unicode derives them from the
     * console bytes through CP437, so there is one authority per layout. */
    CK(keymap_us.ulo == 0 && keymap_de.ulo == 0);
    CK(keymap_unicode(&keymap_us, 0x10, 0) == 'q');
    CK(keymap_unicode(&keymap_us, 0x10, KB_MOD_SHIFT) == 'Q');
    CK(keymap_unicode(&keymap_us, 0x04, KB_MOD_SHIFT) == '#');   /* Shift+3 */
    /* The German umlauts arrive as codepoints, not as CP437 bytes. */
    CK(keymap_unicode(&keymap_de, 0x27, 0) == 0x00F6);           /* ö */
    CK(keymap_unicode(&keymap_de, 0x10, KB_MOD_ALTGR) == '@');   /* AltGr+Q */

    /* CP437 <-> Unicode, spot-checked where it matters: ASCII identity, an
     * umlaut, a box-drawing character, the no-break space. */
    CK(cp437_to_unicode('A') == 'A');
    CK(cp437_to_unicode(0x84) == 0x00E4);    /* ä */
    CK(cp437_to_unicode(0xC4) == 0x2500);    /* box drawing */
    CK(cp437_to_unicode(0xFF) == 0x00A0);    /* no-break space */
    CK(cp437_to_unicode(0xE1) == 0x00DF);    /* ß */
}

/* The modifier bits the keyboard driver feeds keymap_lookup() must keep
 * these values: kb_event_t.mods is a one-byte ABI for GUI clients. */
static void test_mod_bits(void) {
    CK(KB_MOD_SHIFT == 0x01 && KB_MOD_CTRL == 0x02 && KB_MOD_ALT == 0x04);
    CK(KB_MOD_CAPS == 0x08 && KB_MOD_META == 0x10);
    CK(KB_MOD_NUM == 0x20 && KB_MOD_SCROLL == 0x40 && KB_MOD_ALTGR == 0x80);
}

int main(void) {
    test_us_verbatim();
    test_de_table();
    test_de_altgr_table();
    test_lookup_semantics();
    test_registry();
    test_mod_bits();
    test_ru_letters();
    test_ru_punctuation();
    test_ru_control_bytes();
    test_unicode_layer();
    if (failures) {
        printf("test_keymap: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_keymap: all checks passed\n");
    return 0;
}
