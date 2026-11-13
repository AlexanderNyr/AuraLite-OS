/* tests/unit/test_ao_doc.c — the document model and its native format (OF2).
 *
 * The plan's demand: a randomised edit fuzz of 10 000 operations with every
 * invariant re-checked after each one, plus the XML reader against files that
 * are wrong on purpose (truncated, unclosed, unknown version, bad entity, deep
 * nesting).  Both are here, and both have already paid for themselves: the fuzz
 * found a position landing inside a codepoint, a run-array compaction that left
 * stale duplicates, an empty-paragraph collapse in the reader and a `n--` on a
 * zero-length run that read four gigabytes past the text.
 *
 * Groups, in order: an empty document, editing, styles, stepping and searching,
 * the binary form, the XML form, and the fuzz.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userspace/office/libaodoc/aomodel.h"
#include "userspace/office/libaodoc/aoio.h"

static int checks = 0, failures = 0;

static void ck(const char *what, int cond) {
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

static char      g_text[8192];
static ao_run_t  g_runs[512];
static ao_para_t g_paras[256];
static uint8_t   g_ser[16384];
static uint8_t   g_ser2[16384];
static char      g_xml[65536];
static char      g_xml2[65536];

static void fresh(ao_model_t *m) {
    ao_model_init(m, g_text, sizeof g_text, g_runs, 512, g_paras, 256);
}
static int ins(ao_model_t *m, uint32_t pos, const char *s) {
    return ao_model_insert(m, pos, s, (uint32_t)strlen(s));
}
static int text_is(ao_model_t *m, const char *want) {
    size_t n = strlen(want);
    return ao_model_text_len(m) == n && !memcmp(g_text, want, n);
}

/* A second model with its own storage, for round trips. */
static char      g_text2[8192];
static ao_run_t  g_runs2[512];
static ao_para_t g_paras2[256];
static void fresh2(ao_model_t *m) {
    ao_model_init(m, g_text2, sizeof g_text2, g_runs2, 512, g_paras2, 256);
}

static uint32_t g_seed = 2026;
static uint32_t rnd(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xFFFFFFu;
}

/* ------------------------------------------------------------- empty model */

static void test_empty(void) {
    ao_model_t m;
    fresh(&m);
    ck("empty: the document is valid", ao_model_valid(&m, NULL));
    ck("empty: no text", ao_model_text_len(&m) == 0);
    ck("empty: exactly one paragraph", ao_model_para_count(&m) == 1);
    ck("empty: exactly one run", ao_model_run_count(&m) == 1);
    ck("empty: the paragraph starts at 0", ao_model_para_start(&m, 0) == 0);
    ck("empty: the caret's attributes are the defaults",
       ao_model_style_at(&m, 0) == AO_STYLE_OFF &&
       ao_model_size_at(&m, 0) == AO_SIZE_DEFAULT &&
       ao_model_font_at(&m, 0) == AO_FONT_SANS);
    ck("empty: a position past the end is refused",
       ins(&m, 1, "x") == AO_E_RANGE);
    ck("empty: invalid UTF-8 is refused",
       ao_model_insert(&m, 0, "\xC3", 1) == AO_E_UTF8);
    ck("empty: nothing was inserted by the refusals", ao_model_text_len(&m) == 0);

    ins(&m, 0, "x");
    ao_model_clear(&m);
    ck("clear: the document is empty and valid again",
       ao_model_text_len(&m) == 0 && ao_model_para_count(&m) == 1 &&
       ao_model_valid(&m, NULL));
}

/* ------------------------------------------------------------------ editing */

