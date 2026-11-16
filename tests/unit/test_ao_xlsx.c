/* tests/unit/test_ao_xlsx.c — OF8: the .xlsx bridge (import, export, refusal).
 *
 * The same five questions test_ao_ooxml.c asks about .docx, asked about a
 * workbook:
 *
 *   1. the package we write is one other readers accept (part layout and
 *      well-formed XML, checked here with our own strict reader and, on the
 *      emitted fixture, by tools/office/check_xlsx.py);
 *   2. export -> import -> export is an identity, so a save is auditable;
 *   3. a workbook WE DID NOT WRITE imports as what it says (the markup below
 *      is hand-written in Excel's shape, not produced by our exporter);
 *   4. hostile input is refused with a code, never absorbed;
 *   5. what we do not support is named in a receipt, not silently dropped —
 *      and merged cells are the interesting case, because refusing them is
 *      the only honest answer a model without spans can give.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "userspace/office/libaodoc/aoxlsx.h"
#include "userspace/office/libaodoc/aocalc.h"
#include "userspace/office/libaodoc/aodoc.h"
#include "userspace/office/libaodoc/aozip.h"
#include "userspace/office/libaodoc/aoxml.h"
#include "userspace/office/libaodoc/aomodel.h"

static unsigned checks;
static unsigned failures;
#define CHECK(expr) do { checks++; if (expr) printf("PASS: %s\n", #expr); \
    else { failures++; printf("FAIL: %s (line %d)\n", #expr, __LINE__); } } while (0)

#define OUT_CAP 262144u
static uint8_t out_a[OUT_CAP], out_b[OUT_CAP];
static char    scratch[65536];
static char    strbuf[32768];
static ao_xlsx_strings_t strings_a, strings_b;
static ao_xlsx_cols_t    cols_a, cols_b;

static uint8_t arena_mem_a[1u << 20];
static uint8_t arena_mem_b[1u << 20];
static ao_arena_t arena_a, arena_b;

typedef struct { uint8_t *p; size_t cap, len; } buffer_t;

static int to_buffer(void *ctx, const void *bytes, size_t n) {
    buffer_t *b = (buffer_t *)ctx;
    if (n > b->cap - b->len) return -1;
    if (n) memcpy(b->p + b->len, bytes, n);
    b->len += n;
    return 0;
}

/* --------------------------------------------------------- the workbooks */

static void fresh(void) {
    ao_arena_init(&arena_a, arena_mem_a, sizeof arena_mem_a);
    ao_arena_init(&arena_b, arena_mem_b, sizeof arena_mem_b);
}

/* A workbook with one of every cell kind we claim, and a second sheet so the
 * sheet list is exercised rather than assumed. */
static ao_book_t *build_book(ao_arena_t *arena) {
    ao_book_t *b = ao_book_new(arena);
    if (!b) return NULL;
    ao_sheet_t *s = ao_book_sheet(b, "Sheet1", 1);
    if (!s) return NULL;
    ao_sheet_set(b, s, 0, 0, "10");          /* A1 number            */
    ao_sheet_set(b, s, 0, 1, "20");          /* B1 number            */
    ao_sheet_set(b, s, 0, 2, "=SUM(A1:B1)"); /* C1 formula -> 30     */
    ao_sheet_set(b, s, 1, 0, "hello");       /* A2 text              */
    ao_sheet_set(b, s, 1, 1, "Привет");      /* B2 Cyrillic text     */
    ao_sheet_set(b, s, 1, 2, "TRUE");        /* C2 boolean           */
    ao_sheet_set(b, s, 2, 0, "3.5");         /* A3 fractional number */
    ao_sheet_t *t = ao_book_sheet(b, "Второй", 1);
    if (!t) return NULL;
    ao_sheet_set(b, t, 0, 0, "second");
    return b;
}

static int count_cells(ao_book_t *b, int sheet) {
    ao_sheet_t *s = ao_book_sheet_at(b, sheet);
    return s ? (int)ao_sheet_cell_count(s) : -1;
}

/* ------------------------------------------------------------- the cases */

