/* userspace/office/libaodoc/aoxlsx.c — the OOXML bridge: .xlsx (OF8).
 *
 * See aoxlsx.h for the subset and for the two rules (refusal, determinism)
 * that decide every edge case.  This file is only translation: aozip.c owns
 * the container, aoxml.c owns the markup, aocalc.c owns the workbook.
 *
 * One fact about the parser drives the shape of the importer, and it is the
 * same fact aodox.c is built around: a self-closing element such as <c/>
 * yields a START token with empty=1 and NO matching END token.  Every element
 * that opens state therefore commits or clears its state when `empty` is set,
 * because no END will ever arrive to do it.
 */
#include "aoxlsx.h"

#include "aomodel.h"
#include "aoxml.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ part names */

/* Macros, not const arrays: these are concatenated into XML string literals
 * below, which only works for literals. */
#define PART_CONTENT_TYPES "[Content_Types].xml"
#define PART_ROOT_RELS     "_rels/.rels"
#define PART_WORKBOOK      "xl/workbook.xml"
#define PART_WORKBOOK_RELS "xl/_rels/workbook.xml.rels"
#define PART_STYLES        "xl/styles.xml"
#define PART_SHARED        "xl/sharedStrings.xml"

#define NS_MAIN   "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
#define NS_DOCREL "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define NS_PKGREL "http://schemas.openxmlformats.org/package/2006/relationships"
#define NS_CT     "http://schemas.openxmlformats.org/package/2006/content-types"

#define CT_RELS     "application/vnd.openxmlformats-package.relationships+xml"
#define CT_WORKBOOK "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"
#define CT_SHEET    "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"
#define CT_STYLES   "application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"
#define CT_SHARED   "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"

/* Sink failure: the caller's sink would not take the archive.  AO_E_FULL is
 * the closest documented outcome for "the bytes had nowhere to go". */
#define AO_XLSX_E_IO AO_E_FULL

/* ------------------------------------------------------------- small bits */

static const char *local_name(const char *name, size_t *len) {
    for (size_t i = 0; i < *len; i++) {
        if (name[i] == ':') { *len -= i + 1u; return name + i + 1u; }
    }
    return name;
}

static int name_is(const char *name, size_t len, const char *want) {
    size_t n = len;
    const char *p = local_name(name, &n);
    size_t w = strlen(want);
    return n == w && memcmp(p, want, w) == 0;
}

static int token_is(const ao_xml_token_t *t, const char *want) {
    return name_is(t->name, t->name_len, want);
}

static int attr_is(const ao_xml_attr_t *a, const char *want) {
    return name_is(a->name, a->name_len, want);
}

static const ao_xml_attr_t *find_attr(const ao_xml_token_t *t, const char *want) {
    for (uint32_t i = 0; i < t->attr_count; i++)
        if (attr_is(&t->attrs[i], want)) return &t->attrs[i];
    return NULL;
}

static int xml_error_to_doc(int rc) {
    if (rc == AO_XML_E_UTF8) return AO_E_UTF8;
    if (rc == AO_XML_E_FULL || rc == AO_XML_E_LIMIT) return AO_E_FULL;
    return AO_E_FORMAT;
}

static int attr_value(const ao_xml_attr_t *a, char *out, size_t cap) {
    if (!a || !out || cap == 0) return AO_E_ARG;
    size_t n = 0;
    int rc = ao_xml_decode(a->value, a->value_len, out, cap - 1u, &n);
    if (rc != AO_XML_OK) return xml_error_to_doc(rc);
    out[n] = '\0';
    return AO_OK;
}

/* "12.5" -> 1250 hundredths.  Refuses anything it cannot round-trip: a width
 * we misread is a sheet that looks wrong, which is worse than a receipt. */
static int parse_width(const char *s, uint32_t *out) {
    if (!s || !*s) return AO_E_FORMAT;
    uint32_t whole = 0, frac = 0, frac_div = 1;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        if (whole > (0xFFFFFFFFu - (uint32_t)(*p - '0')) / 10u) return AO_E_FULL;
        whole = whole * 10u + (uint32_t)(*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && frac_div < 100u) {
            frac = frac * 10u + (uint32_t)(*p - '0');
            frac_div *= 10u;
            p++;
        }
        while (*p >= '0' && *p <= '9') p++;   /* drop beyond hundredths */
    }
    if (*p) return AO_E_FORMAT;
    while (frac_div < 100u) { frac *= 10u; frac_div *= 10u; }
    if (whole > 0xFFFFFFFFu / 100u) return AO_E_FULL;
    *out = whole * 100u + frac;
    return AO_OK;
}

/* ------------------------------------------------------------- receipts */

typedef struct {
    ao_xlsx_note_fn fn;
    void           *context;
    uint32_t        skipped;    /* every skipped construct, deduped or not */
    uint32_t        count;      /* distinct receipts emitted             */
    char            text[AO_XLSX_MAX_NOTES][AO_XLSX_MAX_NOTE_BYTES];
    int             aborted;
} note_sink_t;

static void note_init(note_sink_t *n, ao_xlsx_note_fn fn, void *context) {
    n->fn = fn;
    n->context = context;
    n->skipped = 0;
    n->count = 0;
    n->aborted = 0;
    for (uint32_t i = 0; i < AO_XLSX_MAX_NOTES; i++) n->text[i][0] = '\0';
}

static void note_emit(note_sink_t *n, const char *text) {
    n->skipped++;
    if (!n->fn || n->aborted) return;
    for (uint32_t i = 0; i < n->count; i++) {
        if (strcmp(n->text[i], text) == 0) return;   /* deduped */
    }
    if (n->count < AO_XLSX_MAX_NOTES) {
        size_t k = strlen(text);
        if (k >= AO_XLSX_MAX_NOTE_BYTES) k = AO_XLSX_MAX_NOTE_BYTES - 1u;
        memcpy(n->text[n->count], text, k);
        n->text[n->count][k] = '\0';
        n->count++;
    }
    if (n->fn(n->context, text, strlen(text)) != 0) n->aborted = 1;
}

/* ------------------------------------------------------------- xml sink */

typedef struct {
    int (*write)(void *context, const void *bytes, size_t length);
    void  *context;
    size_t used;
    int    error;
} xsink_t;

static void xs_put(xsink_t *k, const char *s, size_t n) {
    if (k->error || n == 0) return;
    if (k->write(k->context, s, n) != 0) { k->error = AO_XLSX_E_IO; return; }
    k->used += n;
}

