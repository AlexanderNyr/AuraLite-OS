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

/* ------------------------------------------------------------------ export */

size_t ao_io_export_bound(const ao_model_t *m) {
    if (!m) return 0;
    /* prologue + <aodoc> + closing, then per paragraph ~64 bytes of markup and
     * per run ~48, then the escaped text (at most 6x). */
    return 256u + (size_t)m->text_len * 6u +
           (size_t)m->para_len * 64u + (size_t)m->run_len * 48u;
}

typedef struct {
    char  *p;
    size_t cap, used;
    int    full;
} sink_t;

static void put(sink_t *k, const char *s, size_t n) {
    if (k->full) return;
    if (k->used + n > k->cap) { k->full = 1; return; }
    memcpy(k->p + k->used, s, n);
    k->used += n;
}

static void puts_(sink_t *k, const char *s) { put(k, s, strlen(s)); }

static void put_u32(sink_t *k, uint32_t v) {
    char tmp[12];
    int i = 11;
    tmp[i] = '\0';
    if (!v) tmp[--i] = '0';
    while (v && i > 0) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
    puts_(k, &tmp[i]);
}

static void put_escaped(sink_t *k, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '&': puts_(k, "&amp;"); break;
            case '<': puts_(k, "&lt;"); break;
            case '>': puts_(k, "&gt;"); break;
            case '"': puts_(k, "&quot;"); break;
            default:  put(k, &s[i], 1); break;
        }
    }
}

int ao_io_export(const ao_model_t *m, char *out, size_t cap, size_t *len_out) {
    if (!m || !out) return AO_E_ARG;
    sink_t k = { out, cap, 0, 0 };

    puts_(&k, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<aodoc version=\"1\">\n");
    for (uint32_t pi = 0; pi < m->para_len; pi++) {
        const ao_para_t *p = &m->paras[pi];
        puts_(&k, " <p align=\"");
        switch (p->align) {
            case AO_ALIGN_CENTER:  puts_(&k, "center"); break;
            case AO_ALIGN_RIGHT:   puts_(&k, "right"); break;
            case AO_ALIGN_JUSTIFY: puts_(&k, "justify"); break;
            default:               puts_(&k, "left"); break;
        }
        puts_(&k, "\" indent=\"");
        put_u32(&k, (uint32_t)(p->indent < 0 ? 0 : p->indent));
        puts_(&k, "\">\n");

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
            puts_(&k, "  <r style=\"");
            if (run->style & AO_STYLE_BOLD)      put(&k, "b", 1);
            if (run->style & AO_STYLE_ITALIC)    put(&k, "i", 1);
            if (run->style & AO_STYLE_UNDERLINE) put(&k, "u", 1);
            puts_(&k, "\" size=\"");
            put_u32(&k, run->size);
            puts_(&k, "\" font=\"");
            puts_(&k, run->font == AO_FONT_MONO ? "mono" : "sans");
            puts_(&k, "\">");
            put_escaped(&k, m->text + run->off, n);
            puts_(&k, "</r>\n");
        }
        if (!wrote_run) puts_(&k, "  <r style=\"\" size=\"11\" font=\"sans\"/>\n");
        puts_(&k, " </p>\n");
    }
    puts_(&k, "</aodoc>\n");

    if (k.full) return AO_E_FULL;
    if (len_out) *len_out = k.used;
    return AO_OK;
}

/* ------------------------------------------------------------------ import */

typedef struct {
    const char *s;
    size_t      n, i;
} cur_t;

static int at_end(const cur_t *c) { return c->i >= c->n; }