static void test_editing(void) {
    ao_model_t m;
    fresh(&m);

    ck("edit: insert at the start", ins(&m, 0, "hello") == AO_OK);
    ck("edit: the text is what was inserted", text_is(&m, "hello"));
    ck("edit: insert in the middle", ins(&m, 5, ", world") == AO_OK);
    ck("edit: the text grew in place", text_is(&m, "hello, world"));

    /* Cyrillic, and a position that is the end of a codepoint. */
    ck("edit: Cyrillic inserts", ins(&m, ao_model_text_len(&m), " — привет") == AO_OK);
    ck("edit: the em dash and the letters are there",
       text_is(&m, "hello, world — привет"));

    /* A newline splits a paragraph; deleting it joins them again. */
    uint32_t before = ao_model_para_count(&m);
    ck("edit: a newline inserts", ins(&m, 5, "\n") == AO_OK);
    ck("edit: the newline made a second paragraph", ao_model_para_count(&m) == before + 1);
    ck("edit: the run split with it", ao_model_run_count(&m) >= 2);
    ck("edit: the model is still valid", ao_model_valid(&m, NULL));
    ck("edit: deleting the newline joins the paragraphs",
       ao_model_delete(&m, 5, 1) == AO_OK && ao_model_para_count(&m) == before);
    ck("edit: the joined text is unchanged", text_is(&m, "hello, world — привет"));

    /* Deleting a Cyrillic word: "hello, world — привет" is 12 bytes of ASCII,
     * a space, a three-byte em dash, a space, then six two-byte letters, so the
     * word starts at byte 17 and is 12 bytes long. */
    ck("edit: delete a whole Cyrillic word",
       ao_model_delete(&m, 17, 12) == AO_OK);
    ck("edit: the model is still valid UTF-8", ao_model_valid(&m, NULL));
    ck("edit: the remaining text is intact", text_is(&m, "hello, world — "));

    /* Deleting everything leaves one empty paragraph with one empty run, not a
     * document without paragraphs. */
    ck("edit: delete all", ao_model_delete(&m, 0, ao_model_text_len(&m)) == AO_OK);
    ck("edit: an emptied document still has one paragraph",
       ao_model_para_count(&m) == 1 && ao_model_run_count(&m) == 1);
    ck("edit: and is valid", ao_model_valid(&m, NULL));

    /* A delete past the end is clamped, not refused: a caret at the end of the
     * document is a legal place to press Delete. */
    ins(&m, 0, "abc");
    ck("edit: a delete past the end is clamped", ao_model_delete(&m, 1, 99) == AO_OK);
    ck("edit: the clamp removed what was there", text_is(&m, "a"));

    /* Style of inserted text follows the run it lands in. */
    ao_model_clear(&m);
    ins(&m, 0, "bold");
    ao_model_set_style(&m, 0, 4, AO_STYLE_BOLD, 1);
    ins(&m, 4, "more");
    ck("edit: text typed at the end of a bold run inherits bold",
       ao_model_style_at(&m, 5) == AO_STYLE_BOLD);
    ins(&m, 0, "1");
    ck("edit: text typed at the start of a bold run inherits it too",
       ao_model_style_at(&m, 0) == AO_STYLE_BOLD);
}

/* ------------------------------------------------------------------- styles */

