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
    /* OF1: no ulo/uhi/ualtgr — keymap_unicode() derives them from the bytes. */
};

/* ---------------- German QWERTZ (DIN 2137 T1) ----------------------------
 *
 * The y/z swap, the digit-row symbols and the {[]}@|~ characters behind
 * AltGr are plain ASCII and therefore visible even through the legacy fd-0
 * line reader.  Umlauts/ß/§/°/µ are CP437 bytes > 0x7E: they reach
 * /dev/tty0 readers and the GUI, but the shell's fd-0 path drops them (see
 * keymap.h).  ´ is a dead key on hardware; RESIDUE2 T4 arms it on the plain
 * unshifted layer (see the .dead side table below) and Shift still emits an
 * ASCII backquote.  AltGr+E (€)
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
    &keymap_ru,          /* OF1 */
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

/* ---------------- Russian ЙЦУКЕН (OFFICE_PLAN OF1) ----------------------
 *
 * The layout Russians actually type on: ЙЦУКЕН on the top letter row, ФЫВА on
 * the home row, ЯЧС on the bottom, ё on the key left of 1, and the punctuation
 * a Russian text needs (No on Shift+3, and "." / "," on the bottom-right
 * keys).  The digit row follows the Windows/GOST convention, so muscle memory
 * works.
 *
 * Two views again: the char tables carry what the CONSOLE can draw (ASCII and
 * the control keys; Cyrillic is 0 there, because the VGA font has no Cyrillic
 * at all) and the uint32_t tables carry what the key really means.
 * keymap_unicode() is the one that answers "what did the user type".
 */
static const uint32_t ru_ulo[128] = {
    0, 0x001B, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036,
    0x0037, 0x0038, 0x0039, 0x0030, 0x002D, 0x003D, 0x0008, 0x0009,
    0x0439, 0x0446, 0x0443, 0x043A, 0x0435, 0x043D, 0x0433, 0x0448,
    0x0449, 0x0437, 0x0445, 0x044A, 0x000A, 0, 0x0444, 0x044B,
    0x0432, 0x0430, 0x043F, 0x0440, 0x043E, 0x043B, 0x0434, 0x0436,
    0x044D, 0x0451, 0, 0x005C, 0x044F, 0x0447, 0x0441, 0x043C,
    0x0438, 0x0442, 0x044C, 0x0431, 0x044E, 0x002E, 0, 0,
    0, 0x0020, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0x005C, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

static const uint32_t ru_uhi[128] = {
    0, 0x001B, 0x0021, 0x0022, 0x2116, 0x003B, 0x0025, 0x003A,
    0x003F, 0x002A, 0x0028, 0x0029, 0x005F, 0x002B, 0x0008, 0x0009,
    0x0419, 0x0426, 0x0423, 0x041A, 0x0415, 0x041D, 0x0413, 0x0428,
    0x0429, 0x0417, 0x0425, 0x042A, 0x000A, 0, 0x0424, 0x042B,
    0x0412, 0x0410, 0x041F, 0x0420, 0x041E, 0x041B, 0x0414, 0x0416,
    0x042D, 0x0401, 0, 0x002F, 0x042F, 0x0427, 0x0421, 0x041C,
    0x0418, 0x0422, 0x042C, 0x0411, 0x042E, 0x002C, 0, 0,
    0, 0x0020, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0x002F, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

const struct keymap keymap_ru = {
    /* Console bytes: ASCII where the key really is ASCII, 0 where the character
     * exists only in Unicode (every Cyrillic letter).  Typing Russian into the
     * serial console therefore produces nothing rather than a wrong glyph --
     * the same refusal rule the font layer follows. */
    .lo = {
        0, 27, 49, 50, 51, 52, 53, 54,
        55, 56, 57, 48, 45, 61, 8, 9,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 10, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 92, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 46, 0, 0,
        0, 32, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 92, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    },
    .hi = {
        0, 27, 33, 34, 0, 59, 37, 58,
        63, 42, 40, 41, 95, 43, 8, 9,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 10, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 47, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 44, 0, 0,
        0, 32, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 47, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    },
    .altgr = { 0 },
    .dead  = { 0 },
    .name  = "ru",
    .ulo   = ru_ulo,
    .uhi   = ru_uhi,
    .ualtgr = 0,
};

/* ---------------- OFFICE_PLAN OF1: the Unicode layer --------------------- */

/* CP437 0x00..0x7F is ASCII, so the table starts at 0x80.  This is the VGA
 * font's repertoire written down: the reason a Russian document cannot be
 * typed into the console, and the reason the GUI needs its own rasteriser. */
static const uint32_t cp437_high[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

uint32_t cp437_to_unicode(uint8_t b) {
    return (b < 0x80) ? (uint32_t)b : cp437_high[b - 0x80];
}

/* Case-folding for the two blocks the shipped layouts use.  ASCII keeps the
 * driver's historical CapsLock rule; the Cyrillic block gets the equivalent so
 * that CapsLock + й is Й and Shift + й is Й as well. */
static uint32_t uni_upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp == 0x0451) return 0x0401;              /* ё -> Ё, the ONLY exception
                                                   * to the -0x20 rule below */
    if (cp >= 0x0430 && cp <= 0x044F) return cp - 0x20;    /* а..я -> А..Я */
    if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) {    /* à..þ -> À..Þ */
        return cp - 0x20;
    }
    return cp;
}

static uint32_t uni_lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
    if (cp == 0x0401) return 0x0451;
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) return cp + 0x20;
    return cp;
}

uint32_t keymap_unicode(const struct keymap *km, uint8_t sc, uint8_t mods) {
    if (!km || sc >= 128) return 0;

    if (km->ulo || km->uhi || km->ualtgr) {
        /* A layout with a Unicode layer: same rules, codepoints. */
        if ((mods & KB_MOD_ALTGR) && km->ualtgr && km->ualtgr[sc])
            return km->ualtgr[sc];
        uint32_t cp = (mods & KB_MOD_SHIFT) ? (km->uhi ? km->uhi[sc] : 0)
                                            : (km->ulo ? km->ulo[sc] : 0);
        if (cp == 0) return 0;
        if (mods & KB_MOD_CAPS) {
            /* CapsLock XOR Shift decides the case, which is the rule the ASCII
             * path has always applied — expressed on codepoints so it works for
             * ЙЦУКЕН as well as for QWERTY.  Punctuation has no case and is
             * left alone. */
            uint32_t lo = uni_lower(cp);
            uint32_t up = uni_upper(cp);
            if (lo != up)                    /* it is a letter */
                cp = (mods & KB_MOD_SHIFT) ? lo : up;
        }
        return cp;
    }

    /* Derived from the console byte tables: identical rules, one lookup more. */
    char c = keymap_lookup(km, sc, mods);
    return c ? cp437_to_unicode((uint8_t)c) : 0;
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
