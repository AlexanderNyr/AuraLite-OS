/* keymap.c — the shipped keyboard layouts and the (host-testable) decode
 * helper.  This file is deliberately free of kernel dependencies so
 * tests/unit/test_keymap.c can link it directly on the build host and check
 * every shipped table against the characters it promises. */

#include "drivers/keyboard/keymap.h"
#include "drivers/keyboard/keyboard.h"   /* KB_MOD_* mask bits */

/* Rows below follow the same scan-code geometry the driver always used:
 *   0x02..0x0D  digit row (1 2 3 4 5 6 7 8 9 0 and the two keys right of 0)
 *   0x10..0x1B  first letter row + the two keys right of P
 *   0x1E..0x29  home row + the key left of 1
 *   0x2C..0x35  bottom letter row
 *   0x56        the 102nd key between Left-Shift and the bottom row
 * Control bytes (ESC/BS/TAB/NL/keypad-'*'/space) stay fixed per layout. */

/* ---------------- US QWERTY (the two tables the driver always had) ------- */

const struct keymap keymap_us = {
    .lo = {
        0, 0x1B,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
        0, 'a','s','d','f','g','h','j','k','l',';','\'', '`',
        0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
        '*', 0, ' ',
    },
    .hi = {
        0, 0x1B,'!','@','#','$','%','^','&','*','(',')','_','+', '\b',
        '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
        0, 'A','S','D','F','G','H','J','K','L',':','"', '~',
        0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
        '*', 0, ' ',
    },
    /* US keyboards reach the special characters directly; no third layer. */
    .altgr = { 0 },
    .dead  = { 0 },   /* no dead keys on US */
    .name  = "us",
};

/* ---------------- German QWERTZ (DIN 2137 T1) ----------------------------
 *
 * The y/z swap, the digit-row symbols and the {[]}@|~ characters behind
 * AltGr are plain ASCII and therefore visible even through the legacy fd-0
 * line reader.  Umlauts/ß/§/°/µ are CP437 bytes > 0x7E: they reach
 * /dev/tty0 readers and the GUI, but the shell's fd-0 path drops them (see
 * keymap.h).  ´ is a dead key on hardware; with no dead-key support yet the
 * key emits nothing unshifted and an ASCII backquote shifted.  AltGr+E (€)
 * has no CP437 representation and is deliberately unmapped. */

const struct keymap keymap_de = {
    .lo = {
        0, 0x1B,'1','2','3','4','5','6','7','8','9','0',(char)0xE1/*ß*/,0,
        '\b',
        '\t','q','w','e','r','t','z','u','i','o','p',(char)0x81/*ü*/, '+',
        '\n',
        0, 'a','s','d','f','g','h','j','k','l',(char)0x94/*ö*/,
        (char)0x84/*ä*/, '^',
        0, '#','y','x','c','v','b','n','m',',','.','-', 0,
        '*', 0, ' ',
        [0x56] = '<',
    },
    .hi = {
        0, 0x1B,'!','"',(char)0x15/*§*/,'$','%','&','/','(',')','=','?',
        '`', '\b',
        '\t','Q','W','E','R','T','Z','U','I','O','P',(char)0x9A/*Ü*/, '*',
        '\n',
        0, 'A','S','D','F','G','H','J','K','L',(char)0x99/*Ö*/,
        (char)0x8E/*Ä*/, (char)0xF8/*°*/,
        0, '\'','Y','X','C','V','B','N','M',';',':','_', 0,
        '*', 0, ' ',
        [0x56] = '>',
    },
    .altgr = {
        [0x08] = '{',            /* AltGr+7 */
        [0x09] = '[',            /* AltGr+8 */
        [0x0A] = ']',            /* AltGr+9 */
        [0x0B] = '}',            /* AltGr+0 */
        [0x0C] = '\\',           /* AltGr+ß */
        [0x10] = '@',            /* AltGr+Q */
        [0x1B] = '~',            /* AltGr++ */
        [0x32] = (char)0xE6,     /* AltGr+M = µ (CP437) */
        [0x56] = '|',            /* AltGr+< */
    },
    /* RESIDUE2 T4: the two real German dead keys, armed on the plain
     * layer only.  The lo/hi tables above are UNCHANGED (their bytes are
     * pinned by tests/unit/test_keymap.c: lo[0x0D] == 0, hi[0x0D] == '`',
     * lo[0x29] == '^') — the accent semantics ride in this side table,
     * so the direct bytes stay reachable via Shift (`, °). */
    .dead = {
        [0x0D] = KB_DEAD_ACUTE,       /* ´ (right of ß)   */
        [0x29] = KB_DEAD_CIRCUMFLEX,  /* ^ (left of 1)    */
    },
    .name  = "de",
};

