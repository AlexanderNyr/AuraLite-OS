/* userspace/office/libaodoc/aoxml.c — bounded pull XML reader and escaper. */
#include "aoxml.h"

#include <string.h>

#include "aodoc.h"

static int xml_char_ok(uint32_t cp, int native_mark) {
    if (native_mark && cp == 1u) return 1;
    return cp == 0x09u || cp == 0x0Au || cp == 0x0Du ||
           (cp >= 0x20u && cp <= 0xD7FFu) ||
           (cp >= 0xE000u && cp <= 0xFFFDu) ||
           (cp >= 0x10000u && cp <= 0x10FFFFu);
}

static int xml_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int name_start(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '_' || c == ':';
}

static int name_char(unsigned char c) {
    return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static void advance(ao_xml_reader_t *r) {
    if (r->pos >= r->len) return;
    unsigned char c = (unsigned char)r->src[r->pos++];
    if (c == '\n') { r->line++; r->col = 1; }
    else if ((c & 0xC0u) != 0x80u) r->col++;
}

static int starts(const ao_xml_reader_t *r, const char *s, size_t n) {
    return r->pos <= r->len && n <= r->len - r->pos &&
           memcmp(r->src + r->pos, s, n) == 0;
}

static void skip_space(ao_xml_reader_t *r) {
    while (r->pos < r->len && xml_space(r->src[r->pos])) advance(r);
}

static int contains_bytes(const char *s, size_t n, const char *needle,
                          size_t needle_len) {
    if (!needle_len) return 1;
    if (needle_len > n) return 0;
    for (size_t i = 0; i <= n - needle_len; i++)
        if (memcmp(s + i, needle, needle_len) == 0) return 1;
    return 0;
}

static int raw_xml_chars(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        int k = ao_utf8_decode(s + i, n - i, &cp);
        if (k <= 0) return AO_XML_E_UTF8;
        if (!xml_char_ok(cp, 0)) return AO_XML_E_FORMAT;
        i += (size_t)k;
    }
    return AO_XML_OK;
}

/* Decode one numeric or predefined entity.  `i` points just after '&'. */
static int entity(const char *s, size_t n, size_t *i, uint32_t *cp,
                  int native_mark) {
    size_t p = *i;
    if (p >= n) return AO_XML_E_FORMAT;
    if (s[p] == '#') {
        p++;
        unsigned base = 10;
        if (p < n && (s[p] == 'x' || s[p] == 'X')) { base = 16; p++; }
        size_t digits = 0;
        uint32_t v = 0;
        while (p < n && s[p] != ';') {
            unsigned d;
            unsigned char c = (unsigned char)s[p];
            if (c >= '0' && c <= '9') d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10u;
            else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10u;
            else return AO_XML_E_FORMAT;
            if (d >= base || v > (0x10FFFFu - d) / base)
                return AO_XML_E_FORMAT;
            v = v * base + d;
            digits++; p++;
        }
        if (!digits || p >= n || s[p] != ';' || !xml_char_ok(v, native_mark))
            return AO_XML_E_FORMAT;
        *i = p + 1; *cp = v;
        return AO_XML_OK;
    }
    size_t start = p;
    while (p < n && s[p] != ';' && p - start < 8) p++;
    if (p >= n || s[p] != ';') return AO_XML_E_FORMAT;
    size_t k = p - start;
    const char *name = s + start;
    if (k == 3 && memcmp(name, "amp", 3) == 0) *cp = '&';
    else if (k == 2 && memcmp(name, "lt", 2) == 0) *cp = '<';
    else if (k == 2 && memcmp(name, "gt", 2) == 0) *cp = '>';
    else if (k == 4 && memcmp(name, "quot", 4) == 0) *cp = '"';
    else if (k == 4 && memcmp(name, "apos", 4) == 0) *cp = '\'';
    else return AO_XML_E_FORMAT;
    *i = p + 1;
    return AO_XML_OK;
}

