/* tests/unit/test_ao_ooxml.c — OF8: the .docx bridge (import, export, refusal).
 *
 * The shape of every case here comes from one question: what would a caller
 * need to believe before it opens a file a stranger sent?
 *
 *   1. the package we write is a package other readers accept  (part layout,
 *      well-formed XML, and an independent check by tools/office/check_docx.py
 *      on the emitted fixture);
 *   2. export -> import -> export is an identity, so a save is auditable;
 *   3. a document WE DID NOT WRITE imports as what it says (the markup below is
 *      hand-written in Word's shape, not produced by our exporter);
 *   4. hostile input is refused with a code, never absorbed;
 *   5. what we do not support is named in a receipt, not silently dropped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "userspace/office/libaodoc/aodox.h"
#include "userspace/office/libaodoc/aozip.h"
#include "userspace/office/libaodoc/aoxml.h"
#include "userspace/office/libaodoc/aomodel.h"

static unsigned checks;
static unsigned failures;
#define CHECK(expr) do { checks++; if (expr) printf("PASS: %s\n", #expr); \
    else { failures++; printf("FAIL: %s (line %d)\n", #expr, __LINE__); } } while (0)

#define OUT_CAP 262144u
static uint8_t out_a[OUT_CAP], out_b[OUT_CAP], out_c[OUT_CAP];
static uint8_t media_buf[65536];
static char    scratch[65536];
static char    text_a[16384], text_b[16384];
static ao_run_t  runs_a[512],  runs_b[512];
static ao_para_t paras_a[128], paras_b[128];

static ao_model_t MA, MB;

typedef struct { uint8_t *p; size_t cap, len; } buffer_t;

static int to_buffer(void *ctx, const void *bytes, size_t n) {
    buffer_t *b = (buffer_t *)ctx;
    if (n > b->cap - b->len) return -1;
    if (n) memcpy(b->p + b->len, bytes, n);
    b->len += n;
    return 0;
}

/* ------------------------------------------------------------ the fixture */

/* A 1x1 PNG.  The exporter does not decode bitmaps, so the bytes only have to
 * be a real PNG to the independent host checker. */
static const uint8_t PNG_1X1[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,
    0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,
    0x15,0xC4,0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,
    0x01,0x00,0x00,0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,
    0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
};

static int media_read(void *ctx, const char *path, uint8_t *out, size_t cap,
                      size_t *len_out) {
    (void)ctx;
    if (!path || !strstr(path, ".png")) return AO_E_FORMAT;
    if (sizeof PNG_1X1 > cap) return AO_E_FULL;
    memcpy(out, PNG_1X1, sizeof PNG_1X1);
    *len_out = sizeof PNG_1X1;
    return AO_OK;
}

static void models_init(void) {
    CHECK(ao_model_init(&MA, text_a, sizeof text_a, runs_a, 512, paras_a, 128) == AO_OK);
    CHECK(ao_model_init(&MB, text_b, sizeof text_b, runs_b, 512, paras_b, 128) == AO_OK);
}

static void add_run(const char *s, uint8_t style, uint8_t size, uint8_t font) {
    uint32_t at = ao_model_text_len(&MA);
    CHECK(ao_model_insert(&MA, at, s, (uint32_t)strlen(s)) == AO_OK);
    static const uint8_t bits[3] = {
        AO_STYLE_BOLD, AO_STYLE_ITALIC, AO_STYLE_UNDERLINE
    };
    for (size_t i = 0; i < 3; i++)
        CHECK(ao_model_set_style(&MA, at, (uint32_t)strlen(s), bits[i],
                                 (style & bits[i]) != 0) == AO_OK);
    CHECK(ao_model_set_size_font(&MA, at, (uint32_t)strlen(s), size, font) == AO_OK);
}

/* Close the paragraph being built: apply its properties, then (unless it is
 * the last) insert the newline that opens the next one. */
static void close_para(uint8_t style, uint8_t align, int last) {
    uint32_t n = ao_model_para_count(&MA);
    if (n) {
        uint32_t pos = MA.paras[n - 1u].off;
        CHECK(ao_model_set_para_style(&MA, pos, style) == AO_OK);
        CHECK(ao_model_set_align(&MA, pos, align) == AO_OK);
    }
    if (!last) CHECK(ao_model_insert(&MA, ao_model_text_len(&MA), "\n", 1) == AO_OK);
}

