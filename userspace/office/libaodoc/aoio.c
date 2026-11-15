/* userspace/office/libaodoc/aoio.c — native XML format (OFFICE_PLAN OF2).
 *
 * The reader is a small hand-written scanner, not a general XML parser, and it
 * says so: no DTDs, no namespaces, no CDATA, no processing instructions beyond
 * skipping them.  What it does have is the property that matters for a document
 * that a user opens: every way a file can be wrong ends in an error code.
 *
 * The two rules that keep it honest:
 *
 *   - nothing is written to the model until the whole file has parsed.  The
 *     caller's document is cleared first, so a failed load leaves an EMPTY
 *     document, never a partially rebuilt one (that is the "refusal, not
 *     corruption" rule applied to files);
 *   - every loop advances.  A parser that can fail to consume a character is a
 *     parser that hangs on a crafted file, so each branch either consumes input
 *     or returns.
 */

#include "aoio.h"

#include <string.h>

#include "aodoc.h"
#include "aoxml.h"

/* ------------------------------------------------------------------ export */

size_t ao_io_export_bound(const ao_model_t *m) {
    if (!m) return 0;
    /* prologue + <aodoc> + closing, then per paragraph ~64 bytes of markup and
     * per run ~48, then the escaped text (at most 6x). */
    return 256u + (size_t)m->text_len * 6u +
           (size_t)m->para_len * 64u + (size_t)m->run_len * 48u;
}

typedef struct {
    char *p;
    size_t cap, used;
    ao_io_write_fn write;
    void *context;
    char pending[1024];
    size_t pending_len;
    int full;
    int error;
    int callback_failed;
} sink_t;

static void flush_stream(sink_t *k) {
    if (!k->write || !k->pending_len || k->error) return;
    int rc = k->write(k->context, k->pending, k->pending_len);
    if (rc != 0) {
        k->error = rc; k->callback_failed = 1; return;
    }
    k->pending_len = 0;
}

static void put(sink_t *k, const char *s, size_t n) {
    if (k->full || k->error) return;
    if (n > SIZE_MAX - k->used) { k->full = 1; return; }
    if (!k->write) {
        if (n > k->cap - k->used) { k->full = 1; return; }
        if (n) memcpy(k->p + k->used, s, n);
        k->used += n;
        return;
    }
    size_t left = n;
    const char *at = s;
    while (left) {
        if (k->pending_len == sizeof(k->pending)) flush_stream(k);
        if (k->error) return;
        size_t room = sizeof(k->pending) - k->pending_len;
        size_t take = left < room ? left : room;
        memcpy(k->pending + k->pending_len, at, take);
        k->pending_len += take;
        at += take; left -= take;
    }
    k->used += n;
}

static void puts_(sink_t *k, const char *s) { put(k, s, strlen(s)); }

static int xml_stream_emit(void *context, const void *bytes, size_t length) {
    sink_t *k = (sink_t *)context;
    if (!k || !k->write) return AO_E_ARG;
    put(k, (const char *)bytes, length);
    return k->error ? k->error : (k->full ? AO_E_FULL : 0);
}

static void put_xml_escaped(sink_t *k, const char *s, size_t n) {
    if (k->full || k->error) return;
    size_t written = 0;
    int rc;
    if (k->write) {
        size_t before = k->used;
        rc = ao_xml_escape_stream(s, n, 0, xml_stream_emit, k, &written);
        if (rc != AO_XML_OK) {
            if (!k->callback_failed) {
                if (rc == AO_XML_E_FULL || rc == AO_XML_E_LIMIT) k->full = 1;
                else k->error = rc == AO_XML_E_UTF8 ? AO_E_UTF8 : AO_E_FORMAT;
            }
            return;
        }
        if (k->used - before != written) k->full = 1;
    } else {
        rc = ao_xml_escape(s, n, 0, k->p + k->used,
                           k->cap - k->used, &written);
        if (rc == AO_XML_E_FULL) { k->full = 1; return; }
        if (rc != AO_XML_OK) {
            k->error = rc == AO_XML_E_UTF8 ? AO_E_UTF8 : AO_E_FORMAT;
            return;
        }
        if (written > SIZE_MAX - k->used) { k->full = 1; return; }
        k->used += written;
    }
}