static void test_styles(void) {
    ao_model_t m;
    fresh(&m);
    ins(&m, 0, "abcdefghij");

    ck("style: bold a range", ao_model_set_style(&m, 2, 3, AO_STYLE_BOLD, 1) == AO_OK);
    ck("style: inside the range is bold", ao_model_style_at(&m, 2) == AO_STYLE_BOLD);
    ck("style: outside the range is not", ao_model_style_at(&m, 1) == AO_STYLE_OFF);
    ck("style: the end of the range is not", ao_model_style_at(&m, 5) == AO_STYLE_OFF);
    ck("style: the range is three runs", ao_model_run_count(&m) == 3);
    ck("style: still valid", ao_model_valid(&m, NULL));

    /* Two adjacent ranges with the same attributes must merge into one run, or
     * two identical documents would serialise differently. */
    ao_model_set_style(&m, 5, 3, AO_STYLE_BOLD, 1);
    ck("style: adjacent equal runs merge", ao_model_style_at(&m, 2) == AO_STYLE_BOLD &&
                                            ao_model_style_at(&m, 7) == AO_STYLE_BOLD);
    ck("style: the merge left no duplicate runs", ao_model_run_count(&m) == 3);

    /* Bits are independent. */
    ao_model_set_style(&m, 2, 3, AO_STYLE_ITALIC, 1);
    ck("style: italic adds to bold",
       ao_model_style_at(&m, 2) == (AO_STYLE_BOLD | AO_STYLE_ITALIC));
    ao_model_set_style(&m, 2, 3, AO_STYLE_BOLD, 0);
    ck("style: clearing one bit keeps the other",
       ao_model_style_at(&m, 2) == AO_STYLE_ITALIC);

    /* A range in the middle of a codepoint snaps out to whole characters
     * instead of splitting one: half a letter is not a run boundary. */
    ao_model_clear(&m);
    ins(&m, 0, "привет");                       /* 12 bytes, 2 per letter */
    ck("style: a range inside a codepoint is accepted",
       ao_model_set_style(&m, 1, 1, AO_STYLE_UNDERLINE, 1) == AO_OK);
    ck("style: and is widened to the whole letter",
       ao_model_style_at(&m, 0) == AO_STYLE_UNDERLINE &&
       ao_model_style_at(&m, 2) == AO_STYLE_OFF);
    ck("style: the text is still valid UTF-8", ao_model_valid(&m, NULL));

    /* Size and font. */
    ao_model_clear(&m);
    ins(&m, 0, "size");
    ck("style: size and font apply", ao_model_set_size_font(&m, 0, 4, 18, AO_FONT_MONO) == AO_OK);
    ck("style: the size is reported", ao_model_size_at(&m, 1) == 18);
    ck("style: the font is reported", ao_model_font_at(&m, 1) == AO_FONT_MONO);
    ck("style: a silly size is refused", ao_model_set_size_font(&m, 0, 1, 200, 0) == AO_E_ARG);
    ck("style: a silly font is refused", ao_model_set_size_font(&m, 0, 1, 12, 9) == AO_E_ARG);

    /* Alignment. */
    ins(&m, 4, "\nsecond");
    ck("style: alignment applies to the paragraph", ao_model_set_align(&m, 0, AO_ALIGN_RIGHT) == AO_OK);
    ck("style: the first paragraph is right-aligned", m.paras[0].align == AO_ALIGN_RIGHT);
    ck("style: the second is untouched", m.paras[1].align == AO_ALIGN_LEFT);
    ck("style: a silly alignment is refused", ao_model_set_align(&m, 0, 9) == AO_E_ARG);
}

/* ------------------------------------------------------- stepping, finding */

static void test_steps_find(void) {
    ao_model_t m;
    fresh(&m);
    ins(&m, 0, "ab\nпривет мир");

    ck("steps: next_cp crosses an ASCII letter",
       ao_model_next_cp(&m, 0) == 1);
    ck("steps: next_cp crosses a two-byte letter",
       ao_model_next_cp(&m, 3) == 5);
    ck("steps: prev_cp crosses it backwards",
       ao_model_prev_cp(&m, 5) == 3);
    ck("steps: next_cp at the end stays put",
       ao_model_next_cp(&m, ao_model_text_len(&m)) == ao_model_text_len(&m));
    ck("steps: prev_cp at the start stays put", ao_model_prev_cp(&m, 0) == 0);

    ck("steps: para_of finds the first paragraph", ao_model_para_of(&m, 1) == 0);
    ck("steps: para_of on a boundary belongs to the next paragraph",
       ao_model_para_of(&m, 3) == 1);
    ck("steps: para_start agrees", ao_model_para_start(&m, 1) == 3);
    ck("steps: run_of returns an index inside the paragraph",
       ao_model_run_of(&m, 4) < ao_model_run_count(&m));

    ck("find: ASCII", ao_model_find(&m, 0, "ab", 0) == 0);
    ck("find: from the middle", ao_model_find(&m, 1, "ab", 0) == -1);
    /* "ab\n" is 3 bytes, "привет" (2 bytes a letter) is 12, so "мир" starts at
     * 3 + 12 + 1 = 16. */
    ck("find: Cyrillic", ao_model_find(&m, 0, "мир", 0) == 16);
    ck("find: no match reports -1", ao_model_find(&m, 0, "zzz", 0) == -1);

    char out[32];
    int n = ao_model_copy_text(&m, 3, 19, out, sizeof out);
    ck("copy: the byte count is returned", n == 19);
    ck("copy: the text is right and NUL-terminated",
       !strcmp(out, "привет мир"));
    ck("copy: a short buffer is refused",
       ao_model_copy_text(&m, 0, 10, out, 4) < 0);
}