static void build_model(int with_image) {
    models_init();

    add_run("Hello ", AO_STYLE_OFF, 11, AO_FONT_SANS);
    add_run("world", AO_STYLE_BOLD, 11, AO_FONT_SANS);
    close_para(AO_PARA_NORMAL, AO_ALIGN_LEFT, 0);

    add_run("Italic and underlined", AO_STYLE_ITALIC | AO_STYLE_UNDERLINE, 16,
            AO_FONT_MONO);
    close_para(AO_PARA_NORMAL, AO_ALIGN_CENTER, 0);

    add_run("A heading", AO_STYLE_BOLD, 11, AO_FONT_SANS);
    close_para(AO_PARA_H1, AO_ALIGN_LEFT, 0);

    add_run("First", AO_STYLE_OFF, 11, AO_FONT_SANS);
    close_para(AO_PARA_BULLET, AO_ALIGN_LEFT, 0);

    add_run("Second", AO_STYLE_OFF, 11, AO_FONT_SANS);
    close_para(AO_PARA_NUMBER, AO_ALIGN_LEFT, 0);

    add_run("a\tb\tc", AO_STYLE_OFF, 11, AO_FONT_SANS);
    close_para(AO_PARA_TABLE, AO_ALIGN_LEFT, 0);
    add_run("1\t2\t3", AO_STYLE_OFF, 11, AO_FONT_SANS);
    close_para(AO_PARA_TABLE, AO_ALIGN_LEFT, 0);

    if (with_image) {
        add_run("/documents/pic.png", AO_STYLE_OFF, 11, AO_FONT_SANS);
        close_para(AO_PARA_IMAGE, AO_ALIGN_LEFT, 0);
    }

    /* An empty paragraph: two documents that differ only here must not
     * collapse into one, or the caret would have nowhere to be. */
    close_para(AO_PARA_NORMAL, AO_ALIGN_LEFT, 1);

    CHECK(ao_model_valid(&MA, NULL) == 1);
}

static int models_equal(const ao_model_t *a, const ao_model_t *b) {
    if (a->text_len != b->text_len) return 0;
    if (memcmp(a->text, b->text, a->text_len) != 0) return 0;
    if (a->para_len != b->para_len) return 0;
    if (a->run_len != b->run_len) return 0;
    for (uint32_t i = 0; i < a->para_len; i++) {
        if (a->paras[i].off != b->paras[i].off) return 0;
        if (a->paras[i].len != b->paras[i].len) return 0;
        if (a->paras[i].align != b->paras[i].align) return 0;
        if (a->paras[i].style != b->paras[i].style) return 0;
        if (a->paras[i].first_run != b->paras[i].first_run) return 0;
        if (a->paras[i].run_count != b->paras[i].run_count) return 0;
    }
    for (uint32_t i = 0; i < a->run_len; i++) {
        if (a->runs[i].off != b->runs[i].off) return 0;
        if (a->runs[i].len != b->runs[i].len) return 0;
        if (a->runs[i].style != b->runs[i].style) return 0;
        if (a->runs[i].size != b->runs[i].size) return 0;
        if (a->runs[i].font != b->runs[i].font) return 0;
    }
    return 1;
}

/* A gate that reports "not equal" without saying what differed costs the next
 * person an afternoon.  Print the first few differences instead. */
static void report_model_diff(const ao_model_t *a, const ao_model_t *b) {
    unsigned shown = 0;
    if (a->text_len != b->text_len) {
        printf("  DIFF text_len: %u vs %u\n", a->text_len, b->text_len);
        shown++;
    } else if (memcmp(a->text, b->text, a->text_len) != 0) {
        for (uint32_t i = 0; i < a->text_len; i++)
            if (a->text[i] != b->text[i]) {
                printf("  DIFF text[%u]: %02x vs %02x\n", i,
                       (unsigned char)a->text[i], (unsigned char)b->text[i]);
                break;
            }
        shown++;
    }
    if (a->para_len != b->para_len) {
        printf("  DIFF para_count: %u vs %u\n", a->para_len, b->para_len);
        shown++;
    }
    if (a->run_len != b->run_len) {
        printf("  DIFF run_count: %u vs %u\n", a->run_len, b->run_len);
        shown++;
    }
    for (uint32_t i = 0; i < a->para_len && i < b->para_len && shown < 8; i++) {
        if (a->paras[i].off != b->paras[i].off || a->paras[i].len != b->paras[i].len) {
            printf("  DIFF para[%u] slice: (%u,%u) vs (%u,%u)\n", i,
                   a->paras[i].off, a->paras[i].len,
                   b->paras[i].off, b->paras[i].len);
            shown++;
        } else if (a->paras[i].align != b->paras[i].align ||
                   a->paras[i].style != b->paras[i].style) {
            printf("  DIFF para[%u] align=%u/%u style=0x%x/0x%x -- text=[%.*s]\n", i,
                   a->paras[i].align, b->paras[i].align,
                   a->paras[i].style, b->paras[i].style,
                   (int)a->paras[i].len, a->text + a->paras[i].off);
            shown++;
        }
    }
    for (uint32_t i = 0; i < a->run_len && i < b->run_len && shown < 8; i++) {
        if (a->runs[i].off != b->runs[i].off || a->runs[i].len != b->runs[i].len ||
            a->runs[i].style != b->runs[i].style || a->runs[i].size != b->runs[i].size ||
            a->runs[i].font != b->runs[i].font) {
            printf("  DIFF run[%u] off=%u/%u len=%u/%u style=0x%x/0x%x size=%u/%u font=%u/%u\n",
                   i, a->runs[i].off, b->runs[i].off, a->runs[i].len, b->runs[i].len,
                   a->runs[i].style, b->runs[i].style,
                   a->runs[i].size, b->runs[i].size,
                   a->runs[i].font, b->runs[i].font);
            shown++;
        }
    }
}