static int scan_value(const char *src, size_t len, int native_mark,
                      char *out, size_t cap, size_t *out_len) {
    if ((!src && len) || (!out && cap)) return AO_XML_E_ARG;
    size_t i = 0, used = 0;
    while (i < len) {
        uint32_t cp = 0;
        if (src[i] == '&') {
            i++;
            int rc = entity(src, len, &i, &cp, native_mark);
            if (rc != AO_XML_OK) return rc;
        } else {
            int n = ao_utf8_decode(src + i, len - i, &cp);
            if (n <= 0) return AO_XML_E_UTF8;
            i += (size_t)n;
            /* Native compatibility is deliberately limited to the legacy
             * numeric entity &#1;, never a raw XML 1.0 control character. */
            if (!xml_char_ok(cp, 0)) return AO_XML_E_FORMAT;
        }
        char enc[4];
        int n = ao_utf8_encode(cp, enc);
        if (n <= 0) return AO_XML_E_UTF8;
        if (out) {
            if ((size_t)n > cap - used) return AO_XML_E_FULL;
            memcpy(out + used, enc, (size_t)n);
        }
        used += (size_t)n;
    }
    if (out_len) *out_len = used;
    return AO_XML_OK;
}

int ao_xml_decode(const char *src, size_t len, char *out, size_t cap,
                  size_t *out_len) {
    return scan_value(src, len, 0, out, cap, out_len);
}

int ao_xml_decode_native(const char *src, size_t len, char *out, size_t cap,
                         size_t *out_len) {
    return scan_value(src, len, 1, out, cap, out_len);
}

static int xml_init(ao_xml_reader_t *r, const char *src, size_t len,
                    int native_mark) {
    if (!r || !src) return AO_XML_E_ARG;
    if (len > AO_XML_MAX_INPUT_BYTES) return AO_XML_E_LIMIT;
    memset(r, 0, sizeof(*r));
    r->src = src; r->len = len; r->line = 1; r->col = 1;
    r->allow_legacy_merge_mark = (uint8_t)(native_mark != 0);
    if (len >= 3 && (uint8_t)src[0] == 0xEFu &&
        (uint8_t)src[1] == 0xBBu && (uint8_t)src[2] == 0xBFu) {
        r->pos = 3; /* A UTF-8 BOM is not part of the first token's column. */
    }
    return AO_XML_OK;
}

int ao_xml_reader_init(ao_xml_reader_t *r, const char *src, size_t len) {
    return xml_init(r, src, len, 0);
}

int ao_xml_reader_init_native(ao_xml_reader_t *r, const char *src, size_t len) {
    return xml_init(r, src, len, 1);
}

static int read_name(ao_xml_reader_t *r, const char **name, size_t *len) {
    size_t start = r->pos;
    if (start >= r->len || !name_start((unsigned char)r->src[start]))
        return AO_XML_E_FORMAT;
    advance(r);
    while (r->pos < r->len && name_char((unsigned char)r->src[r->pos])) {
        if (r->pos - start >= AO_XML_MAX_NAME) return AO_XML_E_LIMIT;
        advance(r);
    }
    *name = r->src + start;
    *len = r->pos - start;
    return AO_XML_OK;
}

static int value_valid(ao_xml_reader_t *r, const char *s, size_t n) {
    return scan_value(s, n, r->allow_legacy_merge_mark, NULL, 0, NULL);
}

static int skip_until(ao_xml_reader_t *r, const char *end, size_t end_len,
                      int validate_chars) {
    size_t start = r->pos;
    while (r->pos + end_len <= r->len && !starts(r, end, end_len))
        advance(r);
    if (r->pos + end_len > r->len) return AO_XML_E_FORMAT;
    if (validate_chars) {
        int rc = raw_xml_chars(r->src + start, r->pos - start);
        if (rc != AO_XML_OK) return rc;
    }
    for (size_t i = 0; i < end_len; i++) advance(r);
    return AO_XML_OK;
}

static int text_outside_root_is_space(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (!xml_space(s[i])) return 0;
    return 1;
}