static void test_round_trip(void) {
    printf("[unit] xlsx: export -> import -> export identity\n");
    fresh();
    ao_book_t *b = build_book(&arena_a);
    CHECK(b != NULL);
    if (!b) return;

    buffer_t out = { out_a, OUT_CAP, 0 };
    size_t size = 0;
    uint32_t skipped = 99;
    int rc = ao_xlsx_export_ex(b, NULL, to_buffer, &out, &size,
                               strbuf, sizeof strbuf, &strings_a,
                               NULL, NULL, &skipped);
    CHECK(rc == AO_OK);
    CHECK(size > 0 && size == out.len);
    CHECK(skipped == 0);
    printf("[unit]   exported %zu bytes, %u shared strings\n", size,
           strings_a.count);

    /* Determinism: the same workbook must produce the same bytes twice. */
    buffer_t out2 = { out_b, OUT_CAP, 0 };
    size_t size2 = 0;
    ao_arena_init(&arena_b, arena_mem_b, sizeof arena_mem_b);
    ao_book_t *b2 = build_book(&arena_b);
    ao_xlsx_strings_t str1;
    CHECK(ao_xlsx_export_ex(b2, NULL, to_buffer, &out2, &size2,
                            strbuf, sizeof strbuf, &str1,
                            NULL, NULL, NULL) == AO_OK);
    CHECK(size2 == size && out2.len == out.len &&
          memcmp(out_a, out_b, size) == 0);

    /* Re-import into a fresh book. */
    ao_book_t *in = ao_book_new(&arena_b);
    CHECK(in != NULL);
    ao_xlsx_strings_t str2;
    uint32_t skipped2 = 99;
    rc = ao_xlsx_import(in, out_a, size, scratch, sizeof scratch,
                        strbuf, sizeof strbuf, &str2, NULL, NULL, NULL,
                        &skipped2);
    CHECK(rc == AO_OK);
    CHECK(skipped2 == 0);
    CHECK(ao_book_sheet_count(in) == 2);
    CHECK(count_cells(in, 0) == 7);
    CHECK(count_cells(in, 1) == 1);

    /* The formula must come back as typed input, not as its cached value. */
    ao_sheet_t *s0 = ao_book_sheet_at(in, 0);
    ao_cell_t *c = s0 ? ao_sheet_cell(in, s0, 0, 2) : NULL;
    CHECK(c != NULL && ao_cell_input(c) && strcmp(ao_cell_input(c), "=SUM(A1:B1)") == 0);

    /* ...and the engine must still compute it. */
    if (s0) {
        ao_value_t v = ao_sheet_get(in, s0, 0, 2);
        CHECK(v.kind == AO_VAL_NUM && v.num == 30.0);
    }

    /* Cyrillic survived as text, and the boolean came back as a boolean. */
    if (s0) {
        ao_value_t vt = ao_sheet_get(in, s0, 1, 1);
        CHECK(vt.kind == AO_VAL_TEXT && vt.text &&
              strcmp(vt.text, "Привет") == 0);
        ao_value_t vb = ao_sheet_get(in, s0, 1, 2);
        CHECK(vb.kind == AO_VAL_BOOL && vb.num == 1.0);
        ao_value_t vf = ao_sheet_get(in, s0, 2, 0);
        CHECK(vf.kind == AO_VAL_NUM && vf.num == 3.5);
    }

    /* Re-export the imported book and require the same bytes. */
    buffer_t out3 = { out_b, OUT_CAP, 0 };
    size_t size3 = 0;
    CHECK(ao_xlsx_export_ex(in, NULL, to_buffer, &out3, &size3,
                            strbuf, sizeof strbuf, &strings_b,
                            NULL, NULL, NULL) == AO_OK);
    CHECK(size3 == size && memcmp(out_a, out_b, size) == 0);
    printf("[unit]   re-export identical: %zu bytes\n", size3);
}

static void test_parts_are_valid_xml(void) {
    printf("[unit] xlsx: every part is well-formed and the set is right\n");
    fresh();
    ao_book_t *b = build_book(&arena_a);
    buffer_t out = { out_a, OUT_CAP, 0 };
    size_t size = 0;
    CHECK(ao_xlsx_export(b, to_buffer, &out, &size) == AO_OK);

    ao_zip_reader_t r;
    CHECK(ao_zip_reader_init(&r, out_a, size) == AO_ZIP_OK);
    size_t n = ao_zip_reader_count(&r);
    /* 6 fixed parts + one worksheet per sheet. */
    CHECK(n == 8u);
    printf("[unit]   %zu parts\n", n);

    static const char *required[] = {
        "[Content_Types].xml", "_rels/.rels", "xl/workbook.xml",
        "xl/_rels/workbook.xml.rels", "xl/styles.xml", "xl/sharedStrings.xml",
        "xl/worksheets/sheet1.xml", "xl/worksheets/sheet2.xml"
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
        ao_zip_entry_t e;
        int found = ao_zip_reader_find(&r, required[i],
                                       strlen(required[i]), &e) == AO_ZIP_OK;
        CHECK(found);
        if (!found) continue;
        size_t got = 0;
        if (ao_zip_extract(&r, &e, scratch, sizeof scratch, &got) != AO_ZIP_OK) {
            CHECK(0);
            continue;
        }
        int ok = ao_xml_validate(scratch, got) == AO_XML_OK;
        if (!ok) printf("      (bad XML: %s)\n", required[i]);
        CHECK(ok);
    }

    /* The workbook part must name both sheets. */
    ao_zip_entry_t e;
    CHECK(ao_zip_reader_find(&r, "xl/workbook.xml", 15u, &e) == AO_ZIP_OK);
    size_t got = 0;
    CHECK(ao_zip_extract(&r, &e, scratch, sizeof scratch, &got) == AO_ZIP_OK);
    CHECK(strstr(scratch, "name=\"Sheet1\"") != NULL);
    CHECK(strstr(scratch, "name=\"Второй\"") != NULL);
}