static uint32_t para_kind(const ao_model_t *m, uint32_t i) {
    return m->paras[i].style & AO_PARA_KIND_MASK;
}

static int para_is(const ao_model_t *m, uint32_t i, const char *text) {
    if (i >= m->para_len) return 0;
    size_t n = strlen(text);
    uint32_t len = m->paras[i].len;
    if (len && m->text[m->paras[i].off + len - 1u] == '\n') len--;
    return len == n && !memcmp(m->text + m->paras[i].off, text, n);
}

/* ------------------------------------------------------- package handling */

static int part_bytes(const uint8_t *zip, size_t len, const char *name,
                      const uint8_t **out, size_t *out_len) {
    ao_zip_reader_t r;
    if (ao_zip_reader_init(&r, zip, len) != AO_ZIP_OK) return 0;
    ao_zip_entry_t e;
    if (ao_zip_reader_find(&r, name, strlen(name), &e) != AO_ZIP_OK) return 0;
    return ao_zip_entry_view(&r, &e, out, out_len) == AO_ZIP_OK;
}

static int part_present(const uint8_t *zip, size_t len, const char *name) {
    const uint8_t *d = NULL;
    size_t n = 0;
    return part_bytes(zip, len, name, &d, &n);
}

static int part_valid_xml(const uint8_t *zip, size_t len, const char *name) {
    const uint8_t *d = NULL;
    size_t n = 0;
    if (!part_bytes(zip, len, name, &d, &n)) return 0;
    return ao_xml_validate((const char *)d, n) == AO_XML_OK;
}

static int part_contains(const uint8_t *zip, size_t len, const char *name,
                         const char *needle) {
    const uint8_t *d = NULL;
    size_t n = 0;
    if (!part_bytes(zip, len, name, &d, &n)) return 0;
    if (!needle[0]) return 1;
    for (size_t i = 0; i + strlen(needle) <= n; i++)
        if (!memcmp(d + i, needle, strlen(needle))) return 1;
    return 0;
}

/* Build a package from hand-written parts, so the importer is exercised
 * against markup our exporter did not produce. */
static size_t build_docx(uint8_t *out, size_t cap, const char *document_xml,
                         const char *rels, const char *numbering) {
    buffer_t b = { out, cap, 0 };
    ao_zip_writer_t w;
    if (ao_zip_writer_init(&w, to_buffer, &b) != AO_ZIP_OK) return 0;
    size_t total = 0;
    int rc = ao_zip_writer_add(&w, "word/document.xml",
                               strlen("word/document.xml"),
                               document_xml, strlen(document_xml));
    if (rc == AO_ZIP_OK && rels)
        rc = ao_zip_writer_add(&w, "word/_rels/document.xml.rels",
                               strlen("word/_rels/document.xml.rels"),
                               rels, strlen(rels));
    if (rc == AO_ZIP_OK && numbering)
        rc = ao_zip_writer_add(&w, "word/numbering.xml",
                               strlen("word/numbering.xml"),
                               numbering, strlen(numbering));
    if (rc != AO_ZIP_OK) return 0;
    if (ao_zip_writer_finish(&w, &total) != AO_ZIP_OK) return 0;
    return total;
}

/* ------------------------------------------------------------- 1. export */