static void put_u32(sink_t *k, uint32_t v) {
    char tmp[12];
    int i = 11;
    tmp[i] = '\0';
    if (!v) tmp[--i] = '0';
    while (v && i > 0) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
    puts_(k, &tmp[i]);
}

static void put_escaped(sink_t *k, const char *s, uint32_t n) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if ((uint8_t)s[i] != 1u) continue;
        put_xml_escaped(k, s + start, i - start);
        puts_(k, "&#1;"); /* AO_TABLE_MERGE_MARK: not an XML 1.0 literal. */
        start = i + 1u;
    }
    put_xml_escaped(k, s + start, (size_t)n - start);
}

static int io_export_sink(const ao_model_t *m, sink_t *k, size_t *len_out) {
    if (!m || !k) return AO_E_ARG;

    puts_(k, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<aodoc version=\"1\">\n");
    for (uint32_t pi = 0; pi < m->para_len; pi++) {
        const ao_para_t *p = &m->paras[pi];
        puts_(k, " <p align=\"");
        switch (p->align) {
            case AO_ALIGN_CENTER:  puts_(k, "center"); break;
            case AO_ALIGN_RIGHT:   puts_(k, "right"); break;
            case AO_ALIGN_JUSTIFY: puts_(k, "justify"); break;
            default:               puts_(k, "left"); break;
        }
        puts_(k, "\" indent=\"");
        put_u32(k, (uint32_t)(p->indent < 0 ? 0 : p->indent));
        puts_(k, "\" pstyle=\"");
        switch (p->style & AO_PARA_KIND_MASK) {
            case AO_PARA_H1:     puts_(k, "h1"); break;
            case AO_PARA_H2:     puts_(k, "h2"); break;
            case AO_PARA_H3:     puts_(k, "h3"); break;
            case AO_PARA_QUOTE:  puts_(k, "quote"); break;
            case AO_PARA_BULLET: puts_(k, "bullet"); break;
            case AO_PARA_NUMBER: puts_(k, "number"); break;
            case AO_PARA_TABLE:  puts_(k, "table"); break;
            case AO_PARA_IMAGE:  puts_(k, "image"); break;
            default:             puts_(k, "normal"); break;
        }
        puts_(k, "\"");
        if (p->style & AO_PARA_PAGEBREAK) puts_(k, " pb=\"1\"");
        puts_(k, ">\n");

        int wrote_run = 0;
        for (uint32_t r = 0; r < p->run_count; r++) {
            const ao_run_t *run = &m->runs[p->first_run + r];
            /* A paragraph's text is stored without its trailing newline. */
            uint32_t n = run->len;
            if (p->off + p->len > 0 && run->off + n >= p->off + p->len) {
                uint32_t end = p->off + p->len;
                if (n > 0 && end > 0 && m->text[end - 1] == '\n' && run->off + n == end) n--;
            }
            /* A run that held ONLY the newline has no text to write, and writing
             * "<r></r>" for it would make two documents that mean the same thing
             * serialise differently -- which is exactly what "save -> load ->
             * save is byte identical" forbids. */
            if (n == 0) continue;
            wrote_run = 1;
            puts_(k, "  <r style=\"");
            if (run->style & AO_STYLE_BOLD)      put(k, "b", 1);
            if (run->style & AO_STYLE_ITALIC)    put(k, "i", 1);
            if (run->style & AO_STYLE_UNDERLINE) put(k, "u", 1);
            puts_(k, "\" size=\"");
            put_u32(k, run->size);
            puts_(k, "\" font=\"");
            puts_(k, run->font == AO_FONT_MONO ? "mono" : "sans");
            puts_(k, "\">");
            put_escaped(k, m->text + run->off, n);
            puts_(k, "</r>\n");
        }
        if (!wrote_run) puts_(k, "  <r style=\"\" size=\"11\" font=\"sans\"/>\n");
        puts_(k, " </p>\n");
    }
    puts_(k, "</aodoc>\n");
    flush_stream(k);

    if (k->error) return k->error;
    if (k->full) return AO_E_FULL;
    if (len_out) *len_out = k->used;
    return AO_OK;
}

int ao_io_export(const ao_model_t *m, char *out, size_t cap, size_t *len_out) {
    if (!m || !out) return AO_E_ARG;
    sink_t k = { .p = out, .cap = cap };
    return io_export_sink(m, &k, len_out);
}