static void xs_puts(xsink_t *k, const char *s) { xs_put(k, s, strlen(s)); }

static void xs_put_u32(xsink_t *k, uint32_t v) {
    char tmp[12];
    size_t i = sizeof(tmp);
    tmp[--i] = '\0';
    if (!v) tmp[--i] = '0';
    while (v && i > 0) { tmp[--i] = (char)('0' + (v % 10u)); v /= 10u; }
    xs_puts(k, &tmp[i]);
}

static void xs_escape(xsink_t *k, const char *s, size_t n) {
    if (k->error || n == 0) return;
    size_t out = 0;
    int rc = ao_xml_escape_stream(s, n, 0, k->write, k->context, &out);
    if (rc != AO_XML_OK) { k->error = xml_error_to_doc(rc); return; }
    k->used += out;
}

static void xs_escape_attr(xsink_t *k, const char *s, size_t n) {
    if (k->error || n == 0) return;
    size_t out = 0;
    int rc = ao_xml_escape_stream(s, n, 1, k->write, k->context, &out);
    if (rc != AO_XML_OK) { k->error = xml_error_to_doc(rc); return; }
    k->used += out;
}

/* Hundredths as a decimal: 960 -> "9.60".  Printed with two places always,
 * so the bytes of a part do not depend on the value's trailing zeros. */
static void xs_put_hundredths(xsink_t *k, uint32_t v) {
    xs_put_u32(k, v / 100u);
    xs_puts(k, ".");
    uint32_t f = v % 100u;
    char d[3];
    d[0] = (char)('0' + f / 10u);
    d[1] = (char)('0' + f % 10u);
    xs_put(k, d, 2);
}

/* ------------------------------------------------------- zip plumbing */

typedef struct { size_t length; uint32_t crc_state; } xml_measure_t;

static int measure_write(void *context, const void *bytes, size_t length) {
    xml_measure_t *s = (xml_measure_t *)context;
    size_t max = (size_t)AO_ZIP_MAX_ARCHIVE_BYTES;
    if (!s || (!bytes && length) || length > max - s->length) return AO_ZIP_E_LIMIT;
    s->crc_state = ao_zip_crc32_update(s->crc_state, bytes, length);
    s->length += length;
    return 0;
}

typedef struct { ao_zip_writer_t *writer; } zip_stream_t;

static int zip_write_xml(void *context, const void *bytes, size_t length) {
    zip_stream_t *s = (zip_stream_t *)context;
    if (!s || !s->writer) return AO_ZIP_E_ARG;
    return ao_zip_writer_write_entry(s->writer, bytes, length);
}

typedef int (*part_writer_t)(void *ctx, xsink_t *k);

/* Emit one part: measure it, then stream it, so a worksheet never has to be
 * staged whole in memory. */
static int emit_part(ao_zip_writer_t *w, const char *name, size_t name_len,
                     part_writer_t fn, void *ctx) {
    xml_measure_t m;
    m.length = 0;
    m.crc_state = AO_ZIP_CRC32_INITIAL;
    xsink_t mk;
    mk.write = measure_write; mk.context = &m; mk.used = 0; mk.error = 0;
    if (fn(ctx, &mk) != AO_OK || mk.error) return mk.error ? mk.error : AO_E_FORMAT;

    int rc = ao_zip_writer_begin_entry(w, name, name_len, m.length,
                                       ao_zip_crc32_finish(m.crc_state));
    if (rc != AO_ZIP_OK) return rc == AO_ZIP_E_LIMIT ? AO_E_FULL : AO_E_FORMAT;

    zip_stream_t zs;
    zs.writer = w;
    xsink_t zk;
    zk.write = zip_write_xml; zk.context = &zs; zk.used = 0; zk.error = 0;
    if (fn(ctx, &zk) != AO_OK || zk.error) return zk.error ? zk.error : AO_E_FORMAT;
    if (zk.used != m.length) {
        /* The two passes disagreed: the archive header would lie about the
         * length.  Refuse rather than emit a package that lies. */
        return AO_E_FORMAT;
    }
    rc = ao_zip_writer_end_entry(w);
    return rc == AO_ZIP_OK ? AO_OK : (rc == AO_ZIP_E_LIMIT ? AO_E_FULL : AO_E_FORMAT);
}

/* ------------------------------------------------------- export context */

typedef struct {
    ao_book_t            *book;
    const ao_xlsx_cols_t *cols;
    uint32_t              sheets;

    char                 *strbuf;
    size_t                strbuf_cap;
    size_t                strbuf_used;
    ao_xlsx_strings_t    *strings;

    note_sink_t           notes;
} xexp_t;

/* Intern one string.  Returns 1 and sets *index when the string has a slot,
 * 0 when the caller must fall back to an inline string.  A full table is a
 * receipt, never a refused save. */
static int intern(xexp_t *x, const char *s, size_t n, uint32_t *index) {
    if (!x->strbuf || !x->strings || n == 0) return 0;
    for (uint32_t i = 0; i < x->strings->count; i++) {
        if (strcmp(x->strbuf + x->strings->off[i], s) == 0) { *index = i; return 1; }
    }
    if (x->strings->count >= AO_XLSX_MAX_SHARED ||
        n + 1u > x->strbuf_cap - x->strbuf_used) {
        x->strings->overflow++;
        return 0;
    }
    memcpy(x->strbuf + x->strbuf_used, s, n);
    x->strbuf[x->strbuf_used + n] = '\0';
    x->strings->off[x->strings->count] = (uint32_t)x->strbuf_used;
    x->strbuf_used += n + 1u;
    *index = x->strings->count;
    x->strings->count++;
    return 1;
}

/* --------------------------------------------------------- export parts */

static int w_content_types(void *ctx, xsink_t *k) {
    xexp_t *x = (xexp_t *)ctx;
    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<Types xmlns=\"" NS_CT "\">");
    xs_puts(k, "<Default Extension=\"rels\" ContentType=\"" CT_RELS "\"/>");
    xs_puts(k, "<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
    xs_puts(k, "<Override PartName=\"/" PART_WORKBOOK "\" ContentType=\"" CT_WORKBOOK "\"/>");
    for (uint32_t i = 0; i < x->sheets; i++) {
        xs_puts(k, "<Override PartName=\"/xl/worksheets/sheet");
        xs_put_u32(k, i + 1u);
        xs_puts(k, ".xml\" ContentType=\"" CT_SHEET "\"/>");
    }
    xs_puts(k, "<Override PartName=\"/" PART_STYLES "\" ContentType=\"" CT_STYLES "\"/>");
    xs_puts(k, "<Override PartName=\"/" PART_SHARED "\" ContentType=\"" CT_SHARED "\"/>");
    xs_puts(k, "</Types>");
    return k->error ? k->error : AO_OK;
}

