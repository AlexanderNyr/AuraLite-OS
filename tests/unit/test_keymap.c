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
    CK(n == 2);   /* exactly us + de ship */
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
    if (failures) {
        printf("test_keymap: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_keymap: all checks passed\n");
    return 0;
}