int ao_xml_next(ao_xml_reader_t *r, ao_xml_token_t *t) {
    if (!r || !t || !r->src) return AO_XML_E_ARG;
    if (r->failed) return AO_XML_E_FORMAT;
    memset(t, 0, sizeof(*t));
    if (r->eof_seen) { t->type = AO_XML_T_EOF; return AO_XML_OK; }

    for (;;) {
        if (r->pos >= r->len) {
            if (r->depth || !r->root_seen || !r->root_closed) {
                r->failed = 1; return AO_XML_E_FORMAT;
            }
            r->eof_seen = 1; t->type = AO_XML_T_EOF;
            t->line = r->line; t->col = r->col;
            return AO_XML_OK;
        }
        uint32_t line = r->line, col = r->col;
        if (r->src[r->pos] != '<') {
            size_t start = r->pos;
            while (r->pos < r->len && r->src[r->pos] != '<') advance(r);
            size_t n = r->pos - start;
            if (memchr(r->src + start, '\0', n) ||
                contains_bytes(r->src + start, n, "]]>", 3)) {
                r->failed = 1; return AO_XML_E_FORMAT;
            }
            int rc = value_valid(r, r->src + start, n);
            if (rc != AO_XML_OK) { r->failed = 1; return rc; }
            if (!r->depth && !text_outside_root_is_space(r->src + start, n)) {
                r->failed = 1; return AO_XML_E_FORMAT;
            }
            t->type = AO_XML_T_TEXT;
            t->text = r->src + start; t->text_len = n;
            t->line = line; t->col = col;
            return AO_XML_OK;
        }

        if (starts(r, "<!--", 4)) {
            for (int i = 0; i < 4; i++) advance(r);
            size_t start = r->pos;
            int found = 0;
            while (r->pos + 2 < r->len) {
                if (starts(r, "-->", 3)) { found = 1; break; }
                if (starts(r, "--", 2)) { r->failed = 1; return AO_XML_E_FORMAT; }
                advance(r);
            }
            if (!found) { r->failed = 1; return AO_XML_E_FORMAT; }
            size_t n = r->pos - start;
            int rc = raw_xml_chars(r->src + start, n);
            if (rc != AO_XML_OK) { r->failed = 1; return rc; }
            for (int i = 0; i < 3; i++) advance(r);
            continue;
        }
        if (starts(r, "<?", 2)) {
            advance(r); advance(r);
            int rc = skip_until(r, "?>", 2, 1);
            if (rc != AO_XML_OK) { r->failed = 1; return rc; }
            continue;
        }
        if (starts(r, "<![CDATA[", 9)) {
            if (!r->depth) { r->failed = 1; return AO_XML_E_FORMAT; }
            for (int i = 0; i < 9; i++) advance(r);
            size_t start = r->pos;
            int found = 0;
            while (r->pos + 2 < r->len) {
                if (starts(r, "]]>", 3)) { found = 1; break; }
                advance(r);
            }
            if (!found) { r->failed = 1; return AO_XML_E_FORMAT; }
            size_t n = r->pos - start;
            int rc = raw_xml_chars(r->src + start, n);
            if (rc != AO_XML_OK) { r->failed = 1; return rc; }
            for (int i = 0; i < 3; i++) advance(r);
            t->type = AO_XML_T_CDATA; t->text = r->src + start; t->text_len = n;
            t->line = line; t->col = col;
            return AO_XML_OK;
        }
        /* DTDs, entity declarations and every other declaration are refused. */
        if (starts(r, "<!", 2)) { r->failed = 1; return AO_XML_E_FORMAT; }

        advance(r); /* '<' */
        if (r->pos < r->len && r->src[r->pos] == '/') {
            advance(r);
            const char *name = NULL; size_t nlen = 0;
            int rc = read_name(r, &name, &nlen);
            if (rc != AO_XML_OK) { r->failed = 1; return rc; }
            skip_space(r);
            if (r->pos >= r->len || r->src[r->pos] != '>' || r->depth == 0) {
                r->failed = 1; return AO_XML_E_FORMAT;
            }
            uint32_t idx = r->depth - 1u;
            if (r->stack_len[idx] != nlen ||
                memcmp(r->stack_names[idx], name, nlen) != 0) {
                r->failed = 1; return AO_XML_E_FORMAT;
            }
            advance(r); r->depth--;
            if (r->depth == 0) r->root_closed = 1;
            t->type = AO_XML_T_END; t->name = name; t->name_len = nlen;
            t->line = line; t->col = col;
            return AO_XML_OK;
        }

        const char *name = NULL; size_t nlen = 0;
        int rc = read_name(r, &name, &nlen);
        if (rc != AO_XML_OK) { r->failed = 1; return rc; }
        if (!r->depth) {
            if (r->root_closed || r->root_seen) { r->failed = 1; return AO_XML_E_FORMAT; }
            r->root_seen = 1;
        }
        t->type = AO_XML_T_START; t->name = name; t->name_len = nlen;
        t->line = line; t->col = col;

        for (;;) {
            size_t before = r->pos;
            skip_space(r);
            int ws_seen = r->pos != before;
            if (r->pos >= r->len) { r->failed = 1; return AO_XML_E_FORMAT; }
            if (r->src[r->pos] == '>') { advance(r); break; }
            if (r->src[r->pos] == '/') {
                advance(r);
                if (r->pos >= r->len || r->src[r->pos] != '>') {
                    r->failed = 1; return AO_XML_E_FORMAT;
                }
                advance(r); t->empty = 1; break;
            }
            if (!ws_seen || t->attr_count >= AO_XML_MAX_ATTRS) {
                r->failed = 1;
                return t->attr_count >= AO_XML_MAX_ATTRS ? AO_XML_E_LIMIT : AO_XML_E_FORMAT;
            }
            const char *aname = NULL; size_t alen = 0;
            rc = read_name(r, &aname, &alen);
            if (rc != AO_XML_OK) { r->failed = 1; return rc; }
            for (uint32_t ai = 0; ai < t->attr_count; ai++) {
                if (t->attrs[ai].name_len == alen &&
                    memcmp(t->attrs[ai].name, aname, alen) == 0) {
                    r->failed = 1; return AO_XML_E_FORMAT;
                }
            }
            skip_space(r);
            if (r->pos >= r->len || r->src[r->pos] != '=') {
                r->failed = 1; return AO_XML_E_FORMAT;
            }
            advance(r); skip_space(r);
            if (r->pos >= r->len ||
                (r->src[r->pos] != '\'' && r->src[r->pos] != '"')) {
                r->failed = 1; return AO_XML_E_FORMAT;
            }
            char quote = r->src[r->pos]; advance(r);
            size_t vstart = r->pos;
            while (r->pos < r->len && r->src[r->pos] != quote) {
                if (r->src[r->pos] == '<') { r->failed = 1; return AO_XML_E_FORMAT; }
                advance(r);
                if (r->pos - vstart > AO_XML_MAX_ATTR_BYTES) {
                    r->failed = 1; return AO_XML_E_LIMIT;
                }
            }
            if (r->pos >= r->len) { r->failed = 1; return AO_XML_E_FORMAT; }
            size_t vlen = r->pos - vstart;
            rc = value_valid(r, r->src + vstart, vlen);
            if (rc != AO_XML_OK) { r->failed = 1; return rc; }
            advance(r);
            ao_xml_attr_t *a = &t->attrs[t->attr_count++];
            a->name = aname; a->name_len = alen;
            a->value = r->src + vstart; a->value_len = vlen;
        }

        if (t->empty) {
            if (!r->depth) r->root_closed = 1;
        } else {
            if (r->depth >= AO_XML_MAX_DEPTH) { r->failed = 1; return AO_XML_E_LIMIT; }
            uint32_t idx = r->depth++;
            r->stack_len[idx] = (uint16_t)nlen;
            memcpy(r->stack_names[idx], name, nlen);
            r->stack_names[idx][nlen] = 0;
        }
        return AO_XML_OK;
    }
}