static int w_root_rels(void *ctx, xsink_t *k) {
    (void)ctx;
    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<Relationships xmlns=\"" NS_PKGREL "\">");
    xs_puts(k, "<Relationship Id=\"rId1\" Type=\"" NS_DOCREL "/officeDocument\""
               " Target=\"" PART_WORKBOOK "\"/>");
    xs_puts(k, "</Relationships>");
    return k->error ? k->error : AO_OK;
}

static int w_workbook(void *ctx, xsink_t *k) {
    xexp_t *x = (xexp_t *)ctx;
    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<workbook xmlns=\"" NS_MAIN "\" xmlns:r=\"" NS_DOCREL "\"><sheets>");
    for (uint32_t i = 0; i < x->sheets; i++) {
        ao_sheet_t *s = ao_book_sheet_at(x->book, (int)i);
        const char *name = (s && ao_sheet_name(s)) ? ao_sheet_name(s) : "Sheet";
        xs_puts(k, "<sheet name=\"");
        xs_escape_attr(k, name, strlen(name));
        xs_puts(k, "\" sheetId=\"");
        xs_put_u32(k, i + 1u);
        xs_puts(k, "\" r:id=\"rId");
        xs_put_u32(k, i + 1u);
        xs_puts(k, "\"/>");
    }
    xs_puts(k, "</sheets></workbook>");
    return k->error ? k->error : AO_OK;
}

static int w_workbook_rels(void *ctx, xsink_t *k) {
    xexp_t *x = (xexp_t *)ctx;
    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<Relationships xmlns=\"" NS_PKGREL "\">");
    for (uint32_t i = 0; i < x->sheets; i++) {
        xs_puts(k, "<Relationship Id=\"rId");
        xs_put_u32(k, i + 1u);
        xs_puts(k, "\" Type=\"" NS_DOCREL "/worksheet\" Target=\"worksheets/sheet");
        xs_put_u32(k, i + 1u);
        xs_puts(k, ".xml\"/>");
    }
    xs_puts(k, "<Relationship Id=\"rId");
    xs_put_u32(k, x->sheets + 1u);
    xs_puts(k, "\" Type=\"" NS_DOCREL "/styles\" Target=\"styles.xml\"/>");
    xs_puts(k, "<Relationship Id=\"rId");
    xs_put_u32(k, x->sheets + 2u);
    xs_puts(k, "\" Type=\"" NS_DOCREL "/sharedStrings\" Target=\"sharedStrings.xml\"/>");
    xs_puts(k, "</Relationships>");
    return k->error ? k->error : AO_OK;
}

/* The one style we can honestly claim: General.  ao_book_t keeps what the
 * user typed and the engine's value; it has nowhere to keep a display
 * format, so a styles.xml advertising number formats would be a promise the
 * model never made.  Everything else in the part is the minimum a validator
 * requires to accept the file at all. */
static int w_styles(void *ctx, xsink_t *k) {
    (void)ctx;
    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<styleSheet xmlns=\"" NS_MAIN "\">");
    xs_puts(k, "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>");
    xs_puts(k, "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
               "<fill><patternFill patternType=\"gray125\"/></fill></fills>");
    xs_puts(k, "<borders count=\"1\"><border><left/><right/><top/><bottom/>"
               "<diagonal/></border></borders>");
    xs_puts(k, "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\""
               " borderId=\"0\"/></cellStyleXfs>");
    xs_puts(k, "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\""
               " borderId=\"0\" xfId=\"0\"/></cellXfs>");
    xs_puts(k, "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\""
               " builtinId=\"0\"/></cellStyles>");
    xs_puts(k, "</styleSheet>");
    return k->error ? k->error : AO_OK;
}

/* No count/uniqueCount attributes: both are optional, and `count` is the
 * number of OCCURRENCES, which we do not track.  Emitting uniqueCount in
 * that slot would be a number that is true only by accident. */
static int w_shared(void *ctx, xsink_t *k) {
    xexp_t *x = (xexp_t *)ctx;
    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<sst xmlns=\"" NS_MAIN "\">");
    if (x->strings) {
        for (uint32_t i = 0; i < x->strings->count; i++) {
            const char *s = x->strbuf + x->strings->off[i];
            xs_puts(k, "<si><t");
            if (*s && (*s == ' ' || *(s + strlen(s) - 1u) == ' ')) xs_puts(k, " xml:space=\"preserve\"");
            xs_puts(k, ">");
            xs_escape(k, s, strlen(s));
            xs_puts(k, "</t></si>");
        }
    }
    xs_puts(k, "</sst>");
    return k->error ? k->error : AO_OK;
}

typedef struct { xexp_t *x; ao_sheet_t *sheet; uint32_t index; } sheet_ctx_t;