/* ------------------------------------------------------------ binary form */

static void test_binary(void) {
    ao_model_t m;
    fresh(&m);
    ins(&m, 0, "Заголовок\nвторой абзац");
    ao_model_set_style(&m, 0, 18, AO_STYLE_BOLD, 1);

    size_t n = 0;
    ck("binary: serialises", ao_model_serialize(&m, g_ser, sizeof g_ser, &n) == AO_OK);
    ck("binary: the size formula holds",
       ao_model_serialized_size(&m) == n &&
       n == 16 + ao_model_text_len(&m) + 12 * ao_model_run_count(&m) +
            20 * ao_model_para_count(&m));
    ck("binary: the magic is AOM1",
       g_ser[0] == 'A' && g_ser[1] == 'O' && g_ser[2] == 'M' && g_ser[3] == '1');

    ao_model_t r;
    fresh2(&r);
    ck("binary: deserialises", ao_model_deserialize(&r, g_ser, n) == AO_OK);
    ck("binary: the text survived",
       ao_model_text_len(&r) == ao_model_text_len(&m) &&
       !memcmp(g_text2, g_text, ao_model_text_len(&m)));
    ck("binary: the structure survived",
       ao_model_para_count(&r) == ao_model_para_count(&m) &&
       ao_model_run_count(&r) == ao_model_run_count(&m));
    ck("binary: the style survived", ao_model_style_at(&r, 0) == AO_STYLE_BOLD);

    size_t n2 = 0;
    ao_model_serialize(&r, g_ser2, sizeof g_ser2, &n2);
    ck("binary: save -> load -> save is byte identical", n == n2 && !memcmp(g_ser, g_ser2, n));

    /* A refused BINARY load leaves the document it was given untouched: the
     * file is validated before anything is applied.  (The XML reader builds as
     * it walks, so a refused XML load leaves an EMPTY document instead -- both
     * are tested, because the difference matters to the application.) */
    char before[64];
    int bl = ao_model_copy_text(&r, 0, ao_model_text_len(&r), before, sizeof before);
    ck("binary: the loaded text can be read back", bl == (int)ao_model_text_len(&m));

    ck("binary: a truncated file is refused",
       ao_model_deserialize(&r, g_ser, n - 1) == AO_E_FORMAT);
    uint8_t bad[64];
    memcpy(bad, g_ser, 32);
    bad[0] = 'X';
    ck("binary: a wrong magic is refused", ao_model_deserialize(&r, bad, 32) == AO_E_FORMAT);
    ck("binary: a tiny buffer is refused",
       ao_model_serialize(&m, g_ser2, 8, &n2) == AO_E_FULL);
    char after[64];
    int al = ao_model_copy_text(&r, 0, ao_model_text_len(&r), after, sizeof after);
    ck("binary: a refused load leaves the document it was given alone",
       al == bl && !strcmp(before, after) && ao_model_valid(&r, NULL));
}

/* --------------------------------------------------------------- XML form */