static void test_foreign_workbook(void) {
    printf("[unit] xlsx: a workbook we did not write imports as what it says\n");
    /* Hand-written in Excel's shape: prefixed namespaces, an absolute
     * relationship target, inline strings, a boolean and a shared string. */
    static const char SHEET[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<cols><col min=\"1\" max=\"1\" width=\"14.5\" customWidth=\"1\"/></cols>"
        "<sheetData>"
        "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>title</t></is></c>"
        "<c r=\"B1\"><v>42</v></c></row>"
        "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>0</v></c>"
        "<c r=\"B2\" t=\"b\"><v>1</v></c></row>"
        "<row r=\"3\"><c r=\"A3\"><f>B1*2</f><v>84</v></c></row>"
        "</sheetData></worksheet>";
    static const char SST[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
        " count=\"1\" uniqueCount=\"1\"><si><t>shared</t></si></sst>";
    static const char WB[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets><sheet name=\"Data\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
    /* Absolute target: the importer must resolve it to the same part. */
    static const char RELS[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
        "officeDocument/2006/relationships/worksheet\""
        " Target=\"/xl/worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/"
        "officeDocument/2006/relationships/sharedStrings\""
        " Target=\"/xl/sharedStrings.xml\"/>"
        "</Relationships>";

    ao_zip_writer_t w;
    buffer_t out = { out_a, OUT_CAP, 0 };
    CHECK(ao_zip_writer_init(&w, to_buffer, &out) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&w, "xl/workbook.xml", 15u, WB, strlen(WB)) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&w, "xl/_rels/workbook.xml.rels", 26u,
                            RELS, strlen(RELS)) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&w, "xl/sharedStrings.xml", 20u,
                            SST, strlen(SST)) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&w, "xl/worksheets/sheet1.xml", 24u,
                            SHEET, strlen(SHEET)) == AO_ZIP_OK);
    size_t size = 0;
    CHECK(ao_zip_writer_finish(&w, &size) == AO_ZIP_OK);

    fresh();
    ao_book_t *in = ao_book_new(&arena_a);
    uint32_t skipped = 99;
    int rc = ao_xlsx_import(in, out_a, size, scratch, sizeof scratch,
                            strbuf, sizeof strbuf, &strings_a,
                            &cols_a, NULL, NULL, &skipped);
    CHECK(rc == AO_OK);
    CHECK(skipped == 0);
    CHECK(ao_book_sheet_count(in) == 1);
    ao_sheet_t *s = ao_book_sheet_at(in, 0);
    CHECK(s != NULL);
    if (!s) return;
    CHECK(strcmp(ao_sheet_name(s), "Data") == 0);
    CHECK(count_cells(in, 0) == 5);

    /* inlineStr, a number, a shared string, a boolean and a formula. */
    ao_value_t v;
    v = ao_sheet_get(in, s, 0, 0);
    CHECK(v.kind == AO_VAL_TEXT && v.text && strcmp(v.text, "title") == 0);
    v = ao_sheet_get(in, s, 0, 1);
    CHECK(v.kind == AO_VAL_NUM && v.num == 42.0);
    v = ao_sheet_get(in, s, 1, 0);
    CHECK(v.kind == AO_VAL_TEXT && v.text && strcmp(v.text, "shared") == 0);
    v = ao_sheet_get(in, s, 1, 1);
    CHECK(v.kind == AO_VAL_BOOL && v.num == 1.0);
    v = ao_sheet_get(in, s, 2, 0);
    CHECK(v.kind == AO_VAL_NUM && v.num == 84.0);

    /* The column width arrived in hundredths of a character. */
    CHECK(cols_a.w[0] == 1450u);
}