static int w_sheet(void *ctx, xsink_t *k) {
    sheet_ctx_t *sc = (sheet_ctx_t *)ctx;
    xexp_t *x = sc->x;
    ao_book_t *b = x->book;
    ao_sheet_t *s = sc->sheet;

    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<worksheet xmlns=\"" NS_MAIN "\">");

    if (x->cols && sc->index == 0u) {
        int any = 0;
        for (uint32_t c = 0; c < AO_XLSX_MAX_COLS; c++)
            if (x->cols->w[c]) { any = 1; break; }
        if (any) {
            xs_puts(k, "<cols>");
            for (uint32_t c = 0; c < AO_XLSX_MAX_COLS; c++) {
                if (!x->cols->w[c]) continue;
                xs_puts(k, "<col min=\"");
                xs_put_u32(k, c + 1u);
                xs_puts(k, "\" max=\"");
                xs_put_u32(k, c + 1u);
                xs_puts(k, "\" width=\"");
                xs_put_hundredths(k, x->cols->w[c]);
                xs_puts(k, "\" customWidth=\"1\"/>");
            }
            xs_puts(k, "</cols>");
        }
    }

    xs_puts(k, "<sheetData>");
    uint32_t n = ao_sheet_cell_count(s);
    uint32_t cur_row = 0xFFFFFFFFu;
    int row_open = 0;
    for (uint32_t i = 0; i < n; i++) {
        ao_cell_t *c = ao_sheet_cell_sorted(b, s, i);
        if (!c) continue;
        uint32_t row, col;
        ao_cell_pos(c, &row, &col);

        if (!row_open || row != cur_row) {
            if (row_open) xs_puts(k, "</row>");
            xs_puts(k, "<row r=\"");
            xs_put_u32(k, row + 1u);
            xs_puts(k, "\">");
            cur_row = row;
            row_open = 1;
        }

        const char *input = ao_cell_input(c);
        if (!input) input = "";
        int is_formula = (input[0] == '=');
        ao_value_t v = ao_sheet_get(b, s, row, col);

        char ref[16];
        ao_ref_name(row, col, ref, sizeof(ref));
        xs_puts(k, "<c r=\"");
        xs_puts(k, ref);

        if (is_formula) {
            xs_puts(k, "\"><f>");
            xs_escape(k, input + 1u, strlen(input + 1u));
            xs_puts(k, "</f>");
            /* A formula's cached value carries the type of the result. */
            if (v.kind == AO_VAL_TEXT && v.text) {
                xs_puts(k, "<v>");
                xs_escape(k, v.text, strlen(v.text));
                xs_puts(k, "</v>");
            } else if (v.kind == AO_VAL_BOOL) {
                xs_puts(k, "<v>");
                xs_puts(k, v.num != 0.0 ? "1" : "0");
                xs_puts(k, "</v>");
            } else if (v.kind == AO_VAL_ERR) {
                xs_puts(k, "<v>");
                xs_escape(k, ao_err_str((ao_err_t)v.err),
                          strlen(ao_err_str((ao_err_t)v.err)));
                xs_puts(k, "</v>");
            } else if (v.kind == AO_VAL_NUM) {
                char num[64];
                ao_format_value(b, v, "General", num, sizeof(num));
                xs_puts(k, "<v>");
                xs_puts(k, num);
                xs_puts(k, "</v>");
            }
            xs_puts(k, "</c>");
            continue;
        }

        if (v.kind == AO_VAL_TEXT && v.text) {
            uint32_t idx = 0;
            if (intern(x, v.text, strlen(v.text), &idx)) {
                xs_puts(k, "\" t=\"s\"><v>");
                xs_put_u32(k, idx);
                xs_puts(k, "</v></c>");
            } else {
                xs_puts(k, "\" t=\"inlineStr\"><is><t");
                size_t sl = strlen(v.text);
                if (*v.text == ' ' || v.text[sl - 1u] == ' ') xs_puts(k, " xml:space=\"preserve\"");
                xs_puts(k, ">");
                xs_escape(k, v.text, sl);
                xs_puts(k, "</t></is></c>");
            }
        } else if (v.kind == AO_VAL_BOOL) {
            xs_puts(k, "\" t=\"b\"><v>");
            xs_puts(k, v.num != 0.0 ? "1" : "0");
            xs_puts(k, "</v></c>");
        } else if (v.kind == AO_VAL_ERR) {
            xs_puts(k, "\" t=\"e\"><v>");
            xs_escape(k, ao_err_str((ao_err_t)v.err),
                      strlen(ao_err_str((ao_err_t)v.err)));
            xs_puts(k, "</v></c>");
        } else if (v.kind == AO_VAL_NUM) {
            char num[64];
            ao_format_value(b, v, "General", num, sizeof(num));
            xs_puts(k, "\"><v>");
            xs_puts(k, num);
            xs_puts(k, "</v></c>");
        } else {
            xs_puts(k, "\"/>");   /* blank: nothing to claim */
        }
    }
    if (row_open) xs_puts(k, "</row>");
    xs_puts(k, "</sheetData></worksheet>");
    return k->error ? k->error : AO_OK;
}

/* --------------------------------------------------------------- export */

size_t ao_xlsx_export_bound(const ao_book_t *b) {
    if (!b) return 0;
    size_t total = 8192u;   /* the six fixed parts, generously */
    int sheets = ao_book_sheet_count(b);
    if (sheets < 0) sheets = 0;
    if (sheets > (int)AO_XLSX_MAX_SHEETS) sheets = (int)AO_XLSX_MAX_SHEETS;
    for (int i = 0; i < sheets; i++) {
        ao_sheet_t *s = ao_book_sheet_at((ao_book_t *)b, i);
        if (!s) continue;
        uint32_t n = ao_sheet_cell_count(s);
        /* <c r="A1048576" t="inlineStr"><f>..</f><v>..</v></c> is ~64 bytes of
         * markup; escaping can sextuple the text inside it. */
        total += 512u + (size_t)n * 96u;
        for (uint32_t j = 0; j < n; j++) {
            ao_cell_t *c = ao_sheet_cell_sorted((ao_book_t *)b, s, j);
            if (!c) continue;
            const char *in = ao_cell_input(c);
            total += 12u * (in ? strlen(in) : 0u) + 64u;
        }
        total += 2048u;
    }
    return total;
}

int ao_xlsx_export(ao_book_t *b, ao_zip_write_fn write, void *context,
                   size_t *archive_size) {
    return ao_xlsx_export_ex(b, NULL, write, context, archive_size,
                             NULL, 0, NULL, NULL, NULL, NULL);
}