typedef struct {
    char *out;
    size_t cap, used;
    ao_xml_write_fn write;
    void *context;
    char pending[256];
    size_t pending_len;
} xml_escape_sink_t;

static int xml_escape_flush(xml_escape_sink_t *sink) {
    if (!sink->write || !sink->pending_len) return AO_XML_OK;
    if (sink->write(sink->context, sink->pending, sink->pending_len) != 0)
        return AO_XML_E_WRITE;
    sink->pending_len = 0;
    return AO_XML_OK;
}

static int xml_escape_emit(xml_escape_sink_t *sink, const char *bytes,
                           size_t length) {
    if (length > SIZE_MAX - sink->used) return AO_XML_E_LIMIT;
    if (sink->write) {
        if (length > sizeof(sink->pending) - sink->pending_len) {
            int rc = xml_escape_flush(sink);
            if (rc != AO_XML_OK) return rc;
        }
        if (length > sizeof(sink->pending)) return AO_XML_E_LIMIT;
        if (length) memcpy(sink->pending + sink->pending_len, bytes, length);
        sink->pending_len += length;
    } else if (sink->out) {
        if (length > sink->cap - sink->used) return AO_XML_E_FULL;
        if (length) memcpy(sink->out + sink->used, bytes, length);
    }
    sink->used += length;
    return AO_XML_OK;
}

