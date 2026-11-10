/* tests/unit/test_ao_glyph.c — libaoglyph (OFFICE_PLAN.md OF1).
 *
 * Everything here is fed from byte arrays built inside this file: no font file
 * is read, so the gate is hermetic and runs on a bare CI host.  The cases are
 * the ones the plan names — a truncated font, an out-of-range glyph index, a
 * face at the 512-glyph cap — plus the three things that are easy to get wrong
 * and impossible to notice by eye: the baseline convention (yoff), the byte
 * alignment of a narrow glyph, and the exact pixels the style synthesis paints.
 */

#include <stdio.h>
#include <string.h>

#include "aoglyph.h"

static int checks = 0, failures = 0;

static void ck(const char *what, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* A hand-written BDF: five glyphs.
 *
 *   U+0041 'A'  5x5, yoff 0            — sits on the baseline
 *   U+0042 'B'  4x8 solid, yoff 0      — the pixel-count subject
 *   U+003F '?'  4x8, yoff 0            — the fallback glyph
 *   U+005A 'Z'  4x2, yoff 0            — narrow: pins the byte alignment
 *   U+0434 'д'  4x6, yoff -2           — descends two rows, like the letter
 */
static const char bdf_small[] =
    "STARTFONT 2.1\n"
    "FONTBOUNDINGBOX 8 12 0 -2\n"
    "STARTPROPERTIES 2\n"
    "FONT_ASCENT 9\n"
    "FONT_DESCENT 3\n"
    "ENDPROPERTIES\n"
    "SPACING \"P\"\n"
    /* The property line that broke every real font: it starts with "CHARS".
     * Kept in the fixture on purpose so the keyword match is graded. */
    "CHARSET_REGISTRY \"ISO10646\"\n"
    "CHARS 5\n"
    "STARTCHAR A\n"
    "ENCODING 65\n"
    "DWIDTH 6 0\n"
    "BBX 5 5 0 0\n"
    "BITMAP\n"
    "10\n"
    "10\n"
    "F8\n"
    "88\n"
    "88\n"
    "ENDCHAR\n"
    "STARTCHAR B\n"
    "ENCODING 66\n"
    "DWIDTH 5 0\n"
    "BBX 4 8 0 0\n"
    "BITMAP\n"
    "F0\n"
    "F0\n"
    "F0\n"
    "F0\n"
    "F0\n"
    "F0\n"
    "F0\n"
    "F0\n"
    "ENDCHAR\n"
    "STARTCHAR question\n"
    "ENCODING 63\n"
    "DWIDTH 4 0\n"
    "BBX 4 8 0 0\n"
    "BITMAP\n"
    "60\n"
    "90\n"
    "90\n"
    "20\n"
    "40\n"
    "00\n"
    "40\n"
    "00\n"
    "ENDCHAR\n"
    "STARTCHAR narrow\n"
    "ENCODING 90\n"
    "DWIDTH 4 0\n"
    "BBX 4 2 0 0\n"
    "BITMAP\n"
    "80\n"
    "10\n"
    "ENDCHAR\n"
    "STARTCHAR de\n"
    "ENCODING 1076\n"
    "DWIDTH 6 0\n"
    "BBX 4 6 1 -2\n"
    "BITMAP\n"
    "F0\n"
    "80\n"
    "80\n"
    "80\n"
    "90\n"
    "60\n"
    "ENDCHAR\n"
    "ENDFONT\n";

/* A minimal PSF2 container with a Unicode table, built at runtime. */
static size_t make_psf2(uint8_t *buf, int with_unicode) {
    enum { HDR = 32, GW = 8, GH = 16, N = 2 };
    const size_t gpb = (GW / 8) * GH;
    memset(buf, 0, HDR + gpb * N + 32);
    uint32_t *h = (uint32_t *)buf;
    h[0] = 0x864AB572u;
    h[1] = 0;
    h[2] = HDR;
    h[3] = with_unicode ? 1u : 0u;
    h[4] = N;
    h[5] = (uint32_t)gpb;
    h[6] = GH;
    h[7] = GW;
    uint8_t *g0 = buf + HDR;
    g0[0] = 0xFF;
    for (int y = 1; y < GH; y++) g0[y * (GW / 8)] = 0x80;
    uint8_t *g1 = buf + HDR + gpb;
    for (int y = 0; y < GH; y++) g1[y * (GW / 8)] = 0x01;
    size_t off = HDR + gpb * N;
    if (with_unicode) {
        uint16_t *u = (uint16_t *)(buf + off);
        size_t k = 0;
        u[k++] = 0xFFFE; u[k++] = 0;  u[k++] = 0x0041; u[k++] = 0xFFFF;  /* A */
        u[k++] = 0xFFFE; u[k++] = 1;  u[k++] = 0x0416; u[k++] = 0xFFFF;  /* Ж */
        off += k * 2;
    }
    return off;
}

/* A BDF with `n` glyphs: enough to test the 512 cap and the refusal above it. */
static size_t make_bdf_n(char *buf, size_t cap, int n) {
    size_t o = 0;
#define APP(...) o += (size_t)snprintf(buf + o, cap - o, __VA_ARGS__)
    APP("STARTFONT 2.1\nFONTBOUNDINGBOX 8 12 0 -2\nFONT_ASCENT 9\n"
        "FONT_DESCENT 3\nSPACING \"M\"\nCHARS %d\n", n);
    for (int i = 0; i < n; i++) {
        APP("STARTCHAR c%d\nENCODING %d\nDWIDTH 8 0\nBBX 1 1 0 0\nBITMAP\n"
            "80\nENDCHAR\n", i, 0x2100 + i);
    }
    APP("ENDFONT\n");
#undef APP
    return o;
}

int main(void) {
    static uint8_t storage[1 << 20];
    ao_arena_t arena;
    ao_font_t font;

    /* ---- a well-formed BDF ------------------------------------------------ */
    ao_arena_init(&arena, storage, sizeof(storage));
    int rc = ao_font_load_bdf(&font, &arena, bdf_small, sizeof(bdf_small) - 1);
    ck("bdf: loads", rc == AO_FONT_OK);
    ck("bdf: CHARSET_REGISTRY is a property, not a glyph count", rc == AO_FONT_OK);
    ck("bdf: five glyphs", font.n == 5);
    ck("bdf: proportional flag", font.proportional == 1);
    ck("bdf: ascent/descent", font.ascent == 9 && font.descent == 3);
    ck("bdf: cell 8x12", font.cell_w == 8 && font.cell_h == 12);
    ck("bdf: '?' became the fallback", font.fallback != 0xFFFF);

    /* ---- coverage: the Cyrillic block, and the fallback -------------------- */
    ck("coverage: has 'A'", ao_font_has(&font, 'A') == 1);
    ck("coverage: has 'д' U+0434", ao_font_has(&font, 0x0434) == 1);
    ck("coverage: no 'П' in this face", ao_font_has(&font, 0x041F) == 0);
    ck("coverage: U+0410..U+044F counts 1 here",
       ao_font_covers(&font, 0x0410, 0x044F) == 1);
    {
        const ao_glyph_t *g = ao_font_glyph(&font, 0x10FFFF);
        ck("coverage: unmapped codepoint falls back to '?'",
           g && g->cp == (uint32_t)'?');
        ck("coverage: advance of an unmapped codepoint is '?'s advance",
           ao_font_advance(&font, 0x10FFFF) == 4);
        ck("coverage: a codepoint below the table also falls back",
           ao_font_glyph(&font, 0x0001)->cp == (uint32_t)'?');
    }

    /* ---- byte alignment of a narrow glyph ---------------------------------
     *
     * BDF pads each row on the RIGHT, so pixel 0 is the MSB of the first byte:
     * row 0x80 is a pixel at x=0, row 0x10 is a pixel at x=3 in a four-pixel
     * box.  A generator that left-aligns to the glyph box instead produces
     * identical bytes for w == 8 and BLANK glyphs for every narrower one --
     * which is how the comma and the period went missing from the shipped face
     * while every letter looked perfect.  Both directions are pinned here.
     */
    {
        enum { W2 = 32, H2 = 16 };
        static uint32_t px2[W2 * H2];
        memset(px2, 0, sizeof(px2));
        ao_glyph_draw(&font, 'Z', AO_STYLE_NONE, px2, W2, W2, H2, 5, 10,
                      0x00FFFFFF);
        /* The glyph is two rows tall with yoff 0, so its TOP row sits one pixel
         * above the baseline (y = 9) and its bottom row on it (y = 10). */
        ck("bdf: row 0x80 in a 4-wide box inks x=0",
           px2[9 * W2 + 5] == 0x00FFFFFF);
        ck("bdf: row 0x10 in a 4-wide box inks x=3",
           px2[10 * W2 + 8] == 0x00FFFFFF && px2[10 * W2 + 5] == 0);
    }

    /* ---- the baseline convention ------------------------------------------ */
    {
        const ao_glyph_t *a = ao_font_glyph(&font, 'A');
        const ao_glyph_t *d = ao_font_glyph(&font, 0x0434);
        ck("geometry: 'A' is 5x5 with yoff 0", a->w == 5 && a->h == 5 && a->yoff == 0);
        ck("geometry: 'д' descends: yoff is negative", d->yoff == -2);
        ck("geometry: advances are per glyph", a->adv == 6 && d->adv == 6);
    }

    /* ---- style synthesis: exact pixel counts ------------------------------ */
    {
        enum { W = 64, H = 32 };
        static uint32_t px[W * H];
        const uint32_t ink = 0x00FFFFFF;

        memset(px, 0, sizeof(px));
        int plain = ao_glyph_draw(&font, 'B', AO_STYLE_NONE, px, W, W, H,
                                  2, 20, ink);
        ck("style: plain 'B' paints 32 pixels", plain == 32);

        memset(px, 0, sizeof(px));
        int bold = ao_glyph_draw(&font, 'B', AO_STYLE_BOLD, px, W, W, H,
                                 2, 20, ink);
        ck("style: bold smears one column right, 40 pixels", bold == 40);
        {
            static uint32_t plain_px[W * H];
            memset(plain_px, 0, sizeof(plain_px));
            ao_glyph_draw(&font, 'B', AO_STYLE_NONE, plain_px, W, W, H,
                          2, 20, ink);
            ck("style: bold extends the row to the right, not the left",
               px[13 * W + 6] == ink && plain_px[13 * W + 6] != ink &&
               px[13 * W + 1] == 0 && plain_px[13 * W + 1] == 0);
        }

        /* Italic is a shear, and a solid row moved one pixel right is still
         * four pixels: the count cannot show it.  Check the geometry instead,
         * on 'A' (5x5, yoff 0, top row = one pixel at px 3, which sits at
         * dy 4 and therefore leans floor(4/4) = 1 pixel). */
        {
            const int pen = 10, base = 20;
            memset(px, 0, sizeof(px));
            ao_glyph_draw(&font, 'A', AO_STYLE_NONE, px, W, W, H, pen, base, ink);
            int plain_sx = pen + 3;
            ck("style: plain 'A' has its top pixel at x13",
               px[16 * W + plain_sx] == ink && px[16 * W + plain_sx + 1] != ink);

            memset(px, 0, sizeof(px));
            int ital = ao_glyph_draw(&font, 'A', AO_STYLE_ITALIC, px, W, W, H,
                                     pen, base, ink);
            ck("style: italic 'A' leans its top row one pixel right",
               px[16 * W + plain_sx + 1] == ink && px[16 * W + plain_sx] != ink);
            ck("style: italic leaves the baseline row alone",
               px[base * W + pen] == ink && px[base * W + pen + 4] == ink &&
               px[base * W + pen + 5] != ink);
            ck("style: italic paints the same number of pixels as plain",
               ital == 11);
        }

        memset(px, 0, sizeof(px));
        int both = ao_glyph_draw(&font, 'B', AO_STYLE_BOLD | AO_STYLE_ITALIC,
                                 px, W, W, H, 2, 20, ink);
        ck("style: bold+italic paints at least a bold glyph", both >= bold);

        int clipped = ao_glyph_draw(&font, 'B', AO_STYLE_NONE, px, W, W, H,
                                    2, -100, ink);
        ck("clip: a baseline above the buffer paints 0 pixels", clipped == 0);
        clipped = ao_glyph_draw(&font, 'B', AO_STYLE_NONE, px, W, W, H,
                                -100, 20, ink);
        ck("clip: a pen left of the buffer paints 0 pixels", clipped == 0);
    }

    /* ---- text drawing and measurement ------------------------------------- */
    {
        enum { W = 128, H = 32 };
        static uint32_t px[W * H];
        memset(px, 0, sizeof(px));
        int width = 0;
        const char *s = "AB";
        int painted = ao_text_draw(&font, s, 2, AO_STYLE_NONE, px, W, W, H,
                                   0, 20, 0xFFFFFFFF, &width);
        ck("text: 'AB' advances 6+5", width == 11);
        ck("text: 'AB' paints 11 + 32 pixels", painted == 43);
        ck("text: measure agrees with draw", ao_text_width(&font, s, 2) == width);

        memset(px, 0, sizeof(px));
        const char bad[] = { 'A', (char)0xFF, 'A' };
        width = 0;
        ao_text_draw(&font, bad, sizeof(bad), AO_STYLE_NONE, px, W, W, H,
                     0, 20, 0xFFFFFFFF, &width);
        ck("text: damaged byte becomes '?' and keeps the pen moving",
           width == 6 + 4 + 6);
    }

    /* ---- greedy line breaking --------------------------------------------- */
    {
        const char *s = "AB AB AB";
        int w = 0;
        size_t n = ao_text_fit(&font, s, strlen(s), 15, &w);
        ck("wrap: 'AB ' fits in 15px and breaks after the space",
           n == 3 && w == 15);
        n = ao_text_fit(&font, s, strlen(s), 100, &w);
        ck("wrap: everything fits when the line is wide", n == strlen(s));
        n = ao_text_fit(&font, "ABABAB", 6, 8, &w);
        ck("wrap: an over-wide word yields a non-zero prefix", n > 0 && n < 6);
        ck("wrap: space and hyphen are break opportunities",
           ao_text_is_break(' ') && ao_text_is_break('-'));
        ck("wrap: the em dash breaks after itself", ao_text_is_break(0x2014));
        ck("wrap: no-break space and quotes are not",
           !ao_text_is_break(0x00A0) && !ao_text_is_break(0x00AB));
    }

    /* ---- a truncated font ------------------------------------------------- */
    {
        ao_arena_t a2;
        static uint8_t st2[1 << 16];
        ao_arena_init(&a2, st2, sizeof(st2));
        ao_font_t f2;
        size_t cut = sizeof(bdf_small) - 1 - 60;
        ck("bdf: a font cut mid-glyph is refused",
           ao_font_load_bdf(&f2, &a2, bdf_small, cut) == AO_FONT_E_TRUNCATED);
        ck("bdf: a refused font loads zero glyphs", f2.n == 0);
        ck("bdf: empty input is an argument error",
           ao_font_load_bdf(&f2, &a2, "", 0) == AO_FONT_E_ARG);
        ck("bdf: garbage is a format error",
           ao_font_load_bdf(&f2, &a2, "hello\nworld\n", 12) == AO_FONT_E_FORMAT);
        ck("bdf: CHARS smaller than the glyphs present is a format error",
           ao_font_load_bdf(&f2, &a2,
                            "STARTFONT 2.1\nCHARS 1\nSTARTCHAR a\nENCODING 65\n"
                            "DWIDTH 8 0\nBBX 1 1 0 0\nBITMAP\n80\nENDCHAR\n"
                            "STARTCHAR b\nENCODING 66\nDWIDTH 8 0\nBBX 1 1 0 0\n"
                            "BITMAP\n80\nENDCHAR\nENDFONT\n", 178) ==
               AO_FONT_E_FORMAT);
    }

    /* ---- the 512-glyph cap, and one glyph past it ------------------------- */
    {
        static char big[1 << 20];
        static uint8_t st3[1 << 19];
        ao_arena_t a3;
        ao_font_t f3;
        size_t n = make_bdf_n(big, sizeof(big), AO_GLYPH_MAX);
        ao_arena_init(&a3, st3, sizeof(st3));
        ck("cap: a face with exactly 512 glyphs loads",
           ao_font_load_bdf(&f3, &a3, big, n) == AO_FONT_OK);
        ck("cap: and it kept all 512", f3.n == AO_GLYPH_MAX);

        n = make_bdf_n(big, sizeof(big), AO_GLYPH_MAX + 1);
        ao_arena_init(&a3, st3, sizeof(st3));
        ck("cap: 513 glyphs is refused, not truncated",
           ao_font_load_bdf(&f3, &a3, big, n) == AO_FONT_E_CAPACITY);
        ck("cap: the refused font is empty", f3.n == 0);
    }

    /* ---- a tiny arena is a receipt, not a crash --------------------------- */
    {
        static uint8_t tiny[12];   /* one glyph's 8 bytes, then nothing */
        ao_arena_t a4;
        ao_font_t f4;
        ao_arena_init(&a4, tiny, sizeof(tiny));
        ck("storage: an arena too small is refused",
           ao_font_load_bdf(&f4, &a4, bdf_small, sizeof(bdf_small) - 1) ==
               AO_FONT_E_STORAGE);
    }

    /* ---- PSF2, with and without a Unicode table --------------------------- */
    {
        static uint8_t psf[4096];
        ao_arena_t a5;
        ao_font_t f5;
        ao_arena_init(&a5, storage, sizeof(storage));

        size_t n = make_psf2(psf, 0);
        ck("psf2: loads without a Unicode table",
           ao_font_load_psf2(&f5, &a5, psf, n, 12) == AO_FONT_OK);
        ck("psf2: two glyphs", f5.n == 2);
        ck("psf2: without a table, glyph i is codepoint i",
           f5.g[0].cp == 0 && f5.g[1].cp == 1 && !ao_font_has(&f5, 0x41));
        ck("psf2: ascent/descent from the argument",
           f5.ascent == 12 && f5.descent == 4);
        ck("psf2: monospaced advance is the cell width",
           ao_font_advance(&f5, 0x42) == 8);

        n = make_psf2(psf, 1);
        ck("psf2: loads with a Unicode table",
           ao_font_load_psf2(&f5, &a5, psf, n, 12) == AO_FONT_OK);
        ck("psf2: the table maps glyph 0 to 'A'", ao_font_has(&f5, 'A'));
        ck("psf2: the table maps glyph 1 to Ж", ao_font_has(&f5, 0x0416));
        ck("psf2: an unmapped byte is not a glyph", !ao_font_has(&f5, 0x042F));

        ck("psf2: a 16-byte file is truncated",
           ao_font_load_psf2(&f5, &a5, psf, 16, 12) == AO_FONT_E_TRUNCATED);
        ck("psf2: glyph data cut short is truncated",
           ao_font_load_psf2(&f5, &a5, psf, 32 + 16, 12) == AO_FONT_E_TRUNCATED);
        static uint8_t junk[64];
        memset(junk, 0, sizeof(junk));
        ck("psf2: a bad magic is a format error",
           ao_font_load_psf2(&f5, &a5, junk, sizeof(junk), 12) == AO_FONT_E_FORMAT);
        ck("psf2: NULL data is an argument error",
           ao_font_load_psf2(&f5, &a5, NULL, 0, 12) == AO_FONT_E_ARG);
    }

    printf("AO GLYPH: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