int ao_xlsx_export_ex(ao_book_t *b, const ao_xlsx_cols_t *cols,
                      ao_zip_write_fn write, void *context,
                      size_t *archive_size,
                      char *strbuf, size_t strbuf_cap,
                      ao_xlsx_strings_t *strings,
                      ao_xlsx_note_fn note, void *note_context,
                      uint32_t *skipped_out) {
    if (!b || !write) return AO_E_ARG;
    if ((strbuf != NULL) != (strings != NULL)) return AO_E_ARG;
    if (strbuf && strbuf_cap == 0) return AO_E_ARG;

    xexp_t x;
    memset(&x, 0, sizeof(x));
    x.book = b;
    x.cols = cols;
    x.strbuf = strbuf;
    x.strbuf_cap = strbuf_cap;
    x.strings = strings;
    note_init(&x.notes, note, note_context);
    if (strings) { strings->count = 0; strings->overflow = 0; }

    int total = ao_book_sheet_count(b);
    if (total < 0) total = 0;
    if ((uint32_t)total > AO_XLSX_MAX_SHEETS) {
        note_emit(&x.notes, "sheet count over AO_XLSX_MAX_SHEETS");
        total = (int)AO_XLSX_MAX_SHEETS;
    }
    x.sheets = (uint32_t)total;

    /* Pass 1 over every cell: intern the strings so xl/sharedStrings.xml can
     * be written before the sheets that reference it. */
    for (uint32_t i = 0; i < x.sheets; i++) {
        ao_sheet_t *s = ao_book_sheet_at(b, (int)i);
        if (!s) continue;
        uint32_t n = ao_sheet_cell_count(s);
        for (uint32_t j = 0; j < n; j++) {
            ao_cell_t *c = ao_sheet_cell_sorted(b, s, j);
            if (!c) continue;
            uint32_t row, col;
            ao_cell_pos(c, &row, &col);
            const char *in = ao_cell_input(c);
            if (in && in[0] == '=') continue;
            ao_value_t v = ao_sheet_get(b, s, row, col);
            if (v.kind == AO_VAL_TEXT && v.text) {
                uint32_t idx = 0;
                intern(&x, v.text, strlen(v.text), &idx);
            }
        }
    }

    ao_zip_writer_t w;
    int rc = ao_zip_writer_init(&w, write, context);
    if (rc != AO_ZIP_OK) return AO_E_FORMAT;

    rc = emit_part(&w, PART_CONTENT_TYPES, sizeof(PART_CONTENT_TYPES) - 1u,
                   w_content_types, &x);
    if (rc == AO_OK)
        rc = emit_part(&w, PART_ROOT_RELS, sizeof(PART_ROOT_RELS) - 1u,
                       w_root_rels, &x);
    if (rc == AO_OK)
        rc = emit_part(&w, PART_WORKBOOK, sizeof(PART_WORKBOOK) - 1u,
                       w_workbook, &x);
    if (rc == AO_OK)
        rc = emit_part(&w, PART_WORKBOOK_RELS, sizeof(PART_WORKBOOK_RELS) - 1u,
                       w_workbook_rels, &x);
    if (rc == AO_OK)
        rc = emit_part(&w, PART_STYLES, sizeof(PART_STYLES) - 1u, w_styles, &x);
    if (rc == AO_OK)
        rc = emit_part(&w, PART_SHARED, sizeof(PART_SHARED) - 1u, w_shared, &x);

    for (uint32_t i = 0; rc == AO_OK && i < x.sheets; i++) {
        ao_sheet_t *s = ao_book_sheet_at(b, (int)i);
        sheet_ctx_t sc;
        sc.x = &x;
        sc.sheet = s;
        sc.index = i;
        char part[AO_XLSX_MAX_PATH];
        int pn = snprintf(part, sizeof(part), "xl/worksheets/sheet%u.xml", i + 1u);
        if (pn <= 0 || (size_t)pn >= sizeof(part)) { rc = AO_E_FULL; break; }
        rc = emit_part(&w, part, (size_t)pn, w_sheet, &sc);
    }

    if (rc == AO_OK) {
        size_t total_bytes = 0;
        int frc = ao_zip_writer_finish(&w, &total_bytes);
        if (frc != AO_ZIP_OK) rc = (frc == AO_ZIP_E_LIMIT) ? AO_E_FULL : AO_E_FORMAT;
        else if (archive_size) *archive_size = total_bytes;
    }

    if (skipped_out) *skipped_out = x.notes.skipped;
    return rc;
}

/* --------------------------------------------------------------- import */

typedef struct {
    ao_book_t    *book;
    note_sink_t   notes;
    ao_xlsx_strings_t *strings;
    char         *strbuf;
    size_t        strbuf_cap;
    size_t        strbuf_used;
    ao_xlsx_cols_t *cols;
} ximp_t;

/* xl/workbook.xml: the sheet names in order, with the r:id each one is
 * reached through. */
static int parse_workbook(ximp_t *im, const char *xml, size_t len,
                          char names[AO_XLSX_MAX_SHEETS][AO_XLSX_MAX_NAME],
                          char rids[AO_XLSX_MAX_SHEETS][32], uint32_t *count) {
    ao_xml_reader_t r;
    ao_xml_token_t t;
    if (ao_xml_reader_init(&r, xml, len) != AO_XML_OK) return AO_E_FORMAT;
    *count = 0;
    for (;;) {
        int rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type != AO_XML_T_START) continue;
        if (!token_is(&t, "sheet")) continue;
        if (*count >= AO_XLSX_MAX_SHEETS) {
            note_emit(&im->notes, "sheet count over AO_XLSX_MAX_SHEETS");
            return AO_OK;
        }
        const ao_xml_attr_t *an = find_attr(&t, "name");
        const ao_xml_attr_t *ai = find_attr(&t, "r:id");
        names[*count][0] = '\0';
        rids[*count][0] = '\0';
        if (an) {
            if (attr_value(an, names[*count], AO_XLSX_MAX_NAME) != AO_OK)
                note_emit(&im->notes, "sheet name too long");
        }
        if (ai) {
            if (attr_value(ai, rids[*count], 32) != AO_OK)
                note_emit(&im->notes, "sheet r:id too long");
        }
        if (!names[*count][0]) {
            int k = snprintf(names[*count], AO_XLSX_MAX_NAME, "Sheet%u", *count + 1u);
            if (k <= 0) return AO_E_FORMAT;
        }
        (*count)++;
    }
    return AO_OK;
}

/* xl/_rels/workbook.xml.rels: r:id -> part path, worksheet targets only. */
static int parse_workbook_rels(const char *xml, size_t len,
                               char ids[AO_XLSX_MAX_SHEETS][32],
                               char targets[AO_XLSX_MAX_SHEETS][AO_XLSX_MAX_PATH],
                               uint32_t *count) {
    ao_xml_reader_t r;
    ao_xml_token_t t;
    if (ao_xml_reader_init(&r, xml, len) != AO_XML_OK) return AO_E_FORMAT;
    *count = 0;
    for (;;) {
        int rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type != AO_XML_T_START) continue;
        if (!token_is(&t, "Relationship")) continue;
        const ao_xml_attr_t *ai = find_attr(&t, "Id");
        const ao_xml_attr_t *at = find_attr(&t, "Type");
        const ao_xml_attr_t *ag = find_attr(&t, "Target");
        if (!ai || !at || !ag) continue;
        char type[192];
        if (attr_value(at, type, sizeof(type)) != AO_OK) continue;
        size_t tl = strlen(type);
        const char *tail = "/worksheet";
        size_t kl = strlen(tail);
        if (tl < kl || memcmp(type + tl - kl, tail, kl) != 0) continue;
        if (*count >= AO_XLSX_MAX_SHEETS) break;
        char tgt[AO_XLSX_MAX_PATH];
        if (attr_value(ag, tgt, sizeof(tgt)) != AO_OK) continue;
        /* Resolve against xl/: "worksheets/sheet1.xml" and the absolute
         * "/xl/worksheets/sheet1.xml" must both land on the same part. */
        if (tgt[0] == '/') {
            memmove(tgt, tgt + 1, strlen(tgt));
        } else if (strncmp(tgt, "xl/", 3) != 0) {
            char tmp[AO_XLSX_MAX_PATH];
            int k = snprintf(tmp, sizeof(tmp), "xl/%s", tgt);
            if (k <= 0 || (size_t)k >= sizeof(tmp)) continue;
            memcpy(tgt, tmp, strlen(tmp) + 1u);
        }
        if (attr_value(ai, ids[*count], 32) != AO_OK) continue;
        memcpy(targets[*count], tgt, strlen(tgt) + 1u);
        (*count)++;
    }
    return AO_OK;
}