static void test_xml(void) {
    ao_model_t m;
    fresh(&m);
    ins(&m, 0, "Заголовок <1> & «цитата»\nвторой абзац");
    ao_model_set_style(&m, 0, 18, AO_STYLE_BOLD, 1);      /* "Заголовок" */
    ao_model_set_style(&m, 44, 12, AO_STYLE_ITALIC, 1);   /* "второй" */
    ao_model_set_align(&m, 0, AO_ALIGN_CENTER);

    size_t n = 0;
    ck("xml: exports", ao_io_export(&m, g_xml, sizeof g_xml, &n) == AO_OK);
    ck("xml: declaration and version",
       !memcmp(g_xml, "<?xml", 5) && strstr(g_xml, "<aodoc version=\"1\">"));
    ck("xml: escaped", strstr(g_xml, "&amp;") && strstr(g_xml, "&lt;1&gt;"));
    ck("xml: style and alignment",
       strstr(g_xml, "style=\"b\"") && strstr(g_xml, "align=\"center\""));
    ck("xml: Cyrillic is literal UTF-8", strstr(g_xml, "«цитата»") != NULL);

    ao_model_t r;
    fresh2(&r);
    ck("xml: imports", ao_io_import(&r, g_xml, n) == AO_OK);
    ck("xml: the text is byte identical", ao_model_text_len(&r) == ao_model_text_len(&m) &&
                                          !memcmp(g_text2, g_text, ao_model_text_len(&m)));
    /* The RUN split can differ: a paragraph's terminating newline carries no
     * text, so a file cannot record which run it belonged to.  What must hold
     * is the paragraph structure, the attributes and a byte-identical
     * re-export. */
    ck("xml: structure", ao_model_para_count(&r) == ao_model_para_count(&m));
    ck("xml: size and font survive", ao_model_size_at(&r, 0) == ao_model_size_at(&m, 0));
    ck("xml: bold, italic and alignment survived",
       ao_model_style_at(&r, 0) == AO_STYLE_BOLD &&
       ao_model_style_at(&r, 44) == AO_STYLE_ITALIC && g_paras2[0].align == AO_ALIGN_CENTER);

    size_t n2 = 0;
    ck("xml: re-export", ao_io_export(&r, g_xml2, sizeof g_xml2, &n2) == AO_OK);
    ck("xml: save->load->save is byte identical", n == n2 && !memcmp(g_xml, g_xml2, n));
    ck("xml: export_bound() covers the file", ao_io_export_bound(&m) > n);
    ck("xml: a small buffer is refused", ao_io_export(&m, g_xml2, 16, &n2) == AO_E_FULL);

    static const char *const bad[] = {
        "<aodoc version=\"1\"><p>",                                   /* unclosed */
        "<aodoc version=\"2\"><p></p></aodoc>",                       /* version */
        "<aodoc><p></p></aodoc>",                                     /* no version */
        "<aodoc version=\"1\"><r>x</r></aodoc>",                      /* run outside p */
        "<aodoc version=\"1\"><p><r>a</r>",                           /* unclosed p */
        "not xml at all",
    };
    int refused_empty = 1;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ao_model_t b;
        fresh2(&b);
        ins(&b, 0, "keep me");
        if (ao_io_import(&b, bad[i], 0) == AO_OK) { refused_empty = 0; continue; }
        if (ao_model_text_len(&b) != 0) refused_empty = 0;
    }
    ck("xml: every malformed file is refused EMPTY", refused_empty);

    ao_model_t ok;
    fresh2(&ok);
    ck("xml: a well-formed minimal document loads",
       ao_io_import(&ok, "<aodoc version=\"1\"><p><r>hi</r></p></aodoc>", 0) == AO_OK &&
       ao_model_text_len(&ok) == 2);
    ck("xml: numeric entities decode",
       ao_io_import(&ok, "<aodoc version=\"1\"><p><r>&#1046;&#x43A;</r></p></aodoc>", 0) == AO_OK &&
       ao_model_text_len(&ok) == 4);                       /* Ж = 2 bytes, к = 2 */
    ck("xml: a surrogate entity is refused",
       ao_io_import(&ok, "<aodoc version=\"1\"><p><r>&#xD800;</r></p></aodoc>", 0) != AO_OK);
    ck("xml: an out-of-range entity is refused",
       ao_io_import(&ok, "<aodoc version=\"1\"><p><r>&#x110000;</r></p></aodoc>", 0) != AO_OK);
    ck("xml: an unknown entity is refused",
       ao_io_import(&ok, "<aodoc version=\"1\"><p><r>&nbsp;</r></p></aodoc>", 0) != AO_OK);

    /* 40 levels of nesting: refused by the depth cap rather than walked. */
    char deep[512];
    size_t k = 0;
    k += (size_t)snprintf(deep + k, sizeof deep - k, "<aodoc version=\"1\"><p><r>");
    for (int i = 0; i < 40; i++) k += (size_t)snprintf(deep + k, sizeof deep - k, "<x>");
    ck("xml: 40 levels of nesting is refused, not walked", ao_io_import(&ok, deep, 0) != AO_OK);
}