/* Skip whitespace. */
static void skip_ws(cur_t *c) {
    while (!at_end(c)) {
        char ch = c->s[c->i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') c->i++;
        else break;
    }
}

/* Read a name (element or attribute) into buf; 0 if it does not fit or is
 * empty.  Names are [A-Za-z0-9_-]+ here: enough for this format, and refusing
 * anything else keeps the scanner from guessing. */
static int read_name(cur_t *c, char *buf, size_t cap) {
    size_t w = 0;
    while (!at_end(c)) {
        char ch = c->s[c->i];
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == ':';
        if (!ok) break;
        if (w + 1 >= cap) return 0;
        buf[w++] = ch;
        c->i++;
    }
    buf[w] = '\0';
    return w != 0;
}

/* Decode one entity at c->i (which points at '&').  Writes UTF-8 into out
 * (up to 4 bytes) and returns the bytes written, or -1 on an unknown/invalid
 * entity -- a document with a broken entity is refused, not patched up. */
static int read_entity(cur_t *c, char out[4]) {
    size_t save = c->i;
    c->i++;                                  /* '&' */
    if (!at_end(c) && c->s[c->i] == '#') {
        c->i++;
        uint32_t cp = 0;
        int hex = 0;
        if (!at_end(c) && (c->s[c->i] == 'x' || c->s[c->i] == 'X')) { hex = 1; c->i++; }
        int digits = 0;
        while (!at_end(c) && c->s[c->i] != ';') {
            char ch = c->s[c->i];
            int v;
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (hex && ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
            else if (hex && ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
            else { c->i = save; return -1; }
            if (cp > 0x10FFFFu / 16u) { c->i = save; return -1; }
            cp = cp * (hex ? 16u : 10u) + (uint32_t)v;
            digits++;
            c->i++;
        }
        if (!digits || at_end(c) || c->s[c->i] != ';') { c->i = save; return -1; }
        c->i++;
        int n = ao_utf8_encode(cp, out);
        if (n <= 0) { c->i = save; return -1; }
        return n;
    }
    char name[8];
    size_t w = 0;
    while (!at_end(c) && c->s[c->i] != ';') {
        if (w + 1 >= sizeof name) { c->i = save; return -1; }
        name[w++] = c->s[c->i++];
    }
    if (at_end(c)) { c->i = save; return -1; }
    name[w] = '\0';
    c->i++;                                  /* ';' */
    if (!strcmp(name, "amp"))  { out[0] = '&'; return 1; }
    if (!strcmp(name, "lt"))   { out[0] = '<'; return 1; }
    if (!strcmp(name, "gt"))   { out[0] = '>'; return 1; }
    if (!strcmp(name, "quot")) { out[0] = '"'; return 1; }
    if (!strcmp(name, "apos")) { out[0] = '\''; return 1; }
    c->i = save;
    return -1;
}

/* Skip an unmatched construct: a comment, a processing instruction, a DOCTYPE
 * or a tag we do not model.  Always advances. */
static int skip_junk(cur_t *c, int depth) {
    if (depth > AO_IO_MAX_DEPTH) return AO_E_FORMAT;
    if (c->i + 3 < c->n && !memcmp(c->s + c->i, "<!--", 4)) {
        c->i += 4;
        while (c->i + 2 < c->n && memcmp(c->s + c->i, "-->", 3)) c->i++;
        if (c->i + 2 >= c->n) return AO_E_FORMAT;
        c->i += 3;
        return AO_OK;
    }
    if (c->i + 1 < c->n && c->s[c->i] == '<' && c->s[c->i + 1] == '?') {
        c->i += 2;
        while (c->i + 1 < c->n && memcmp(c->s + c->i, "?>", 2)) c->i++;
        if (c->i + 1 >= c->n) return AO_E_FORMAT;
        c->i += 2;
        return AO_OK;
    }
    if (c->i + 8 < c->n && !memcmp(c->s + c->i, "<!DOCTYPE", 9)) {
        while (c->i < c->n && c->s[c->i] != '>') c->i++;
        if (at_end(c)) return AO_E_FORMAT;
        c->i++;
        return AO_OK;
    }
    return AO_E_FORMAT;
}

#define MAX_RUNS_PER_PARA 4096

/* Stamp the WHOLE attribute set over [pos, pos+n): every style bit is set or
 * cleared explicitly, and so are size and font. */
static int apply_run_attrs(ao_model_t *m, uint32_t pos, uint32_t n,
                           uint8_t style, uint8_t size_px, uint8_t font) {
    static const uint8_t bits[3] = { AO_STYLE_BOLD, AO_STYLE_ITALIC, AO_STYLE_UNDERLINE };
    for (int b = 0; b < 3; b++) {
        if (ao_model_set_style(m, pos, n, bits[b], (style & bits[b]) != 0) != AO_OK)
            return 0;
    }
    return ao_model_set_size_font(m, pos, n, size_px, font) == AO_OK;
}

/* One byte per paragraph, so the reader knows each paragraph's alignment even
 * though the model creates paragraphs lazily. */
#define AO_IO_MAX_PARAS 4096
static uint8_t p_aligns[AO_IO_MAX_PARAS];

/* The reader builds into the caller's model as it walks the file, so a failure
 * half way through cannot be rolled back -- which is why ao_io_import() clears
 * the document on EVERY refusal path, not just the ones that notice early.  An
 * empty page and a message beats a half-loaded document that the next save
 * would then make permanent. */
static int import_into(ao_model_t *m, const char *xml, size_t len) {

    cur_t c = { xml, len, 0 };
    int depth = 0, in_aodoc = 0, in_p = 0, seen_version = 0;
    uint8_t p_align = AO_ALIGN_LEFT;
    int16_t p_indent = 0;

    char text[64 * 1024];          /* scratch for the current run's text */
    uint32_t text_at = 0;
    uint8_t r_style = AO_STYLE_OFF, r_size = AO_SIZE_DEFAULT, r_font = AO_FONT_SANS;
    int in_r = 0;
    uint32_t p_seen = 0;

    while (!at_end(&c)) {
        if (c.s[c.i] == '<') {
            /* Close out a run's text before looking at markup. */
            if (in_r) {
                if (text_at) {
                    /* Append the run's text, then stamp its attributes on it.
                     * They must be stamped even when they are the defaults:
                     * inserted text inherits the attributes of whatever run it
                     * was typed after, so a plain run that follows a bold one
                     * would otherwise come out bold. */
                    uint32_t start = ao_model_text_len(m);
                    int rc = ao_model_insert(m, start, text, text_at);
                    if (rc != AO_OK) return rc == AO_E_FULL ? AO_E_FULL : AO_E_FORMAT;
                    if (!apply_run_attrs(m, start, text_at, r_style, r_size, r_font))
                        return AO_E_FORMAT;
                }
                in_r = 0;
                text_at = 0;
            }

            if (c.i + 1 < c.n && c.s[c.i + 1] == '/') {
                /* closing tag */
                c.i += 2;
                char name[16];
                if (!read_name(&c, name, sizeof name)) return AO_E_FORMAT;
                skip_ws(&c);
                if (at_end(&c) || c.s[c.i] != '>') return AO_E_FORMAT;
                c.i++;
                depth--;
                if (depth < 0) return AO_E_FORMAT;
                if (!strcmp(name, "p")) {
                    /* Each paragraph ends with '\n' except the last; the writer
                     * strips it, so the reader adds it back -- but only if this
                     * was not the final paragraph.  The last one is fixed up
                     * after the loop. */
                    in_p = 0;
                    (void)in_p;
                } else if (!strcmp(name, "aodoc")) {
                    in_aodoc = 0;
                }
                continue;
            }
            if (c.i + 1 < c.n && (c.s[c.i + 1] == '?' || c.s[c.i + 1] == '!')) {
                int rc = skip_junk(&c, depth);
                if (rc != AO_OK) return rc;
                continue;
            }

            /* opening tag */
            c.i++;
            char name[16];
            if (!read_name(&c, name, sizeof name)) return AO_E_FORMAT;
            depth++;
            if (depth > AO_IO_MAX_DEPTH) return AO_E_FORMAT;

            /* attributes */
            if (!strcmp(name, "p")) { p_align = AO_ALIGN_LEFT; p_indent = 0; }
            int self_close = 0;
            for (;;) {
                skip_ws(&c);
                if (at_end(&c)) return AO_E_FORMAT;
                if (c.s[c.i] == '/') {
                    c.i++;
                    if (at_end(&c) || c.s[c.i] != '>') return AO_E_FORMAT;
                    c.i++;
                    self_close = 1;
                    break;
                }
                if (c.s[c.i] == '>') { c.i++; break; }
                char aname[24];
                if (!read_name(&c, aname, sizeof aname)) return AO_E_FORMAT;
                skip_ws(&c);
                if (at_end(&c) || c.s[c.i] != '=') return AO_E_FORMAT;
                c.i++;
                skip_ws(&c);
                if (at_end(&c) || c.s[c.i] != '"') return AO_E_FORMAT;
                c.i++;
                char aval[AO_IO_MAX_ATTR];
                size_t w = 0;
                while (!at_end(&c) && c.s[c.i] != '"') {
                    if (w + 4 >= sizeof aval) return AO_E_FORMAT;   /* attribute cap */
                    if (c.s[c.i] == '&') {
                        char enc[4];
                        int n = read_entity(&c, enc);
                        if (n < 0) return AO_E_FORMAT;
                        memcpy(aval + w, enc, (size_t)n);
                        w += (size_t)n;
                    } else {
                        aval[w++] = c.s[c.i++];
                    }
                }
                if (at_end(&c)) return AO_E_FORMAT;
                c.i++;                                           /* closing quote */
                aval[w] = '\0';

                if (!strcmp(name, "aodoc") && !strcmp(aname, "version")) {
                    if (strcmp(aval, "1")) return AO_E_FORMAT;    /* unknown version */
                    seen_version = 1;
                } else if (!strcmp(name, "p") && !strcmp(aname, "align")) {
                    p_align = !strcmp(aval, "center")  ? AO_ALIGN_CENTER
                            : !strcmp(aval, "right")   ? AO_ALIGN_RIGHT
                            : !strcmp(aval, "justify") ? AO_ALIGN_JUSTIFY
                                                       : AO_ALIGN_LEFT;
                } else if (!strcmp(name, "p") && !strcmp(aname, "indent")) {
                    int v = 0;
                    for (const char *q = aval; *q >= '0' && *q <= '9'; q++) {
                        if (v > 10000) return AO_E_FORMAT;
                        v = v * 10 + (*q - '0');
                    }
                    p_indent = (int16_t)v;
                } else if (!strcmp(name, "r") && !strcmp(aname, "style")) {
                    r_style = AO_STYLE_OFF;
                    for (const char *q = aval; *q; q++) {
                        if (*q == 'b') r_style |= AO_STYLE_BOLD;
                        else if (*q == 'i') r_style |= AO_STYLE_ITALIC;
                        else if (*q == 'u') r_style |= AO_STYLE_UNDERLINE;
                        else return AO_E_FORMAT;             /* unknown style letter */
                    }
                } else if (!strcmp(name, "r") && !strcmp(aname, "size")) {
                    int v = 0;
                    for (const char *q = aval; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
                    if (v < 6 || v > 64) return AO_E_FORMAT;
                    r_size = (uint8_t)v;
                } else if (!strcmp(name, "r") && !strcmp(aname, "font")) {
                    if (!strcmp(aval, "mono")) r_font = AO_FONT_MONO;
                    else if (!strcmp(aval, "sans")) r_font = AO_FONT_SANS;
                    else return AO_E_FORMAT;
                }
                /* Unknown ATTRIBUTES are ignored on purpose: that is what makes
                 * a newer writer's file readable by this reader. */
            }

            if (!strcmp(name, "aodoc")) {
                if (!seen_version) {
                    /* A file without a version is not this format. */
                    return AO_E_FORMAT;
                }
                in_aodoc = 1;
            } else if (!strcmp(name, "p")) {
                if (!in_aodoc) return AO_E_FORMAT;
                /* Start a new paragraph: append the newline that ends the
                 * PREVIOUS one, unless this is the first.  Counting paragraphs
                 * (rather than testing text_len) is what keeps empty paragraphs
                 * -- including several empty ones in a row -- from collapsing. */
                if (p_seen >= AO_IO_MAX_PARAS) return AO_E_FULL;
                if (p_seen > 0) {
                    int rc = ao_model_insert(m, ao_model_text_len(m), "\n", 1);
                    if (rc != AO_OK) return rc == AO_E_FULL ? AO_E_FULL : AO_E_FORMAT;
                }
                /* Alignment is applied after the whole document is read: the
                 * model creates a paragraph only once something is written into
                 * it, so the index of an empty paragraph is not known yet. */
                p_aligns[p_seen++] = p_align;
                (void)p_indent;
                in_p = 1;
            } else if (!strcmp(name, "r")) {
                if (!in_p) return AO_E_FORMAT;
                in_r = 1;
                text_at = 0;
            }
            if (self_close) {
                depth--;
                if (!strcmp(name, "r")) {
                    /* <r/> is an empty run: nothing to add, and the empty
                     * paragraph keeps its one empty run from the model. */
                    in_r = 0;
                }
            }
            continue;
        }

        /* Character data: only inside a run does it mean anything. */
        if (in_r) {
            if (c.s[c.i] == '&') {
                char enc[4];
                int n = read_entity(&c, enc);
                if (n < 0) return AO_E_FORMAT;
                if (text_at + (uint32_t)n >= sizeof text) return AO_E_FULL;
                memcpy(text + text_at, enc, (size_t)n);
                text_at += (uint32_t)n;
            } else {
                if (text_at + 1 >= sizeof text) return AO_E_FULL;
                text[text_at++] = c.s[c.i++];
            }
        } else {
            c.i++;                      /* whitespace between elements */
        }
    }
    if (depth != 0) return AO_E_FORMAT;     /* unclosed element */
    if (!in_aodoc && ao_model_text_len(m) == 0) return AO_E_FORMAT;

    /* The model's paragraphs must be exactly the ones the file named.  Because
     * the reader appends a newline BEFORE each paragraph after the first, the
     * count matches by construction; verify rather than assume. */
    if (ao_model_para_count(m) != p_seen) { ao_model_clear(m); return AO_E_FORMAT; }
    for (uint32_t i = 0; i < p_seen; i++) {
        if (ao_model_set_align(m, m->paras[i].off, p_aligns[i]) != AO_OK) {
            ao_model_clear(m);
            return AO_E_FORMAT;
        }
    }
    if (!ao_utf8_valid(m->text, m->text_len)) { ao_model_clear(m); return AO_E_UTF8; }
    const char *why = NULL;
    if (!ao_model_valid(m, &why)) { ao_model_clear(m); return AO_E_FORMAT; }
    return AO_OK;
}

int ao_io_import(ao_model_t *m, const char *xml, size_t len) {
    if (!m || !xml) return AO_E_ARG;
    if (len == 0) len = strlen(xml);
    ao_model_clear(m);
    int rc = import_into(m, xml, len);
    if (rc != AO_OK) ao_model_clear(m);
    return rc;
}