/* xl/sharedStrings.xml -> NUL-terminated strings packed into strbuf.  Rich
 * text (<si> with several <r><t> runs) is concatenated, which is what every
 * reader shows and the only thing our model can hold. */
static int parse_shared_strings(ximp_t *im, const char *xml, size_t len) {
    ao_xml_reader_t r;
    ao_xml_token_t t;
    if (ao_xml_reader_init(&r, xml, len) != AO_XML_OK) return AO_E_FORMAT;

    char si[AO_XLSX_MAX_TEXT_BYTES];
    size_t si_len = 0;
    int in_t = 0;

    for (;;) {
        int rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type == AO_XML_T_START) {
            if (token_is(&t, "si")) {
                si_len = 0;
            } else if (token_is(&t, "t")) {
                in_t = 1;
                if (t.empty) in_t = 0;
                continue;
            }
            continue;
        }
        if (t.type == AO_XML_T_TEXT) {
            if (!in_t || si_len >= sizeof(si) - 1u) continue;
            size_t out = 0;
            int rc = ao_xml_decode(t.text, t.text_len, si + si_len,
                                   sizeof(si) - 1u - si_len, &out);
            if (rc != AO_XML_OK) return xml_error_to_doc(rc);
            si_len += out;
            continue;
        }
        if (t.type == AO_XML_T_END) {
            if (token_is(&t, "t")) { in_t = 0; continue; }
            if (token_is(&t, "si")) { /* commit below */ }
            else continue;
        } else {
            continue;
        }

        /* Commit one shared string. */
        if (!im->strings || !im->strbuf) {
            im->notes.skipped++;
        } else if (im->strings->count >= AO_XLSX_MAX_SHARED ||
                   si_len + 1u > im->strbuf_cap - im->strbuf_used) {
            im->strings->overflow++;
            note_emit(&im->notes, "shared string table full");
        } else {
            memcpy(im->strbuf + im->strbuf_used, si, si_len);
            im->strbuf[im->strbuf_used + si_len] = '\0';
            im->strings->off[im->strings->count] = (uint32_t)im->strbuf_used;
            im->strbuf_used += si_len + 1u;
            im->strings->count++;
        }
        si_len = 0;
    }
    return AO_OK;
}

/* One worksheet.  Commits through ao_sheet_set(), so the engine decides what
 * a formula means — we only carry the text across. */