int ao_io_export_stream(const ao_model_t *m, ao_io_write_fn write,
                        void *context, size_t *len_out) {
    if (!m || !write) return AO_E_ARG;
    sink_t k = { .write = write, .context = context };
    return io_export_sink(m, &k, len_out);
}

/* ------------------------------------------------------------------ import */

#define AO_IO_MAX_RUN_BYTES (64u * 1024u)

static int token_name(const ao_xml_token_t *t, const char *s) {
    size_t n = strlen(s);
    return t->name_len == n && memcmp(t->name, s, n) == 0;
}

static int attr_name(const ao_xml_attr_t *a, const char *s) {
    size_t n = strlen(s);
    return a->name_len == n && memcmp(a->name, s, n) == 0;
}

static int xml_to_io_error(int rc) {
    if (rc == AO_XML_E_UTF8) return AO_E_UTF8;
    if (rc == AO_XML_E_FULL || rc == AO_XML_E_LIMIT) return AO_E_FULL;
    return AO_E_FORMAT;
}

static int decode_attr(const ao_xml_attr_t *a, char *out, size_t cap) {
    if (!a || !out || cap == 0) return AO_E_ARG;
    size_t n = 0;
    int rc = ao_xml_decode_native(a->value, a->value_len, out, cap - 1u, &n);
    if (rc != AO_XML_OK) return xml_to_io_error(rc);
    out[n] = '\0';
    return AO_OK;
}

static int parse_decimal(const char *s, uint32_t max, uint32_t *value) {
    if (!s || !*s || !value) return AO_E_FORMAT;
    uint32_t v = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < '0' || *p > '9') return AO_E_FORMAT;
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (max - d) / 10u) return AO_E_FORMAT;
        v = v * 10u + d;
    }
    *value = v;
    return AO_OK;
}

static int parse_para_attrs(const ao_xml_token_t *t, uint8_t *align,
                            uint8_t *style, int16_t *indent) {
    char value[AO_IO_MAX_ATTR];
    *align = AO_ALIGN_LEFT; *style = AO_PARA_NORMAL; *indent = 0;
    for (uint32_t i = 0; i < t->attr_count; i++) {
        const ao_xml_attr_t *a = &t->attrs[i];
        if (attr_name(a, "align")) {
            int rc = decode_attr(a, value, sizeof(value));
            if (rc != AO_OK) return rc;
            if (!strcmp(value, "left")) *align = AO_ALIGN_LEFT;
            else if (!strcmp(value, "center")) *align = AO_ALIGN_CENTER;
            else if (!strcmp(value, "right")) *align = AO_ALIGN_RIGHT;
            else if (!strcmp(value, "justify")) *align = AO_ALIGN_JUSTIFY;
            else return AO_E_FORMAT;
        } else if (attr_name(a, "pstyle")) {
            int rc = decode_attr(a, value, sizeof(value));
            if (rc != AO_OK) return rc;
            if (!strcmp(value, "normal")) *style = AO_PARA_NORMAL;
            else if (!strcmp(value, "h1")) *style = AO_PARA_H1;
            else if (!strcmp(value, "h2")) *style = AO_PARA_H2;
            else if (!strcmp(value, "h3")) *style = AO_PARA_H3;
            else if (!strcmp(value, "quote")) *style = AO_PARA_QUOTE;
            else if (!strcmp(value, "bullet")) *style = AO_PARA_BULLET;
            else if (!strcmp(value, "number")) *style = AO_PARA_NUMBER;
            else if (!strcmp(value, "table")) *style = AO_PARA_TABLE;
            else if (!strcmp(value, "image")) *style = AO_PARA_IMAGE;
            else return AO_E_FORMAT;
        } else if (attr_name(a, "indent")) {
            uint32_t v = 0;
            int rc = decode_attr(a, value, sizeof(value));
            if (rc == AO_OK) rc = parse_decimal(value, 32767u, &v);
            if (rc != AO_OK) return rc;
            *indent = (int16_t)v;
        } else if (attr_name(a, "pb")) {
            int rc = decode_attr(a, value, sizeof(value));
            if (rc != AO_OK) return rc;
            if (!strcmp(value, "1")) *style |= AO_PARA_PAGEBREAK;
            else if (strcmp(value, "0") != 0) return AO_E_FORMAT;
        }
        /* Unknown attributes are retained by the XML reader but ignored by
         * this version, matching the forward-compatible native XML contract. */
    }
    return AO_OK;
}