static void test_refusals_are_receipts(void) {
    printf("[unit] xlsx: what we do not support is named, not dropped\n");
    static const char SHEET[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData>"
        "<mergeCells count=\"1\"><mergeCell ref=\"A1:B2\"/></mergeCells>"
        "<conditionalFormatting sqref=\"A1\"><cfRule type=\"cellIs\"/>"
        "</conditionalFormatting>"
        "</worksheet>";
    static const char WB[] =
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets><sheet name=\"S\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";

    ao_zip_writer_t w;
    buffer_t out = { out_a, OUT_CAP, 0 };
    CHECK(ao_zip_writer_init(&w, to_buffer, &out) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&w, "xl/workbook.xml", 15u, WB, strlen(WB)) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&w, "xl/worksheets/sheet1.xml", 24u,
                            SHEET, strlen(SHEET)) == AO_ZIP_OK);
    size_t size = 0;
    CHECK(ao_zip_writer_finish(&w, &size) == AO_ZIP_OK);

    fresh();
    ao_book_t *in = ao_book_new(&arena_a);
    uint32_t skipped = 0;
    int rc = ao_xlsx_import(in, out_a, size, scratch, sizeof scratch,
                            strbuf, sizeof strbuf, &strings_a,
                            NULL, NULL, NULL, &skipped);
    CHECK(rc == AO_OK);
    /* Two distinct unsupported constructs, named rather than dropped. */
    CHECK(skipped == 2u);
    /* ...and the supported part of the sheet still arrived. */
    CHECK(count_cells(in, 0) == 1);
}

static void test_hostile_input(void) {
    printf("[unit] xlsx: hostile input is refused with a code\n");
    fresh();

    /* Not a ZIP at all. */
    static const char not_zip[] = "this is not a zip file";
    ao_book_t *b = ao_book_new(&arena_a);
    CHECK(ao_xlsx_import(b, not_zip, sizeof not_zip - 1u, scratch,
                         sizeof scratch, strbuf, sizeof strbuf, &strings_a,
                         NULL, NULL, NULL, NULL) == AO_E_FORMAT);

    /* A ZIP with no xl/workbook.xml. */
    ao_zip_writer_t w;
    buffer_t out = { out_a, OUT_CAP, 0 };
    CHECK(ao_zip_writer_init(&w, to_buffer, &out) == AO_ZIP_OK);
    CHECK(ao_zip_writer_add(&w, "readme.txt", 10u, "hello", 5u) == AO_ZIP_OK);
    size_t size = 0;
    CHECK(ao_zip_writer_finish(&w, &size) == AO_ZIP_OK);
    CHECK(ao_xlsx_import(b, out_a, size, scratch, sizeof scratch,
                         strbuf, sizeof strbuf, &strings_a,
                         NULL, NULL, NULL, NULL) == AO_E_FORMAT);

    /* A truncated archive. */
    CHECK(ao_xlsx_import(b, out_a, size / 2u, scratch, sizeof scratch,
                         strbuf, sizeof strbuf, &strings_a,
                         NULL, NULL, NULL, NULL) == AO_E_FORMAT);

    /* Bad arguments. */
    CHECK(ao_xlsx_import(NULL, out_a, size, scratch, sizeof scratch,
                         strbuf, sizeof strbuf, &strings_a,
                         NULL, NULL, NULL, NULL) == AO_E_ARG);
    CHECK(ao_xlsx_export(NULL, to_buffer, &out, &size) == AO_E_ARG);
}

static void test_column_widths(void) {
    printf("[unit] xlsx: column widths survive a round trip\n");
    fresh();
    ao_book_t *b = ao_book_new(&arena_a);
    ao_sheet_t *s = ao_book_sheet(b, "S", 1);
    ao_sheet_set(b, s, 0, 0, "1");
    memset(&cols_a, 0, sizeof cols_a);
    cols_a.w[0] = 960u;    /* 9.60 characters — the grid's default column */
    cols_a.w[2] = 1450u;

    buffer_t out = { out_a, OUT_CAP, 0 };
    size_t size = 0;
    CHECK(ao_xlsx_export_ex(b, &cols_a, to_buffer, &out, &size,
                            strbuf, sizeof strbuf, &strings_a,
                            NULL, NULL, NULL) == AO_OK);

    ao_book_t *in = ao_book_new(&arena_b);
    CHECK(ao_xlsx_import(in, out_a, size, scratch, sizeof scratch,
                         strbuf, sizeof strbuf, &strings_b,
                         &cols_b, NULL, NULL, NULL) == AO_OK);
    CHECK(cols_b.w[0] == 960u);
    CHECK(cols_b.w[2] == 1450u);
    CHECK(cols_b.w[1] == 0u);
}