static int parse_sheet(ximp_t *im, ao_sheet_t *sheet, const char *xml, size_t len) {
    ao_xml_reader_t r;
    ao_xml_token_t t;
    if (ao_xml_reader_init(&r, xml, len) != AO_XML_OK) return AO_E_FORMAT;

    uint32_t row = 0, col = 0;
    int have_cell = 0;
    char ctype[16];
    char vbuf[AO_XLSX_MAX_CELL_BYTES];
    size_t vlen = 0;
    char fbuf[AO_XLSX_MAX_CELL_BYTES];
    size_t flen = 0;
    int in_v = 0, in_f = 0, in_is_t = 0, in_is = 0;
    int has_f = 0, has_v = 0;
    uint32_t cur_row = 0;
    int saw_row = 0;

    for (;;) {
        int rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type == AO_XML_T_START) {
            if (token_is(&t, "col")) {
                const ao_xml_attr_t *aw = find_attr(&t, "width");
                if (aw && im->cols) {
                    char wbuf[32];
                    if (attr_value(aw, wbuf, sizeof(wbuf)) == AO_OK) {
                        uint32_t w = 0;
                        if (parse_width(wbuf, &w) != AO_OK) {
                            note_emit(&im->notes, "column width not a number");
                        } else {
                            uint32_t k = 0;
                            const ao_xml_attr_t *an = find_attr(&t, "min");
                            if (an) {
                                char mb[32];
                                if (attr_value(an, mb, sizeof(mb)) != AO_OK) mb[0] = '\0';
                                uint32_t mn = 0;
                                int ok = 0;
                                for (const char *q = mb; *q; q++) {
                                    if (*q < '0' || *q > '9') { ok = 0; break; }
                                    mn = mn * 10u + (uint32_t)(*q - '0');
                                    ok = 1;
                                }
                                if (ok && mn) k = mn - 1u;
                            }
                            if (k < AO_XLSX_MAX_COLS) im->cols->w[k] = w;
                            else note_emit(&im->notes, "column index over AO_XLSX_MAX_COLS");
                        }
                    }
                }
                continue;
            }
            if (token_is(&t, "row")) {
                const ao_xml_attr_t *ar = find_attr(&t, "r");
                if (ar) {
                    char rb[32];
                    if (attr_value(ar, rb, sizeof(rb)) == AO_OK) {
                        uint32_t rv = 0;
                        int ok = 1;
                        for (const char *p = rb; *p; p++) {
                            if (*p < '0' || *p > '9') { ok = 0; break; }
                            rv = rv * 10u + (uint32_t)(*p - '0');
                        }
                        if (ok && rv) cur_row = rv - 1u;
                    }
                } else if (saw_row) {
                    cur_row++;
                }
                saw_row = 1;
                col = 0;
                continue;
            }
            if (token_is(&t, "c")) {
                have_cell = 1;
                ctype[0] = '\0';
                vlen = 0; flen = 0;
                has_f = 0; has_v = 0;
                const ao_xml_attr_t *ar = find_attr(&t, "r");
                if (ar) {
                    char rb[32];
                    if (attr_value(ar, rb, sizeof(rb)) == AO_OK) {
                        uint32_t rr = 0, cc = 0;
                        int abs_r = 0, abs_c = 0;
                        if (ao_ref_parse(rb, &rr, &cc, &abs_r, &abs_c) == 0) {
                            row = rr;
                            col = cc;
                        }
                    }
                } else {
                    row = cur_row;
                }
                const ao_xml_attr_t *at = find_attr(&t, "t");
                if (at) {
                    if (attr_value(at, ctype, sizeof(ctype)) != AO_OK) ctype[0] = '\0';
                }
                const ao_xml_attr_t *as = find_attr(&t, "s");
                if (as) {
                    /* AO_XLSX writes General only, so any style at all is a
                     * display format the model cannot keep.  Counted, not
                     * converted. */
                    char sb[32];
                    if (attr_value(as, sb, sizeof(sb)) == AO_OK && strcmp(sb, "0") != 0)
                        note_emit(&im->notes, "cell style (number format)");
                }
                if (t.empty) { /* <c/> carries no value: nothing to commit */ have_cell = 0; }
                continue;
            }
            if (token_is(&t, "v")) { in_v = 1; if (t.empty) in_v = 0; continue; }
            if (token_is(&t, "f")) {
                in_f = 1;
                const ao_xml_attr_t *af = find_attr(&t, "t");
                if (af) {
                    char fb[32];
                    if (attr_value(af, fb, sizeof(fb)) == AO_OK && fb[0])
                        note_emit(&im->notes, "shared formula");
                }
                if (t.empty) in_f = 0;
                continue;
            }
            if (token_is(&t, "is")) { in_is = 1; if (t.empty) in_is = 0; continue; }
            if (token_is(&t, "t") && in_is) { in_is_t = 1; if (t.empty) in_is_t = 0; continue; }
            /* Unsupported worksheet children: named once, import continues. */
            if (token_is(&t, "mergeCells")) {
                note_emit(&im->notes, "skipped <mergeCells>");
            } else if (token_is(&t, "conditionalFormatting")) {
                note_emit(&im->notes, "skipped <conditionalFormatting>");
            } else if (token_is(&t, "dataValidation")) {
                note_emit(&im->notes, "skipped <dataValidation>");
            } else if (token_is(&t, "drawing") || token_is(&t, "picture")) {
                note_emit(&im->notes, "skipped <drawing>");
            } else if (token_is(&t, "hyperlink")) {
                note_emit(&im->notes, "skipped <hyperlink>");
            } else if (token_is(&t, "extLst")) {
                note_emit(&im->notes, "skipped <extLst>");
            }
            continue;
        }

        if (t.type == AO_XML_T_TEXT) {
            char *dst = NULL;
            size_t cap = 0;
            size_t *dlen = NULL;
            if (in_v) { dst = vbuf; cap = sizeof(vbuf) - 1u; dlen = &vlen; has_v = 1; }
            else if (in_f) { dst = fbuf; cap = sizeof(fbuf) - 1u; dlen = &flen; has_f = 1; }
            else if (in_is_t) { dst = vbuf; cap = sizeof(vbuf) - 1u; dlen = &vlen; has_v = 1; }
            if (!dst) continue;
            size_t out = 0;
            int rc = ao_xml_decode(t.text, t.text_len, dst + *dlen, cap - *dlen, &out);
            if (rc == AO_XML_E_FULL) { note_emit(&im->notes, "cell text too long"); continue; }
            if (rc != AO_XML_OK) return xml_error_to_doc(rc);
            *dlen += out;
            dst[*dlen] = '\0';
            continue;
        }

        if (t.type != AO_XML_T_END) continue;

        if (token_is(&t, "v")) { in_v = 0; continue; }
        if (token_is(&t, "f")) { in_f = 0; continue; }
        if (token_is(&t, "t") && in_is_t) { in_is_t = 0; continue; }
        if (token_is(&t, "is")) { in_is = 0; continue; }
        if (!token_is(&t, "c")) continue;

        /* ---- commit the cell ---- */
        if (!have_cell) continue;
        have_cell = 0;
        if (!sheet) continue;

        char input[AO_XLSX_MAX_CELL_BYTES + 2u];
        input[0] = '\0';

        if (has_f && flen) {
            input[0] = '=';
            memcpy(input + 1u, fbuf, flen + 1u);
        } else if (strcmp(ctype, "s") == 0) {
            uint32_t idx = 0;
            int ok = 0;
            for (const char *p = vbuf; *p; p++) {
                if (*p < '0' || *p > '9') { ok = 0; break; }
                idx = idx * 10u + (uint32_t)(*p - '0');
                ok = 1;
            }
            if (!ok && !has_v) {
                /* <c t="s"/> with no index: an empty string cell, which the
                 * model spells as a cleared cell. */
                ao_sheet_set(im->book, sheet, row, col, "");
                continue;
            }
            if (im->strings && idx < im->strings->count) {
                const char *sv = im->strbuf + im->strings->off[idx];
                size_t sl = strlen(sv);
                if (sl + 1u > sizeof(input)) { note_emit(&im->notes, "cell text too long"); continue; }
                memcpy(input, sv, sl + 1u);
            } else {
                note_emit(&im->notes, "shared string index out of range");
                continue;
            }
        } else if (strcmp(ctype, "inlineStr") == 0 || strcmp(ctype, "str") == 0) {
            memcpy(input, vbuf, vlen + 1u);
        } else if (strcmp(ctype, "b") == 0) {
            const char *b = (vlen && vbuf[0] == '1') ? "TRUE" : "FALSE";
            memcpy(input, b, strlen(b) + 1u);
        } else if (strcmp(ctype, "e") == 0) {
            /* An error value has no input the engine could recompute; it is
             * carried as its own text, which is what a reader shows. */
            memcpy(input, vbuf, vlen + 1u);
        } else if (has_v) {
            memcpy(input, vbuf, vlen + 1u);
        } else {
            continue;   /* <c r="A1"/> with nothing in it */
        }

        if (input[0] == '=' && !(has_f && flen)) {
            /* Text that begins with '=' cannot be expressed: ao_sheet_set()
             * reads a leading '=' as a formula.  Named, not silently bent. */
            note_emit(&im->notes, "text starting with '=' imported as formula");
        }

        int src = ao_sheet_set(im->book, sheet, row, col, input);
        if (src == AO_CALC_ERR_CAP || src == AO_CALC_ERR_NOMEM) return AO_E_FULL;
    }
    return AO_OK;
}

/* --------------------------------------------------------------- import */