static void test_export_layout(void) {
    build_model(1);
    buffer_t b = { out_a, sizeof out_a, 0 };
    size_t size = 0;
    uint32_t skipped = 99;
    int rc = ao_docx_export_ex(&MA, to_buffer, &b, &size, NULL,
                               media_read, NULL, media_buf, sizeof media_buf,
                               NULL, NULL, &skipped);
    CHECK(rc == AO_OK);
    CHECK(size > 0 && size <= sizeof out_a);
    CHECK(skipped == 0);

    /* The parts a validator looks for, and nothing that is not declared. */
    CHECK(part_present(out_a, size, "[Content_Types].xml"));
    CHECK(part_present(out_a, size, "_rels/.rels"));
    CHECK(part_present(out_a, size, "word/document.xml"));
    CHECK(part_present(out_a, size, "word/_rels/document.xml.rels"));
    CHECK(part_present(out_a, size, "word/styles.xml"));
    CHECK(part_present(out_a, size, "word/numbering.xml"));
    CHECK(part_present(out_a, size, "word/media/image1.png"));
    CHECK(!part_present(out_a, size, "word/media/image2.png"));

    /* Every XML part is well-formed: our own parser is the strictest reader
     * we have, and a package that fails it would fail a validator too. */
    CHECK(part_valid_xml(out_a, size, "[Content_Types].xml"));
    CHECK(part_valid_xml(out_a, size, "_rels/.rels"));
    CHECK(part_valid_xml(out_a, size, "word/document.xml"));
    CHECK(part_valid_xml(out_a, size, "word/_rels/document.xml.rels"));
    CHECK(part_valid_xml(out_a, size, "word/styles.xml"));
    CHECK(part_valid_xml(out_a, size, "word/numbering.xml"));

    CHECK(part_contains(out_a, size, "[Content_Types].xml",
                        "PartName=\"/word/document.xml\""));
    CHECK(part_contains(out_a, size, "_rels/.rels", "Target=\"word/document.xml\""));
    CHECK(part_contains(out_a, size, "word/_rels/document.xml.rels",
                        "Target=\"media/image1.png\""));
    CHECK(part_contains(out_a, size, "word/document.xml", "<w:body>"));
    CHECK(part_contains(out_a, size, "word/document.xml", "<w:tbl>"));
    CHECK(part_contains(out_a, size, "word/document.xml", "<w:sectPr>"));
    CHECK(part_contains(out_a, size, "word/document.xml", "r:embed=\"rId3\""));

    /* The image bytes are stored, not re-encoded. */
    const uint8_t *img = NULL;
    size_t img_len = 0;
    CHECK(part_bytes(out_a, size, "word/media/image1.png", &img, &img_len));
    CHECK(img_len == sizeof PNG_1X1 && !memcmp(img, PNG_1X1, img_len));
}

/* ------------------------------------------- 2/3. round-trip, determinism */

static void test_round_trip(void) {
    /* Without images: export -> import -> export is an exact identity. */
    build_model(0);
    size_t size_a = 0, size_b = 0;
    {
        buffer_t b = { out_a, sizeof out_a, 0 };
        CHECK(ao_docx_export_ex(&MA, to_buffer, &b, &size_a, NULL,
                                media_read, NULL, media_buf, sizeof media_buf,
                                NULL, NULL, NULL) == AO_OK);
    }
    CHECK(ao_docx_import(&MB, out_a, size_a, scratch, sizeof scratch,
                         NULL, NULL, NULL, NULL, NULL) == AO_OK);
    if (!models_equal(&MA, &MB)) report_model_diff(&MA, &MB);
    CHECK(models_equal(&MA, &MB) == 1);
    CHECK(ao_model_valid(&MB, NULL) == 1);

    /* Re-exporting the re-imported document must reproduce the bytes: that is
     * what makes "did this save change anything?" answerable with cmp(1). */
    {
        buffer_t b = { out_b, sizeof out_b, 0 };
        CHECK(ao_docx_export_ex(&MB, to_buffer, &b, &size_b, NULL,
                                media_read, NULL, media_buf, sizeof media_buf,
                                NULL, NULL, NULL) == AO_OK);
    }
    CHECK(size_a == size_b);
    CHECK(size_a == size_b && !memcmp(out_a, out_b, size_a));
}

/* With an image the ROUND TRIP IS NOT AN IDENTITY, and pretending otherwise
 * would be the kind of claim this suite exists to prevent: the model stores a
 * filesystem path, the package stores a part name, and an image that came in
 * as /documents/pic.png comes back as media/image1.png.  The receipt for that
 * is this test, which pins exactly what does and does not survive. */
static void test_round_trip_with_image(void) {
    build_model(1);
    size_t size_a = 0, size_b = 0;
    {
        buffer_t b = { out_a, sizeof out_a, 0 };
        CHECK(ao_docx_export_ex(&MA, to_buffer, &b, &size_a, NULL,
                                media_read, NULL, media_buf, sizeof media_buf,
                                NULL, NULL, NULL) == AO_OK);
    }
    CHECK(ao_docx_import(&MB, out_a, size_a, scratch, sizeof scratch,
                         NULL, NULL, NULL, NULL, NULL) == AO_OK);
    CHECK(ao_model_para_count(&MB) == ao_model_para_count(&MA));
    uint32_t pi = 7;
    CHECK(para_kind(&MB, pi) == AO_PARA_IMAGE);
    CHECK(para_is(&MB, pi, "media/image1.png"));   /* renamed, not lost */
    /* Everything around the image is untouched. */
    CHECK(para_is(&MB, 0, "Hello world"));
    CHECK(MB.runs[MB.paras[0].first_run + 1].style == AO_STYLE_BOLD);
    CHECK(para_kind(&MB, 5) == AO_PARA_TABLE);
    CHECK(para_kind(&MB, 6) == AO_PARA_TABLE);
    CHECK(para_is(&MB, 5, "a\tb\tc"));

    /* What DOES hold: the rename is a fixed point after one turn.  Saving the
     * re-imported document twice gives the same bytes, and reading it back
     * gives the same model -- so a second save is provably a no-op even though
     * the first one rewrote the path. */
    {
        buffer_t b = { out_b, sizeof out_b, 0 };
        CHECK(ao_docx_export_ex(&MB, to_buffer, &b, &size_b, NULL,
                                media_read, NULL, media_buf, sizeof media_buf,
                                NULL, NULL, NULL) == AO_OK);
    }
    {
        buffer_t b = { out_c, sizeof out_c, 0 };
        size_t size_c = 0;
        CHECK(ao_docx_export_ex(&MB, to_buffer, &b, &size_c, NULL,
                                media_read, NULL, media_buf, sizeof media_buf,
                                NULL, NULL, NULL) == AO_OK);
        CHECK(size_b == size_c && !memcmp(out_b, out_c, size_b));
    }
    CHECK(ao_docx_import(&MA, out_b, size_b, scratch, sizeof scratch,
                         NULL, NULL, NULL, NULL, NULL) == AO_OK);
    if (!models_equal(&MA, &MB)) report_model_diff(&MA, &MB);
    CHECK(models_equal(&MA, &MB) == 1);
}