static int xml_escape_core(const char *src, size_t len, int attribute,
                           xml_escape_sink_t *sink, size_t *out_len) {
    if ((!src && len) || (!sink->out && !sink->write && sink->cap))
        return AO_XML_E_ARG;
    (void)attribute; /* Both quote characters are escaped canonically. */
    for (size_t i = 0; i < len;) {
        uint32_t cp = 0;
        int k = ao_utf8_decode(src + i, len - i, &cp);
        if (k <= 0) return AO_XML_E_UTF8;
        if (!xml_char_ok(cp, 0)) return AO_XML_E_FORMAT;
        const char *rep = src + i; size_t rn = (size_t)k;
        if (cp == '&') { rep = "&amp;"; rn = 5; }
        else if (cp == '<') { rep = "&lt;"; rn = 4; }
        else if (cp == '>') { rep = "&gt;"; rn = 4; }
        else if (cp == '"') { rep = "&quot;"; rn = 6; }
        else if (cp == '\'') { rep = "&apos;"; rn = 6; }
        int rc = xml_escape_emit(sink, rep, rn);
        if (rc != AO_XML_OK) return rc;
        i += (size_t)k;
    }
    int rc = xml_escape_flush(sink);
    if (rc != AO_XML_OK) return rc;
    if (out_len) *out_len = sink->used;
    return AO_XML_OK;
}

int ao_xml_escape(const char *src, size_t len, int attribute,
                  char *out, size_t cap, size_t *out_len) {
    if (!out && cap) return AO_XML_E_ARG;
    xml_escape_sink_t sink = { .out = out, .cap = cap };
    return xml_escape_core(src, len, attribute, &sink, out_len);
}

int ao_xml_escape_stream(const char *src, size_t len, int attribute,
                         ao_xml_write_fn write, void *context,
                         size_t *out_len) {
    if (!write) return AO_XML_E_ARG;
    xml_escape_sink_t sink = { .write = write, .context = context };
    return xml_escape_core(src, len, attribute, &sink, out_len);
}

static int xml_validate_mode(const char *src, size_t len, int native_mark) {
    ao_xml_reader_t r;
    int rc = native_mark ? ao_xml_reader_init_native(&r, src, len)
                         : ao_xml_reader_init(&r, src, len);
    if (rc != AO_XML_OK) return rc;
    ao_xml_token_t t;
    for (;;) {
        rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return rc;
        if (t.type == AO_XML_T_EOF) return AO_XML_OK;
    }
}

int ao_xml_validate(const char *src, size_t len) {
    return xml_validate_mode(src, len, 0);
}

int ao_xml_validate_native(const char *src, size_t len) {
    return xml_validate_mode(src, len, 1);
}