const char *ao_xlsx_workbook_part(void) { return PART_WORKBOOK; }
const char *ao_xlsx_shared_part(void)   { return PART_SHARED; }

int ao_xlsx_import(ao_book_t *book, const void *bytes, size_t length,
                   char *scratch, size_t scratch_cap,
                   char *strbuf, size_t strbuf_cap,
                   ao_xlsx_strings_t *strings,
                   ao_xlsx_cols_t *cols_out,
                   ao_xlsx_note_fn note, void *note_context,
                   uint32_t *skipped_out) {
    if (!book || !bytes || !scratch || scratch_cap == 0) return AO_E_ARG;
    if ((strbuf != NULL) != (strings != NULL)) return AO_E_ARG;

    ximp_t im;
    memset(&im, 0, sizeof(im));
    im.book = book;
    im.strings = strings;
    im.strbuf = strbuf;
    im.strbuf_cap = strbuf_cap;
    im.cols = cols_out;
    note_init(&im.notes, note, note_context);
    if (strings) { strings->count = 0; strings->overflow = 0; }
    if (cols_out) memset(cols_out, 0, sizeof(*cols_out));

    ao_zip_reader_t reader;
    if (ao_zip_reader_init(&reader, bytes, length) != AO_ZIP_OK) return AO_E_FORMAT;

    /* ---- xl/workbook.xml: the sheet list ---- */
    ao_zip_entry_t e;
    if (ao_zip_reader_find(&reader, PART_WORKBOOK, sizeof(PART_WORKBOOK) - 1u, &e)
        != AO_ZIP_OK) {
        return AO_E_FORMAT;
    }
    {
        const uint8_t *view = NULL;
        size_t vlen = 0;
        const char *xml;
        size_t xlen;
        if (e.method == AO_ZIP_STORE &&
            ao_zip_entry_view(&reader, &e, &view, &vlen) == AO_ZIP_OK) {
            xml = (const char *)view;
            xlen = vlen;
        } else {
            size_t got = 0;
            if (ao_zip_extract(&reader, &e, scratch, scratch_cap, &got) != AO_ZIP_OK)
                return AO_E_FORMAT;
            xml = scratch;
            xlen = got;
        }
        static char names[AO_XLSX_MAX_SHEETS][AO_XLSX_MAX_NAME];
        static char rids[AO_XLSX_MAX_SHEETS][32];
        uint32_t nsheets = 0;
        int rc = parse_workbook(&im, xml, xlen, names, rids, &nsheets);
        if (rc != AO_OK) return rc;
        if (nsheets == 0) return AO_E_FORMAT;

        /* ---- xl/_rels/workbook.xml.rels: r:id -> part ---- */
        static char rel_ids[AO_XLSX_MAX_SHEETS][32];
        static char rel_targets[AO_XLSX_MAX_SHEETS][AO_XLSX_MAX_PATH];
        uint32_t nrels = 0;
        if (ao_zip_reader_find(&reader, PART_WORKBOOK_RELS,
                               sizeof(PART_WORKBOOK_RELS) - 1u, &e) == AO_ZIP_OK) {
            const uint8_t *rv = NULL;
            size_t rl = 0;
            const char *rxml;
            size_t rxlen;
            if (e.method == AO_ZIP_STORE &&
                ao_zip_entry_view(&reader, &e, &rv, &rl) == AO_ZIP_OK) {
                rxml = (const char *)rv;
                rxlen = rl;
            } else {
                size_t got = 0;
                if (ao_zip_extract(&reader, &e, scratch, scratch_cap, &got) != AO_ZIP_OK)
                    return AO_E_FORMAT;
                rxml = scratch;
                rxlen = got;
            }
            (void)parse_workbook_rels(rxml, rxlen, rel_ids, rel_targets, &nrels);
        }

        /* ---- xl/sharedStrings.xml ---- */
        if (ao_zip_reader_find(&reader, PART_SHARED, sizeof(PART_SHARED) - 1u, &e)
            == AO_ZIP_OK) {
            size_t got = 0;
            if (ao_zip_extract(&reader, &e, scratch, scratch_cap, &got) != AO_ZIP_OK)
                return AO_E_FORMAT;
            int rc2 = parse_shared_strings(&im, scratch, got);
            if (rc2 != AO_OK) return rc2;
        }

        /* ---- one pass per sheet ---- */
        for (uint32_t i = 0; i < nsheets; i++) {
            char part[AO_XLSX_MAX_PATH];
            const char *target = NULL;
            for (uint32_t k = 0; k < nrels; k++) {
                if (rids[i][0] && strcmp(rel_ids[k], rids[i]) == 0) {
                    target = rel_targets[k];
                    break;
                }
            }
            if (target) {
                size_t tl = strlen(target);
                if (tl + 1u > sizeof(part)) { note_emit(&im.notes, "sheet path too long"); continue; }
                memcpy(part, target, tl + 1u);
            } else {
                /* No relationship (or a workbook that never wrote one): fall
                 * back to the conventional name our own exporter emits. */
                int pn = snprintf(part, sizeof(part), "xl/worksheets/sheet%u.xml", i + 1u);
                if (pn <= 0 || (size_t)pn >= sizeof(part)) { note_emit(&im.notes, "sheet path too long"); continue; }
            }

            if (ao_zip_reader_find(&reader, part, strlen(part), &e) != AO_ZIP_OK) {
                note_emit(&im.notes, "sheet part missing");
                continue;
            }
            const uint8_t *sv = NULL;
            size_t sl = 0;
            const char *sxml;
            size_t sxlen;
            if (e.method == AO_ZIP_STORE &&
                ao_zip_entry_view(&reader, &e, &sv, &sl) == AO_ZIP_OK) {
                sxml = (const char *)sv;
                sxlen = sl;
            } else {
                size_t got = 0;
                if (ao_zip_extract(&reader, &e, scratch, scratch_cap, &got) != AO_ZIP_OK) {
                    note_emit(&im.notes, "sheet part unreadable");
                    continue;
                }
                sxml = scratch;
                sxlen = got;
            }
            ao_sheet_t *sheet = ao_book_sheet(book, names[i], 1);
            if (!sheet) { note_emit(&im.notes, "sheet could not be created"); continue; }
            int rc3 = parse_sheet(&im, sheet, sxml, sxlen);
            if (rc3 != AO_OK) return rc3;
        }
    }

    if (skipped_out) *skipped_out = im.notes.skipped;
    return im.notes.aborted ? AO_E_FORMAT : AO_OK;
}