static int parse_run_attrs(const ao_xml_token_t *t, uint8_t *style,
                           uint8_t *size_px, uint8_t *font) {
    char value[AO_IO_MAX_ATTR];
    *style = AO_STYLE_OFF; *size_px = AO_SIZE_DEFAULT; *font = AO_FONT_SANS;
    for (uint32_t i = 0; i < t->attr_count; i++) {
        const ao_xml_attr_t *a = &t->attrs[i];
        if (attr_name(a, "style")) {
            int rc = decode_attr(a, value, sizeof(value));
            if (rc != AO_OK) return rc;
            for (const char *p = value; *p; p++) {
                if (*p == 'b') *style |= AO_STYLE_BOLD;
                else if (*p == 'i') *style |= AO_STYLE_ITALIC;
                else if (*p == 'u') *style |= AO_STYLE_UNDERLINE;
                else return AO_E_FORMAT;
            }
        } else if (attr_name(a, "size")) {
            uint32_t v = 0;
            int rc = decode_attr(a, value, sizeof(value));
            if (rc == AO_OK) rc = parse_decimal(value, 64u, &v);
            if (rc != AO_OK || v < 6u) return AO_E_FORMAT;
            *size_px = (uint8_t)v;
        } else if (attr_name(a, "font")) {
            int rc = decode_attr(a, value, sizeof(value));
            if (rc != AO_OK) return rc;
            if (!strcmp(value, "sans")) *font = AO_FONT_SANS;
            else if (!strcmp(value, "mono")) *font = AO_FONT_MONO;
            else return AO_E_FORMAT;
        }
    }
    return AO_OK;
}

static int all_xml_space(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
            return 0;
    return 1;
}

static int append_run_text(ao_model_t *m, const ao_xml_token_t *t,
                           int cdata, uint8_t style, uint8_t size_px,
                           uint8_t font, char *scratch, size_t scratch_cap) {
    size_t n = 0;
    int rc;
    if (cdata) {
        if (t->text_len > scratch_cap) return AO_E_FULL;
        if (t->text_len) memcpy(scratch, t->text, t->text_len);
        n = t->text_len;
        if (!ao_utf8_valid(scratch, n)) return AO_E_UTF8;
    } else {
        rc = ao_xml_decode_native(t->text, t->text_len, scratch,
                                  scratch_cap, &n);
        if (rc != AO_XML_OK) return xml_to_io_error(rc);
    }
    /* A paragraph is the newline boundary in the model; newline bytes inside
     * a run would silently change its structure during import. */
    if (memchr(scratch, '\n', n) || memchr(scratch, '\r', n)) return AO_E_FORMAT;
    if (n > UINT32_MAX) return AO_E_FULL;
    uint32_t at = ao_model_text_len(m);
    rc = ao_model_insert(m, at, scratch, (uint32_t)n);
    if (rc != AO_OK) return rc == AO_E_FULL ? AO_E_FULL : AO_E_FORMAT;
    if (n) {
        static const uint8_t bits[3] = {
            AO_STYLE_BOLD, AO_STYLE_ITALIC, AO_STYLE_UNDERLINE
        };
        for (size_t i = 0; i < 3; i++) {
            if (ao_model_set_style(m, at, (uint32_t)n, bits[i],
                                   (style & bits[i]) != 0) != AO_OK)
                return AO_E_FORMAT;
        }
        if (ao_model_set_size_font(m, at, (uint32_t)n, size_px, font) != AO_OK)
            return AO_E_FORMAT;
    }
    return AO_OK;
}

/* Parse the complete strict XML token stream into the model. The caller clears
 * the model on every failure, so no partial document can escape. */
