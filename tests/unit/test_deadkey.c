/* test_deadkey.c — RESIDUE2 T4: the dead-key (accent) layer of keymap.c.
 *
 * Links the shipping keymap.c directly (the FIX_R8 pattern) and checks:
 *   - the lo/hi table BYTES the existing test_keymap.c pins are untouched
 *     (lo[0x0D] == 0, hi[0x0D] == '`', lo[0x29] == '^') — the accents ride
 *     in the side table, they do not rewite the direct layers;
 *   - keymap_dead_of(): plain layer only — Shift/AltGr keep the direct
 *     bytes, US arms nothing;
 *   - keymap_dead_compose(): the CP437 compositions, the accent+space
 *     rule, and the two-byte fallback that keeps an uncomposable base
 *     character unlost and in order;
 *   - keymap_dead_spacing(): the CP437-honest spacing forms.
 *
 * The keyboard.c state machine itself (arm/consume/cancel) is driven
 * end-to-end by tests/integration/cases/test_deadkeys.sh through real
 * sendkey presses.
 */

#include <stdio.h>
#include "drivers/keyboard/keymap.h"
#include "drivers/keyboard/keyboard.h"   /* KB_MOD_* mask bits */

static int failures = 0;

#define CK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

int main(void) {
    printf("test_deadkey: the dead-key (accent) layer (T4)\n");

    /* ---- the pinned direct layers are untouched ---------------------- */
    CK(keymap_de.lo[0x0D] == 0);          /* ´ key: no direct byte  */
    CK(keymap_de.hi[0x0D] == '`');        /* Shift+´: grave direct  */
    CK(keymap_de.lo[0x29] == '^');        /* ^ emits as ASCII       */
    CK(keymap_de.hi[0x29] == (char)0xF8); /* Shift+^: ° direct      */

    /* ---- the side table marks exactly the two German accents --------- */
    CK(keymap_de.dead[0x0D] == KB_DEAD_ACUTE);
    CK(keymap_de.dead[0x29] == KB_DEAD_CIRCUMFLEX);
    for (int sc = 0; sc < 128; sc++) {
        if (sc != 0x0D && sc != 0x29)
            CK(keymap_de.dead[sc] == KB_DEAD_NONE);
    }
    for (int sc = 0; sc < 128; sc++)
        CK(keymap_us.dead[sc] == KB_DEAD_NONE);   /* US arms nothing */

    /* ---- arming is plain-layer only ---------------------------------- */
    CK(keymap_dead_of(&keymap_de, 0x0D, 0) == KB_DEAD_ACUTE);
    CK(keymap_dead_of(&keymap_de, 0x29, 0) == KB_DEAD_CIRCUMFLEX);
    CK(keymap_dead_of(&keymap_de, 0x0D, KB_MOD_SHIFT) == KB_DEAD_NONE);
    CK(keymap_dead_of(&keymap_de, 0x29, KB_MOD_SHIFT) == KB_DEAD_NONE);
    CK(keymap_dead_of(&keymap_de, 0x0D, KB_MOD_ALTGR) == KB_DEAD_NONE);
    CK(keymap_dead_of(&keymap_us, 0x0D, 0) == KB_DEAD_NONE);
    CK(keymap_dead_of(&keymap_us, 0x29, 0) == KB_DEAD_NONE);
    CK(keymap_dead_of(0, 0x0D, 0) == KB_DEAD_NONE);        /* NULL map */
    CK(keymap_dead_of(&keymap_de, 200, 0) == KB_DEAD_NONE); /* out of range */

    /* ---- spacing forms (CP437 has no ´; apostrophe is documented) ---- */
    CK(keymap_dead_spacing(KB_DEAD_ACUTE) == '\'');
    CK(keymap_dead_spacing(KB_DEAD_CIRCUMFLEX) == '^');
    CK(keymap_dead_spacing(KB_DEAD_NONE) == 0);

    /* ---- compositions that exist in CP437 ----------------------------- */
    uint8_t out[2];
    CK(keymap_dead_compose(KB_DEAD_ACUTE, 'a', out) == 1 && out[0] == 0xA0);
    CK(keymap_dead_compose(KB_DEAD_ACUTE, 'e', out) == 1 && out[0] == 0x82);
    CK(keymap_dead_compose(KB_DEAD_ACUTE, 'i', out) == 1 && out[0] == 0xA1);
    CK(keymap_dead_compose(KB_DEAD_ACUTE, 'o', out) == 1 && out[0] == 0xA2);
    CK(keymap_dead_compose(KB_DEAD_ACUTE, 'u', out) == 1 && out[0] == 0xA3);
    CK(keymap_dead_compose(KB_DEAD_ACUTE, 'E', out) == 1 && out[0] == 0x90);
    CK(keymap_dead_compose(KB_DEAD_CIRCUMFLEX, 'a', out) == 1 && out[0] == 0x83);
    CK(keymap_dead_compose(KB_DEAD_CIRCUMFLEX, 'e', out) == 1 && out[0] == 0x88);
    CK(keymap_dead_compose(KB_DEAD_CIRCUMFLEX, 'i', out) == 1 && out[0] == 0x8C);
    CK(keymap_dead_compose(KB_DEAD_CIRCUMFLEX, 'o', out) == 1 && out[0] == 0x93);
    CK(keymap_dead_compose(KB_DEAD_CIRCUMFLEX, 'u', out) == 1 && out[0] == 0x96);

    /* ---- accent + space = the spacing form alone ---------------------- */
    CK(keymap_dead_compose(KB_DEAD_CIRCUMFLEX, ' ', out) == 1 && out[0] == '^');
    CK(keymap_dead_compose(KB_DEAD_ACUTE, ' ', out) == 1 && out[0] == '\'');

    /* ---- no CP437 form: spacing + base, nothing lost, order kept ------ */
    int n = keymap_dead_compose(KB_DEAD_ACUTE, 't', out);
    CK(n == 2 && out[0] == '\'' && out[1] == 't');
    n = keymap_dead_compose(KB_DEAD_ACUTE, 'A', out);   /* CP437 has no Á */
    CK(n == 2 && out[0] == '\'' && out[1] == 'A');
    n = keymap_dead_compose(KB_DEAD_CIRCUMFLEX, 'O', out);
    CK(n == 2 && out[0] == '^' && out[1] == 'O');

    if (failures == 0) printf("test_deadkey: ALL PASS\n");
    else printf("test_deadkey: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