static void test_empty_book(void) {
    printf("[unit] xlsx: an empty workbook is a valid package\n");
    fresh();
    ao_book_t *b = ao_book_new(&arena_a);
    ao_book_sheet(b, "Empty", 1);
    buffer_t out = { out_a, OUT_CAP, 0 };
    size_t size = 0;
    CHECK(ao_xlsx_export(b, to_buffer, &out, &size) == AO_OK);
    CHECK(size > 0);

    ao_book_t *in = ao_book_new(&arena_b);
    CHECK(ao_xlsx_import(in, out_a, size, scratch, sizeof scratch,
                         strbuf, sizeof strbuf, &strings_b,
                         NULL, NULL, NULL, NULL) == AO_OK);
    CHECK(count_cells(in, 0) == 0);
}

/* The .xlsx subset the code actually handles.  This list is the second half of
 * the §9 table in docs/office.md: the test fails if the table omits one of
 * these, AND if the table ticks more than these.  A documented subset that
 * drifts from the code is worse than none, because it is believed. */
static const char *XLSX_SUPPORTED[] = {
    "workbook", "sheets", "sheet", "worksheet", "sheetData", "row", "c", "v",
    "f", "is", "t", "sst", "si", "cols", "col"
};

static void test_documented_subset(void) {
    FILE *f = fopen("docs/office.md", "rb");
    CHECK(f != NULL);
    if (!f) return;

    static char doc[65536];
    size_t n = fread(doc, 1, sizeof doc - 1u, f);
    fclose(f);
    doc[n] = '\0';

    char *begin = strstr(doc, "<!-- OF8-XLSX-SUBSET-BEGIN -->");
    char *end = strstr(doc, "<!-- OF8-XLSX-SUBSET-END -->");
    CHECK(begin != NULL);
    CHECK(end != NULL);
    if (!begin || !end || end <= begin) return;
    *end = '\0';                       /* work inside the marked region only */

    for (size_t i = 0; i < sizeof XLSX_SUPPORTED / sizeof XLSX_SUPPORTED[0]; i++) {
        char row[256];
        snprintf(row, sizeof row, "| `%s`", XLSX_SUPPORTED[i]);
        char *at = strstr(begin, row);
        int ok = at != NULL;
        if (ok) {
            /* The tick must be on that construct's own row, not merely later
             * in the file -- otherwise one tick satisfies every row. */
            char *eol = strchr(at, '\n');
            if (!eol) eol = begin + strlen(begin);
            char save = *eol;
            *eol = '\0';
            ok = strstr(at, "\xe2\x9c\x85") != NULL;   /* U+2705 */
            *eol = save;
        }
        if (!ok) {
            failures++;
            printf("FAIL: docs/office.md omits %s from the .xlsx subset table\n",
                   XLSX_SUPPORTED[i]);
        }
        checks++;
    }

    unsigned ticks = 0;
    for (char *p = begin; (p = strstr(p, "\xe2\x9c\x85")) != NULL; p += 3) ticks++;
    CHECK(ticks == sizeof XLSX_SUPPORTED / sizeof XLSX_SUPPORTED[0]);
    if (ticks != sizeof XLSX_SUPPORTED / sizeof XLSX_SUPPORTED[0])
        printf("  (subset table claims %u supported, the code handles %zu)\n",
               ticks, sizeof XLSX_SUPPORTED / sizeof XLSX_SUPPORTED[0]);
}

/* Emit the fixture tools/office/check_xlsx.py grades, so the independent
 * check and this one look at the same bytes. */
static void write_fixture(void) {
    fresh();
    ao_book_t *b = build_book(&arena_a);
    buffer_t out = { out_a, OUT_CAP, 0 };
    size_t size = 0;
    if (ao_xlsx_export(b, to_buffer, &out, &size) != AO_OK) return;
    const char *path = getenv("AO_XLSX_FIXTURE");
    if (!path) path = "build/test_ao_xlsx.xlsx";
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(out_a, 1, size, f);
    fclose(f);
    printf("[unit]   wrote fixture %s (%zu bytes)\n", path, size);
}

int main(void) {
    printf("[unit] xlsx OOXML bridge\n");
    test_round_trip();
    test_parts_are_valid_xml();
    test_foreign_workbook();
    test_refusals_are_receipts();
    test_hostile_input();
    test_column_widths();
    test_empty_book();
    test_documented_subset();
    write_fixture();

    if (failures) {
        printf("XLSX OOXML: FAIL (%u of %u checks)\n", failures, checks);
        return 1;
    }
    printf("XLSX OOXML: PASS (%u checks)\n", checks);
    return 0;
}