/* ---------------- registry + lookup -------------------------------------- */

const struct keymap *const keymap_registry[] = {
    &keymap_us,
    &keymap_de,
    (const struct keymap *)0,
};

const struct keymap *keymap_find(const char *name) {
    if (!name) return (const struct keymap *)0;
    for (int i = 0; keymap_registry[i]; i++) {
        const char *a = keymap_registry[i]->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return keymap_registry[i];
    }
    return (const struct keymap *)0;
}

char keymap_lookup(const struct keymap *km, uint8_t sc, uint8_t mods) {
    if (!km || sc >= 128) return 0;
    char c;
    if ((mods & KB_MOD_ALTGR) && km->altgr[sc]) {
        c = km->altgr[sc];        /* third layer wins, used verbatim */
    } else {
        c = (mods & KB_MOD_SHIFT) ? km->hi[sc] : km->lo[sc];
        /* CapsLock affects ASCII letters only and XORs with Shift. */
        if (c >= 'a' && c <= 'z' && (mods & KB_MOD_CAPS)) c = (char)(c - 32);
        else if (c >= 'A' && c <= 'Z' && (mods & KB_MOD_CAPS) &&
                (mods & KB_MOD_SHIFT)) c = (char)(c + 32);
    }
    return c;
}

/* ---------------- RESIDUE2 T4: dead keys (accents) ----------------------- */

uint8_t keymap_dead_of(const struct keymap *km, uint8_t sc, uint8_t mods) {
    if (!km || sc >= 128) return KB_DEAD_NONE;
    /* Plain layer only: Shift and AltGr keep their direct bytes (the
     * pinned hi[0x0D] == '`' must keep emitting a grave accent NOW, not
     * arm one), and CapsLock never applies to accents. */
    if (mods & (KB_MOD_SHIFT | KB_MOD_ALTGR)) return KB_DEAD_NONE;
    return (uint8_t)km->dead[sc];
}

uint8_t keymap_dead_spacing(uint8_t accent) {
    switch (accent) {
    case KB_DEAD_ACUTE:       return '\'';  /* CP437 has no ´; closest */
    case KB_DEAD_CIRCUMFLEX:  return '^';
    default:                  return 0;
    }
}

/*
 * The CP437 compositions.  CP437 carries the lowercase accented vowels
 * and É, but no Á Í Ó Ú / À È Ì Ò Ù / uppercase-circumflexed forms —
 * those pairs intentionally miss and take the 2-byte spacing path.
 */
int keymap_dead_compose(uint8_t accent, uint8_t base, uint8_t out[2]) {
    if (accent == KB_DEAD_NONE) { out[0] = base; return 1; }

    if (base == ' ') {                    /* accent + space = spacing form */
        out[0] = keymap_dead_spacing(accent);
        return out[0] ? 1 : 0;
    }

    static const struct { uint8_t accent, base, out; } table[] = {
        { KB_DEAD_ACUTE,      'a', 0xA0 },  /* á */
        { KB_DEAD_ACUTE,      'e', 0x82 },  /* é */
        { KB_DEAD_ACUTE,      'i', 0xA1 },  /* í */
        { KB_DEAD_ACUTE,      'o', 0xA2 },  /* ó */
        { KB_DEAD_ACUTE,      'u', 0xA3 },  /* ú */
        { KB_DEAD_ACUTE,      'E', 0x90 },  /* É */
        { KB_DEAD_CIRCUMFLEX, 'a', 0x83 },  /* â */
        { KB_DEAD_CIRCUMFLEX, 'e', 0x88 },  /* ê */
        { KB_DEAD_CIRCUMFLEX, 'i', 0x8C },  /* î */
        { KB_DEAD_CIRCUMFLEX, 'o', 0x93 },  /* ô */
        { KB_DEAD_CIRCUMFLEX, 'u', 0x96 },  /* û */
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].accent == accent && table[i].base == base) {
            out[0] = table[i].out;
            return 1;
        }
    }
    /* No composition: the accent's spacing form, then the base byte —
     * the stream keeps every character and its order (the X11 rule). */
    out[0] = keymap_dead_spacing(accent);
    out[1] = base;
    return (out[0] != 0) ? 2 : 1;
}