static int import_xml(ao_model_t *m, const char *xml, size_t len) {
    ao_xml_reader_t reader;
    int rc = ao_xml_reader_init_native(&reader, xml, len);
    if (rc != AO_XML_OK) return xml_to_io_error(rc);

    char scratch[AO_IO_MAX_RUN_BYTES];
    ao_xml_token_t t;
    int seen_root = 0, root_open = 0, root_closed = 0;
    int in_para = 0, in_run = 0, seen_version = 0;
    uint32_t para_count = 0;
    uint8_t run_style = AO_STYLE_OFF, run_size = AO_SIZE_DEFAULT;
    uint8_t run_font = AO_FONT_SANS;

    for (;;) {
        rc = ao_xml_next(&reader, &t);
        if (rc != AO_XML_OK) return xml_to_io_error(rc);
        if (t.type == AO_XML_T_EOF) break;

        if (t.type == AO_XML_T_TEXT) {
            if (in_run) {
                rc = append_run_text(m, &t, 0, run_style, run_size, run_font,
                                     scratch, sizeof(scratch));
                if (rc != AO_OK) return rc;
            } else if (!all_xml_space(t.text, t.text_len)) {
                return AO_E_FORMAT;
            }
            continue;
        }
        if (t.type == AO_XML_T_CDATA) {
            if (!in_run) return AO_E_FORMAT;
            rc = append_run_text(m, &t, 1, run_style, run_size, run_font,
                                 scratch, sizeof(scratch));
            if (rc != AO_OK) return rc;
            continue;
        }
        if (t.type == AO_XML_T_END) {
            if (in_run && token_name(&t, "r")) { in_run = 0; continue; }
            if (in_para && token_name(&t, "p")) { in_para = 0; continue; }
            if (root_open && !in_para && token_name(&t, "aodoc")) {
                root_open = 0; root_closed = 1; continue;
            }
            return AO_E_FORMAT;
        }
        if (t.type != AO_XML_T_START) return AO_E_FORMAT;

        if (!seen_root) {
            if (!token_name(&t, "aodoc")) return AO_E_FORMAT;
            seen_root = 1;
            for (uint32_t i = 0; i < t.attr_count; i++) {
                if (!attr_name(&t.attrs[i], "version")) continue;
                char value[AO_IO_MAX_ATTR];
                rc = decode_attr(&t.attrs[i], value, sizeof(value));
                if (rc != AO_OK) return rc;
                if (strcmp(value, "1") != 0) return AO_E_FORMAT;
                seen_version = 1;
            }
            if (!seen_version) return AO_E_FORMAT;
            if (t.empty) root_closed = 1;
            else root_open = 1;
            continue;
        }
        if (!root_open || root_closed || in_run) return AO_E_FORMAT;

        if (token_name(&t, "p")) {
            if (in_para) return AO_E_FORMAT;
            uint8_t align = AO_ALIGN_LEFT, pstyle = AO_PARA_NORMAL;
            int16_t indent = 0;
            rc = parse_para_attrs(&t, &align, &pstyle, &indent);
            if (rc != AO_OK) return rc;
            if (para_count) {
                rc = ao_model_insert(m, ao_model_text_len(m), "\n", 1);
                if (rc != AO_OK) return rc == AO_E_FULL ? AO_E_FULL : AO_E_FORMAT;
            }
            if (para_count >= m->para_cap) return AO_E_FULL;
            uint32_t pi = para_count;
            uint32_t pos = m->paras[pi].off;
            if (ao_model_set_align(m, pos, align) != AO_OK ||
                ao_model_set_para_style(m, pos, pstyle) != AO_OK ||
                ao_model_set_indent(m, pos, indent) != AO_OK)
                return AO_E_FORMAT;
            para_count++;
            if (t.empty) in_para = 0;
            else in_para = 1;
            continue;
        }
        if (token_name(&t, "r")) {
            if (!in_para) return AO_E_FORMAT;
            rc = parse_run_attrs(&t, &run_style, &run_size, &run_font);
            if (rc != AO_OK) return rc;
            if (t.empty) in_run = 0;
            else in_run = 1;
            continue;
        }
        return AO_E_FORMAT;
    }

    if (!seen_root || !root_closed || root_open || in_para || in_run ||
        para_count == 0 || ao_model_para_count(m) != para_count)
        return AO_E_FORMAT;
    const char *why = NULL;
    if (!ao_model_valid(m, &why)) return AO_E_FORMAT;
    return AO_OK;
}

int ao_io_import(ao_model_t *m, const char *xml, size_t len) {
    if (!m || !xml) return AO_E_ARG;
    if (len == 0) len = strlen(xml);
    ao_model_clear(m);
    int rc = import_xml(m, xml, len);
    if (rc != AO_OK) ao_model_clear(m);
    return rc;
}