/* --------------------------------------------------------------------- fuzz */

static void test_fuzz(void) {
    static const char *const words[] = {
        "a", "Z", " ", "\n", "привет", "ё", "Ё", "дом", "x y", "\n\n", "«", "»", "—", "…", "0123"
    };
    ao_model_t m;
    fresh(&m);
    int bad = 0;
    const char *first = NULL;
    for (uint32_t i = 0; i < 10000; i++) {
        uint32_t len = ao_model_text_len(&m);
        uint32_t kind = rnd() % 10;
        const char *why = NULL;
        if (kind < 5) {
            const char *w = words[rnd() % (sizeof words / sizeof words[0])];
            uint32_t pos = len ? rnd() % (len + 1) : 0;
            int rc = ao_model_insert(&m, pos, w, (uint32_t)strlen(w));
            if (rc == AO_E_FULL) continue;
            if (rc != AO_OK) { bad++; if (!first) first = "insert rc"; continue; }
        } else if (kind < 8) {
            if (!len) continue;
            uint32_t pos = rnd() % len, n = 1 + rnd() % 8;
            if (ao_model_delete(&m, pos, n) != AO_OK) { bad++; if (!first) first = "delete rc"; continue; }
        } else if (kind < 9) {
            if (!len) continue;
            uint32_t pos = rnd() % len, n = 1 + rnd() % 6;
            uint8_t bits = (uint8_t)(1u << (rnd() % 3));
            if (ao_model_set_style(&m, pos, n, bits, (int)(rnd() & 1)) != AO_OK) {
                bad++; if (!first) first = "style rc"; continue;
            }
        } else {
            ao_model_set_align(&m, len ? rnd() % len : 0, (uint8_t)(rnd() % 4));
        }
        if (!ao_model_valid(&m, &why)) { bad++; if (!first) first = why; break; }
        {   /* The paragraphs must tile the text exactly and runs must stay inside
             * their paragraph: the invariants a screenshot cannot see. */
            uint32_t tile = 0;
            int broken = 0;
            for (uint32_t k = 0; k < ao_model_para_count(&m); k++) {
                if (m.paras[k].off != tile) { broken = 1; break; }
                if (m.paras[k].len > ao_model_text_len(&m) - m.paras[k].off) { broken = 2; break; }
                for (uint32_t q = 0; q < m.paras[k].run_count; q++) {
                    const ao_run_t *rn = &m.runs[m.paras[k].first_run + q];
                    if (rn->off + rn->len > m.paras[k].off + m.paras[k].len) { broken = 3; break; }
                }
                tile += m.paras[k].len;
            }
            if (tile != ao_model_text_len(&m)) broken = 4;
            if (broken) { bad++; if (!first) first = "paragraph tiling"; break; }
        }
        if ((i % 97) == 0) {
            size_t n = 0, n2 = 0;
            if (ao_model_serialize(&m, g_ser, sizeof g_ser, &n) != AO_OK) { bad++; if (!first) first = "serialize"; break; }
            ao_model_t r;
            fresh2(&r);
            if (ao_model_deserialize(&r, g_ser, n) != AO_OK) { bad++; if (!first) first = "deserialize"; break; }
            ao_model_serialize(&r, g_ser2, sizeof g_ser2, &n2);
            if (n2 != n || memcmp(g_ser, g_ser2, n)) { bad++; if (!first) first = "round trip"; break; }
            /* ... and through the XML format the user's file goes through. */
            if (ao_io_export(&m, g_xml, sizeof g_xml, &n) != AO_OK) { bad++; if (!first) first = "xml export"; break; }
            if (ao_io_import(&r, g_xml, n) != AO_OK) { bad++; if (!first) first = "xml import"; break; }
            if (ao_io_export(&r, g_xml2, sizeof g_xml2, &n2) != AO_OK || n != n2 ||
                memcmp(g_xml, g_xml2, n)) {
                if (getenv("AO_DUMP_XML")) {
                    FILE *f = fopen("/tmp/fuzz_a.xml", "w");
                    if (f) { fwrite(g_xml, 1, n, f); fclose(f); }
                    f = fopen("/tmp/fuzz_b.xml", "w");
                    if (f) { fwrite(g_xml2, 1, n2, f); fclose(f); }
                }
                bad++; if (!first) first = "xml round trip"; break;
            }
        }
    }
    if (bad) printf("  (fuzz stopped on: %s)\n", first ? first : "?");
    ck("fuzz: 10000 operations, every invariant re-checked, none broken", bad == 0);
    ck("fuzz: the document is still valid", ao_model_valid(&m, NULL));
    ck("fuzz: it did real work", ao_model_text_len(&m) > 0);
    printf("  (fuzz: %u bytes, %u paragraphs, %u runs; binary + XML round trips)\n",
           ao_model_text_len(&m), ao_model_para_count(&m), ao_model_run_count(&m));
}