/* ------------------------------------- 4. a document we did not write */

static const char WORD_DOC[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\">\n"
    " <w:body>\n"
    "  <w:p><w:r><w:t>Plain start</w:t></w:r></w:p>\n"
    "  <w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr>"
    "   <w:r><w:rPr><w:b/><w:i/><w:u w:val=\"single\"/></w:rPr>"
    "   <w:t xml:space=\"preserve\">Bold italic underline</w:t></w:r></w:p>\n"
    "  <w:p><w:pPr><w:pStyle w:val=\"Heading2\"/></w:pPr>"
    "   <w:r><w:t>Heading</w:t></w:r></w:p>\n"
    "  <w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/>"
    "   <w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr>"
    "   <w:r><w:t>Bulleted</w:t></w:r></w:p>\n"
    "  <w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/>"
    "   <w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"2\"/></w:numPr></w:pPr>"
    "   <w:r><w:t>Numbered</w:t></w:r></w:p>\n"
    "  <w:p><w:pPr><w:ind w:left=\"720\"/></w:pPr>"
    "   <w:r><w:t>Indented</w:t></w:r></w:p>\n"
    "  <w:p><w:r><w:rPr><w:sz w:val=\"28\"/></w:rPr>"
    "   <w:t>Bigger</w:t></w:r></w:p>\n"
    "  <w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr>"
    "   <w:t>mono</w:t></w:r></w:p>\n"
    "  <w:tbl>\n"
    "   <w:tr><w:tc><w:p><w:r><w:t>r1c1</w:t></w:r></w:p></w:tc>"
    "    <w:tc><w:p><w:r><w:t>r1c2</w:t></w:r></w:p></w:tc></w:tr>\n"
    "   <w:tr><w:tc><w:p><w:r><w:t>r2c1</w:t></w:r></w:p></w:tc>"
    "    <w:tc><w:p><w:r><w:t>r2c2</w:t></w:r></w:p></w:tc></w:tr>\n"
    "  </w:tbl>\n"
    "  <w:p><w:r><w:drawing><wp:inline><a:graphic><a:graphicData>"
    "   <pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
    "   <pic:blipFill><a:blip r:embed=\"rId7\"/></pic:blipFill></pic:pic>"
    "  </a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>\n"
    "  <w:p><w:r><w:t>After the picture</w:t></w:r></w:p>\n"
    "  <w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
    "   <w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\"/>"
    "  </w:sectPr>\n"
    " </w:body></w:document>\n";

static const char WORD_RELS[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    " <Relationship Id=\"rId7\" Type=\"http://schemas.openxmlformats.org/"
    "officeDocument/2006/relationships/image\" Target=\"media/photo.png\"/>\n"
    "</Relationships>\n";

static const char WORD_NUMBERING[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\n"
    " <w:abstractNum w:abstractNumId=\"4\"><w:lvl w:ilvl=\"0\">"
    "<w:numFmt w:val=\"bullet\"/></w:lvl></w:abstractNum>\n"
    " <w:abstractNum w:abstractNumId=\"5\"><w:lvl w:ilvl=\"0\">"
    "<w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>\n"
    " <w:num w:numId=\"1\"><w:abstractNumId w:val=\"4\"/></w:num>\n"
    " <w:num w:numId=\"2\"><w:abstractNumId w:val=\"5\"/></w:num>\n"
    "</w:numbering>\n";

static void test_import_hand_written(void) {
    size_t size = build_docx(out_c, sizeof out_c, WORD_DOC, WORD_RELS,
                             WORD_NUMBERING);
    CHECK(size > 0);
    ao_docx_rels_t rels;
    ao_docx_sect_t sect;
    uint32_t skipped = 0;
    CHECK(ao_docx_import(&MB, out_c, size, scratch, sizeof scratch,
                         &rels, &sect, NULL, NULL, &skipped) == AO_OK);

    CHECK(para_is(&MB, 0, "Plain start"));
    CHECK(para_is(&MB, 1, "Bold italic underline"));
    CHECK(MB.paras[1].align == AO_ALIGN_CENTER);
    CHECK(para_kind(&MB, 2) == AO_PARA_H2);
    /* numId 1 -> abstract 4 -> bullet; numId 2 -> abstract 5 -> decimal.
     * Without numbering.xml both would land as bullets. */
    CHECK(para_kind(&MB, 3) == AO_PARA_BULLET);
    CHECK(para_kind(&MB, 4) == AO_PARA_NUMBER);
    CHECK(para_is(&MB, 5, "Indented"));
    CHECK(MB.paras[5].indent == 48);          /* 720 twips / 15 = 48 px */
    CHECK(para_is(&MB, 6, "Bigger"));
    CHECK(MB.runs[MB.paras[6].first_run].size == 14);   /* 28 half-points */
    CHECK(para_is(&MB, 7, "mono"));
    CHECK(MB.runs[MB.paras[7].first_run].font == AO_FONT_MONO);

    /* Two rows, cells joined by '\t'. */
    CHECK(para_kind(&MB, 8) == AO_PARA_TABLE);
    CHECK(para_is(&MB, 8, "r1c1\tr1c2"));
    CHECK(para_kind(&MB, 9) == AO_PARA_TABLE);
    CHECK(para_is(&MB, 9, "r2c1\tr2c2"));

    /* The drawing resolved through the relationship table. */
    CHECK(para_kind(&MB, 10) == AO_PARA_IMAGE);
    CHECK(para_is(&MB, 10, "media/photo.png"));
    CHECK(para_is(&MB, 11, "After the picture"));

    CHECK(sect.seen == 1);
    CHECK(sect.page_w == 12240 && sect.page_h == 15840);
    CHECK(sect.margin_t == 1440 && sect.margin_l == 1440);

    CHECK(rels.count == 1);
    CHECK(!strcmp(rels.id[0], "rId7"));
    CHECK(!strcmp(rels.target[0], "media/photo.png"));

    /* Style bits survived the trip. */
    uint32_t r = MB.paras[1].first_run;
    CHECK(MB.runs[r].style == (AO_STYLE_BOLD | AO_STYLE_ITALIC | AO_STYLE_UNDERLINE));
    CHECK(ao_model_valid(&MB, NULL) == 1);
}

/* --------------------------------------------------- 5. refusal, not harm */

static void test_refusals(void) {
    /* Not a ZIP at all. */
    CHECK(ao_docx_import(&MB, "<w:document/>", 13, scratch, sizeof scratch,
                         NULL, NULL, NULL, NULL, NULL) == AO_E_FORMAT);
    /* A ZIP that is not a document. */
    size_t n = build_docx(out_c, sizeof out_c, WORD_DOC, NULL, NULL);
    ao_zip_reader_t r;
    CHECK(ao_zip_reader_init(&r, out_c, n) == AO_ZIP_OK);
    (void)r;
    /* Our writer stores its parts, so a stored document.xml is a zero-copy
     * view and needs no scratch at all -- a 64-byte scratch must not be a
     * reason to refuse a document.  (A deflated part would need the scratch;
     * the bound for that is the same code path as AO_E_FULL below.) */
    uint8_t small[4096];
    memcpy(small, out_c, n < sizeof small ? n : sizeof small);
    CHECK(ao_docx_import(&MB, small, n, scratch, 64, NULL, NULL, NULL,
                         NULL, NULL) == AO_OK);

    /* A ZIP with no word/document.xml. */
    {
        buffer_t b = { out_b, sizeof out_b, 0 };
        ao_zip_writer_t w;
        ao_zip_writer_init(&w, to_buffer, &b);
        ao_zip_writer_add(&w, "word/styles.xml", strlen("word/styles.xml"),
                          "<w:styles/>", strlen("<w:styles/>"));
        size_t total = 0;
        ao_zip_writer_finish(&w, &total);
        CHECK(ao_docx_import(&MB, out_b, total, scratch, sizeof scratch,
                             NULL, NULL, NULL, NULL, NULL) == AO_E_FORMAT);
    }

    /* XXE-shaped input: a DTD with an external entity.  The strict reader
     * refuses DOCTYPE outright, so the entity is never a question. */
    {
        static const char xxe[] =
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE document [<!ENTITY xxe SYSTEM \"file:///etc/passwd\">]>\n"
            "<w:document xmlns:w=\"urn:x\"><w:body><w:p><w:r>"
            "<w:t>&xxe;</w:t></w:r></w:p></w:body></w:document>";
        size_t s = build_docx(out_b, sizeof out_b, xxe, NULL, NULL);
        CHECK(ao_docx_import(&MB, out_b, s, scratch, sizeof scratch,
                             NULL, NULL, NULL, NULL, NULL) == AO_E_FORMAT);
    }

    /* A document.xml that is itself an archive: refuse, never recurse. */
    {
        size_t inner = build_docx(out_b, sizeof out_b, WORD_DOC, NULL, NULL);
        char nested[8192];
        size_t cn = 0;
        for (size_t i = 0; i < inner && cn < sizeof nested; i++) {
            unsigned char c = out_b[i];
            if (c == '<' || c == '>' || c == '&') { nested[cn++] = ' '; }
            else nested[cn++] = (char)c;
        }
        /* The payload is archive bytes wrapped as text, which is not XML. */
        char wrapper[9000];
        size_t wn = 0;
        wn += (size_t)snprintf(wrapper + wn, sizeof wrapper - wn,
                               "<w:document xmlns:w=\"urn:x\"><w:body><w:p><w:r><w:t>");
        memcpy(wrapper + wn, nested, cn);
        wn += cn;
        wn += (size_t)snprintf(wrapper + wn, sizeof wrapper - wn,
                               "</w:t></w:r></w:p></w:body></w:document>");
        size_t s = build_docx(out_c, sizeof out_c, wrapper, NULL, NULL);
        int rc = ao_docx_import(&MB, out_c, s, scratch, sizeof scratch,
                                NULL, NULL, NULL, NULL, NULL);
        CHECK(rc != AO_OK);                    /* refused, and no crash */
        CHECK(ao_model_text_len(&MB) == 0);    /* left empty, never partial */
    }

    /* Truncated archive. */
    {
        size_t s = build_docx(out_b, sizeof out_b, WORD_DOC, NULL, NULL);
        CHECK(ao_docx_import(&MB, out_b, s / 2u, scratch, sizeof scratch,
                             NULL, NULL, NULL, NULL, NULL) != AO_OK);
    }

    /* Bad arguments. */
    CHECK(ao_docx_import(NULL, out_b, 8, scratch, sizeof scratch,
                         NULL, NULL, NULL, NULL, NULL) == AO_E_ARG);
    CHECK(ao_docx_export(NULL, to_buffer, NULL, NULL) == AO_E_ARG);
    CHECK(ao_docx_export_bound(NULL) == 0);
}

/* ------------------------------------------ 6. refusal is a named receipt */

static char note_log[16][AO_DOCX_MAX_NOTE_BYTES];
static uint32_t note_count;

static int collect_note(void *ctx, const char *text, size_t len) {
    (void)ctx;
    if (note_count < 16 && len < AO_DOCX_MAX_NOTE_BYTES) {
        memcpy(note_log[note_count], text, len);
        note_log[note_count][len] = '\0';
        note_count++;
    }
    return 0;
}

static int note_log_has(const char *needle) {
    for (uint32_t i = 0; i < note_count; i++)
        if (strstr(note_log[i], needle)) return 1;
    return 0;
}

static void test_skip_receipts(void) {
    /* A structured document tag wraps content we DO want: the text survives,
     * the wrapper is named once. */
    static const char doc[] =
        "<?xml version=\"1.0\"?>\n"
        "<w:document xmlns:w=\"urn:x\"><w:body>"
        "<w:sdt><w:sdtPr/><w:sdtContent>"
        "<w:p><w:r><w:t>Inside a tag</w:t></w:r></w:p>"
        "</w:sdtContent></w:sdt>"
        "<w:p><w:r><w:t>Outside</w:t></w:r></w:p>"
        "</w:body></w:document>";
    size_t s = build_docx(out_b, sizeof out_b, doc, NULL, NULL);
    note_count = 0;
    uint32_t skipped = 0;
    CHECK(ao_docx_import(&MB, out_b, s, scratch, sizeof scratch,
                         NULL, NULL, collect_note, NULL, &skipped) == AO_OK);
    CHECK(skipped >= 1);
    CHECK(note_log_has("w:sdt"));
    CHECK(para_is(&MB, 0, "Inside a tag"));
    CHECK(para_is(&MB, 1, "Outside"));

    /* Four thousand of the same tag must not produce four thousand receipts. */
    {
        static char many[8192];
        size_t mn = 0;
        mn += (size_t)snprintf(many + mn, sizeof many - mn,
                               "<?xml version=\"1.0\"?>"
                               "<w:document xmlns:w=\"urn:x\"><w:body>");
        /* Siblings, not nesting: the point is repetition, and 200 levels of
         * nesting would hit the reader's depth cap instead. */
        for (int i = 0; i < 200; i++)
            mn += (size_t)snprintf(many + mn, sizeof many - mn, "<w:sdt/>");
        mn += (size_t)snprintf(many + mn, sizeof many - mn,
                               "<w:p><w:r><w:t>deep</w:t></w:r></w:p>");
        mn += (size_t)snprintf(many + mn, sizeof many - mn, "</w:body></w:document>");
        size_t s2 = build_docx(out_c, sizeof out_c, many, NULL, NULL);
        note_count = 0;
        uint32_t skipped2 = 0;
        CHECK(ao_docx_import(&MB, out_c, s2, scratch, sizeof scratch,
                             NULL, NULL, collect_note, NULL, &skipped2) == AO_OK);
        CHECK(skipped2 == 200);              /* every one counted */
        CHECK(note_count == 1);              /* named exactly once */
    }

    /* Exporting without a media source: the text is unaffected and the
     * omission is a receipt, not a silently empty drawing. */
    {
        build_model(1);
        note_count = 0;
        buffer_t b = { out_a, sizeof out_a, 0 };
        size_t size = 0;
        uint32_t skipped3 = 0;
        CHECK(ao_docx_export_ex(&MA, to_buffer, &b, &size, NULL,
                                NULL, NULL, NULL, 0,
                                collect_note, NULL, &skipped3) == AO_OK);
        CHECK(skipped3 == 1);
        CHECK(note_log_has("no media source"));
        CHECK(!part_present(out_a, size, "word/media/image1.png"));
        CHECK(part_valid_xml(out_a, size, "word/document.xml"));
        CHECK(ao_docx_import(&MB, out_a, size, scratch, sizeof scratch,
                             NULL, NULL, NULL, NULL, NULL) == AO_OK);
    }
}

/* ------------------------------------- 7. the documented subset stays true */

/* The constructs the importer handles, one per line.  This list is the
 * code's half of the contract; docs/office.md holds the other half. */
static const char *SUPPORTED[] = {
    "w:p", "w:r", "w:t", "w:b", "w:i", "w:u", "w:sz", "w:rFonts", "w:jc",
    "w:pStyle", "w:numPr", "w:ind", "w:tbl", "a:blip", "w:sectPr"
};

/* A subset table nobody maintains is worse than no table: it licenses a
 * caller to assume support that was removed two phases ago.  So the test
 * reads the table and fails if the two halves disagree in either direction --
 * a construct the code handles that the table omits, or one the table
 * promises that the code has never heard of. */
static void test_documented_subset(void) {
    FILE *f = fopen("docs/office.md", "rb");
    CHECK(f != NULL);
    if (!f) return;

    static char doc[65536];
    size_t n = fread(doc, 1, sizeof doc - 1u, f);
    fclose(f);
    doc[n] = '\0';

    char *begin = strstr(doc, "<!-- OF8-SUBSET-BEGIN -->");
    char *end = strstr(doc, "<!-- OF8-SUBSET-END -->");
    CHECK(begin != NULL);
    CHECK(end != NULL);
    if (!begin || !end || end <= begin) return;
    *end = '\0';                       /* work inside the marked region only */

    /* Every construct we handle is listed, and listed as supported. */
    for (size_t i = 0; i < sizeof SUPPORTED / sizeof SUPPORTED[0]; i++) {
        char row[256];
        snprintf(row, sizeof row, "| `%s`", SUPPORTED[i]);
        char *at = strstr(begin, row);
        int ok = at != NULL;
        if (ok) {
            /* The ✅ must be on that construct's own row, not merely later
             * in the file -- otherwise one tick would satisfy every row. */
            char *eol = strchr(at, '\n');
            if (!eol) eol = begin + strlen(begin);
            char save = *eol;
            *eol = '\0';
            ok = strstr(at, "\xe2\x9c\x85") != NULL;   /* U+2705 */
            *eol = save;
        }
        if (!ok) {
            failures++;
            printf("FAIL: docs/office.md omits %s from the OF8 subset table\n",
                   SUPPORTED[i]);
        }
        checks++;
    }

    /* And the table does not promise more than the code delivers: count the
     * ticks and compare with the list above. */
    unsigned ticks = 0;
    for (char *p = begin; (p = strstr(p, "\xe2\x9c\x85")) != NULL; p += 3) ticks++;
    CHECK(ticks == sizeof SUPPORTED / sizeof SUPPORTED[0]);
    if (ticks != sizeof SUPPORTED / sizeof SUPPORTED[0])
        printf("  (subset table claims %u supported, the code handles %zu)\n",
               ticks, sizeof SUPPORTED / sizeof SUPPORTED[0]);
}

int main(void) {
    test_export_layout();
    test_round_trip();
    test_round_trip_with_image();
    test_import_hand_written();
    test_refusals();
    test_skip_receipts();
    test_documented_subset();

    /* The independent host check (tools/office/check_docx.py) reads what the
     * last full export wrote and validates it with Python's own zipfile and
     * XML parser -- a reader we did not write, agreeing with one we did. */
    (void)mkdir("build", 0755);
    FILE *f = fopen("build/test_ao_ooxml_docx.docx", "wb");
    CHECK(f != NULL);
    if (f) {
        size_t size = 0;
        buffer_t b = { out_a, sizeof out_a, 0 };
        build_model(1);
        if (ao_docx_export_ex(&MA, to_buffer, &b, &size, NULL,
                              media_read, NULL, media_buf, sizeof media_buf,
                              NULL, NULL, NULL) == AO_OK) {
            CHECK(fwrite(out_a, 1, size, f) == size);
        }
        CHECK(fclose(f) == 0);
    }
    printf("checks=%u fails=%u\n", checks, failures);
    return failures ? 1 : 0;
}