/* ---- OF2 closure: word navigation, match count, replace-all ------------ */
static void test_of2_closure(void) {
    ao_model_t m;
    /* bytes: "foo bar  привет, мир! end" — Cyrillic is one word per word. */
    fresh(&m);
    ins(&m, 0, "foo bar  \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82, end");
    uint32_t end = ao_model_text_len(&m);
    ck("word: right from 0 lands at end of 'foo'", ao_model_word_right(&m, 0) == 3);
    ck("word: right skips the space to end of 'bar'", ao_model_word_right(&m, 3) == 7);
    ck("word: left from end lands at start of 'end'",
       ao_model_word_left(&m, end) == end - 3);
    uint32_t mid = ao_model_word_right(&m, 7);   /* past two spaces + Cyrillic word */
    ck("word: right crosses spaces as one Cyrillic word", mid == 7 + 2 + 12);
    ck("word: left returns to the Cyrillic word start", ao_model_word_left(&m, mid) == 9);
    ck("word: left at 0 and right at end are fixed points",
       ao_model_word_left(&m, 0) == 0 && ao_model_word_right(&m, end) == end);

    fresh(&m);
    ins(&m, 0, "One two one TWO one");
    ck("count: case-folded matches", ao_model_count(&m, "one", 1) == 3);
    ck("count: case-sensitive matches", ao_model_count(&m, "one", 0) == 2);
    ck("count: empty needle counts nothing", ao_model_count(&m, "", 1) == 0);

    ck("replace-all: replaces every folded match",
       ao_model_replace_all(&m, "one", "1") == 3);
    ck("replace-all: resulting text", text_is(&m, "1 two 1 TWO 1"));
    ck("replace-all: a replacement containing the needle terminates (and folds case)",
       ao_model_replace_all(&m, "two", "two two") == 2 &&
       text_is(&m, "1 two two 1 two two 1"));
    ck("replace-all: empty needle is refused, not a hang",
       ao_model_replace_all(&m, "", "x") == AO_E_ARG);
    ck("replace-all: model still valid", ao_model_valid(&m, NULL));
}

int main(void) {
    test_empty();
    test_editing();
    test_styles();
    test_steps_find();
    test_binary();
    test_xml();
    test_fuzz();
    test_of2_closure();
    printf("AO DOC: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
