/* userspace/office/libaodoc/aodox.c — the OOXML bridge: .docx (OF8).
 *
 * See aodox.h for the subset and for the two rules (refusal, determinism)
 * that decide every edge case.  This file is only translation: aozip.c owns
 * the container, aoxml.c owns the markup, aomodel.c owns the document.
 *
 * One fact about the parser drives the shape of the importer: a self-closing
 * element such as <w:tab/> yields a START token with empty=1 and NO matching
 * END token.  Every element that opens state therefore checks `empty` before
 * setting its flag, and every element that carries an action performs it
 * whether or not it was written self-closing.
 */
#include "aodox.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "aoxml.h"

/* A sink failure during export.  The numeric family is aozip's, because that
 * is the failure a caller already handles when writing a package. */
#define AO_DOCX_E_IO AO_ZIP_E_IO

static const char DOCUMENT_PART[]  = "word/document.xml";
static const char RELS_PART[]      = "word/_rels/document.xml.rels";
static const char NUMBERING_PART[] = "word/numbering.xml";

const char *ao_docx_document_part(void) { return DOCUMENT_PART; }
const char *ao_docx_rels_part(void)     { return RELS_PART; }

/* ------------------------------------------------------------------ shared */

/* Namespace URIs are not validated, so a name is compared on its LOCAL part:
 * "<w:p>" and "<p>" are the same element here. */
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

static int copy_str(char *dst, size_t cap, const char *src, size_t len) {
    if (!dst || cap == 0) return AO_E_ARG;
    size_t n = len < cap - 1u ? len : cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n == len ? AO_OK : AO_E_FULL;
}

static int xml_error_to_doc(int rc) {
    if (rc == AO_XML_E_UTF8) return AO_E_UTF8;
    if (rc == AO_XML_E_FULL || rc == AO_XML_E_LIMIT) return AO_E_FULL;
    return AO_E_FORMAT;
}

/* Decode an attribute value into a bounded buffer.  Refuses on overflow: an
 * attribute we cannot hold is a receipt, never a silently truncated value. */
static int attr_value(const ao_xml_attr_t *a, char *out, size_t cap) {
    if (!a || !out || cap == 0) return AO_E_ARG;
    size_t n = 0;
    int rc = ao_xml_decode(a->value, a->value_len, out, cap - 1u, &n);
    if (rc != AO_XML_OK) return xml_error_to_doc(rc);
    out[n] = '\0';
    return AO_OK;
}

static int attr_u32(const ao_xml_token_t *t, const char *want, uint32_t max,
                    uint32_t *out) {
    const ao_xml_attr_t *a = find_attr(t, want);
    if (!a) return AO_E_FORMAT;
    char buf[32];
    int rc = attr_value(a, buf, sizeof(buf));
    if (rc != AO_OK) return rc;
    if (!buf[0]) return AO_E_FORMAT;
    uint32_t v = 0;
    for (const char *p = buf; *p; p++) {
        if (*p < '0' || *p > '9') return AO_E_FORMAT;
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (max - d) / 10u) return AO_E_FULL;
        v = v * 10u + d;
    }
    *out = v;
    return AO_OK;
}

static unsigned char fold(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static int ascii_ieq(const char *s, const char *lit) {
    for (size_t i = 0;; i++) {
        /* BOTH sides are folded: comparing against an unfolded literal is the
         * sort of bug that only shows up on the mixed-case OOXML style ids. */
        unsigned char a = fold((unsigned char)s[i]);
        unsigned char b = fold((unsigned char)lit[i]);
        if (a != b) return 0;
        if (!b) return 1;
    }
}

/* Append to a bounded buffer.  Returns the new length, or 0 on overflow (the
 * caller turns that into AO_E_FULL). */
static size_t cat_str(char *dst, size_t cap, size_t at, const char *fmt, ...) {
    if (at >= cap) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst + at, cap - at, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - at) return 0;
    return at + (size_t)n;
}

static void note_call(ao_docx_note_fn note, void *ctx, const char *text) {
    if (note) note(ctx, text, strlen(text));
}

/* ------------------------------------------------------------- export sink */

typedef struct {
    int (*write)(void *context, const void *bytes, size_t length);
    void  *context;
    size_t used;
    int    error;
} xsink_t;

static void xs_put(xsink_t *k, const char *s, size_t n) {
    if (k->error || n == 0) return;
    if (k->write(k->context, s, n) != 0) { k->error = AO_DOCX_E_IO; return; }
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

/* --------------------------------------------------------- export: content */

#define NS_W   "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
#define NS_R   "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define NS_WP  "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
#define NS_A   "http://schemas.openxmlformats.org/drawingml/2006/main"
#define NS_PIC "http://schemas.openxmlformats.org/drawingml/2006/picture"

/* Fixed extent for an exported image: 2 x 2 inches in EMU.  libaodoc is pure
 * and does not decode bitmaps, so the pixel size is not ours to know; the
 * bytes are intact and a viewer re-flows the paragraph around it. */
#define DOCX_IMG_EMU 1828800u

/* px <-> twips at 96 dpi: 1 px = 1/96 in = 15 twips. */
#define PX_TO_TWIPS(px) ((uint32_t)(px) * 15u)
#define TWIPS_TO_PX(t)  ((int16_t)((t) / 15u))

static const char CONTENT_TYPES_BODY[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
    " <Default Extension=\"rels\""
    " ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
    " <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
    " <Default Extension=\"png\" ContentType=\"image/png\"/>\n"
    " <Override PartName=\"/word/document.xml\" ContentType=\"application/vnd."
    "openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>\n"
    " <Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd."
    "openxmlformats-officedocument.wordprocessingml.styles+xml\"/>\n"
    " <Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd."
    "openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>\n"
    "</Types>\n";

static const char PACKAGE_RELS[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
    " <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
    "officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>\n"
    "</Relationships>\n";

static const char STYLES_XML[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:styles xmlns:w=\"" NS_W "\">\n"
    " <w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
    "<w:name w:val=\"Normal\"/></w:style>\n"
    " <w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
    "<w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>"
    "<w:rPr><w:b/><w:sz w:val=\"32\"/></w:rPr></w:style>\n"
    " <w:style w:type=\"paragraph\" w:styleId=\"Heading2\">"
    "<w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:outlineLvl w:val=\"1\"/></w:pPr>"
    "<w:rPr><w:b/><w:sz w:val=\"26\"/></w:rPr></w:style>\n"
    " <w:style w:type=\"paragraph\" w:styleId=\"Heading3\">"
    "<w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:outlineLvl w:val=\"2\"/></w:pPr>"
    "<w:rPr><w:b/><w:sz w:val=\"24\"/></w:rPr></w:style>\n"
    " <w:style w:type=\"paragraph\" w:styleId=\"Quote\">"
    "<w:name w:val=\"quote\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:ind w:left=\"720\" w:right=\"720\"/></w:pPr>"
    "<w:rPr><w:i/></w:rPr></w:style>\n"
    " <w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\">"
    "<w:name w:val=\"List Paragraph\"/><w:basedOn w:val=\"Normal\"/>"
    "<w:pPr><w:ind w:left=\"720\"/></w:pPr></w:style>\n"
    "</w:styles>\n";

static const char NUMBERING_XML[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<w:numbering xmlns:w=\"" NS_W "\">\n"
    " <w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\">"
    "<w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
    "<w:lvlText w:val=\"\xE2\x80\xA2\"/><w:lvlJc w:val=\"left\"/>"
    "<w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
    "</w:lvl></w:abstractNum>\n"
    " <w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\">"
    "<w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
    "<w:lvlText w:val=\"%1.\"/><w:lvlJc w:val=\"left\"/>"
    "<w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
    "</w:lvl></w:abstractNum>\n"
    " <w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>\n"
    " <w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>\n"
    "</w:numbering>\n";

static const char *style_id_for(uint8_t kind) {
    switch (kind) {
        case AO_PARA_H1:     return "Heading1";
        case AO_PARA_H2:     return "Heading2";
        case AO_PARA_H3:     return "Heading3";
        case AO_PARA_QUOTE:  return "Quote";
        case AO_PARA_BULLET: return "ListParagraph";
        case AO_PARA_NUMBER: return "ListParagraph";
        default:             return NULL;
    }
}

static const char *jc_for(uint8_t align) {
    switch (align) {
        case AO_ALIGN_CENTER:  return "center";
        case AO_ALIGN_RIGHT:   return "right";
        case AO_ALIGN_JUSTIFY: return "both";
        default:               return "left";
    }
}

/* Media discovered while writing word/document.xml.  rIds are assigned in
 * encounter order, so the measuring pass and the writing pass agree -- which
 * is what keeps the archive byte-identical across two saves. */
typedef struct {
    char     path[AO_DOCX_MAX_MEDIA][AO_DOCX_MAX_PATH];
    uint32_t count;
    uint32_t overflow;
} media_list_t;

#define MEDIA_RID_BASE 3u   /* rId1 = styles.xml, rId2 = numbering.xml */

static int media_rid(media_list_t *ml, const char *path, size_t len,
                     uint32_t *rid, uint32_t *skipped) {
    for (uint32_t i = 0; i < ml->count; i++)
        if (strlen(ml->path[i]) == len &&
            !memcmp(ml->path[i], path, len)) { *rid = MEDIA_RID_BASE + i; return AO_OK; }
    if (ml->count >= AO_DOCX_MAX_MEDIA) {
        if (skipped) (*skipped)++;
        ml->overflow++;
        return AO_E_FULL;
    }
    if (copy_str(ml->path[ml->count], AO_DOCX_MAX_PATH, path, len) != AO_OK)
        return AO_E_FULL;
    *rid = MEDIA_RID_BASE + ml->count;
    ml->count++;
    return AO_OK;
}

static void put_run_pr(xsink_t *k, uint8_t style, uint8_t size, uint8_t font) {
    int plain = style == AO_STYLE_OFF && size == AO_SIZE_DEFAULT &&
                font == AO_FONT_SANS;
    if (plain) return;
    xs_puts(k, "<w:rPr>");
    if (style & AO_STYLE_BOLD)      xs_puts(k, "<w:b/>");
    if (style & AO_STYLE_ITALIC)    xs_puts(k, "<w:i/>");
    if (style & AO_STYLE_UNDERLINE) xs_puts(k, "<w:u w:val=\"single\"/>");
    if (size != AO_SIZE_DEFAULT) {
        xs_puts(k, "<w:sz w:val=\"");
        xs_put_u32(k, (uint32_t)size * 2u);            /* half-points */
        xs_puts(k, "\"/><w:szCs w:val=\"");
        xs_put_u32(k, (uint32_t)size * 2u);
        xs_puts(k, "\"/>");
    }
    if (font != AO_FONT_SANS)
        xs_puts(k, "<w:rFonts w:ascii=\"Aura Mono\" w:hAnsi=\"Aura Mono\""
                   " w:cs=\"Aura Mono\"/>");
    xs_puts(k, "</w:rPr>");
}

static void put_text_element(xsink_t *k, const char *s, uint32_t n) {
    xs_puts(k, "<w:t xml:space=\"preserve\">");
    xs_escape(k, s, n);
    xs_puts(k, "</w:t>");
}

/* One paragraph that is not part of a table.  `len` is the paragraph's text
 * length with its trailing newline already removed. */
static void put_paragraph(xsink_t *k, const ao_model_t *m, uint32_t pi,
                          const char *text, uint32_t len,
                          media_list_t *ml, uint32_t *skipped) {
    const ao_para_t *p = &m->paras[pi];
    uint8_t kind = p->style & AO_PARA_KIND_MASK;

    if (kind == AO_PARA_IMAGE) {
        uint32_t rid = 0;
        if (media_rid(ml, text, len, &rid, skipped) != AO_OK) {
            xs_puts(k, "<w:p/>\n");
            return;
        }
        xs_puts(k, "<w:p><w:r><w:drawing>"
                   "<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                   "<wp:extent cx=\"");
        xs_put_u32(k, DOCX_IMG_EMU);
        xs_puts(k, "\" cy=\"");
        xs_put_u32(k, DOCX_IMG_EMU);
        xs_puts(k, "\"/><wp:docPr id=\"");
        xs_put_u32(k, rid);
        xs_puts(k, "\" name=\"Picture\"/>"
                   "<a:graphic><a:graphicData uri=\"" NS_PIC "\">"
                   "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"0\" name=\"");
        xs_escape(k, text, len);
        xs_puts(k, "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>"
                   "<a:blip r:embed=\"rId");
        xs_put_u32(k, rid);
        xs_puts(k, "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                   "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"");
        xs_put_u32(k, DOCX_IMG_EMU);
        xs_puts(k, "\" cy=\"");
        xs_put_u32(k, DOCX_IMG_EMU);
        xs_puts(k, "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/>"
                   "</a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
                   "</a:graphic></wp:inline></w:drawing></w:r></w:p>\n");
        return;
    }

    xs_puts(k, "<w:p>");
    const char *sid = style_id_for(kind);
    int listed = kind == AO_PARA_BULLET || kind == AO_PARA_NUMBER;
    if (sid || p->align != AO_ALIGN_LEFT || p->indent != 0 || listed) {
        xs_puts(k, "<w:pPr>");
        if (sid) {
            xs_puts(k, "<w:pStyle w:val=\"");
            xs_puts(k, sid);
            xs_puts(k, "\"/>");
        }
        if (listed) {
            xs_puts(k, "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"");
            xs_puts(k, kind == AO_PARA_NUMBER ? "2" : "1");
            xs_puts(k, "\"/></w:numPr>");
        }
        if (p->align != AO_ALIGN_LEFT) {
            xs_puts(k, "<w:jc w:val=\"");
            xs_puts(k, jc_for(p->align));
            xs_puts(k, "\"/>");
        }
        if (p->indent != 0) {
            xs_puts(k, "<w:ind w:left=\"");
            xs_put_u32(k, PX_TO_TWIPS(p->indent < 0 ? 0 : p->indent));
            xs_puts(k, "\"/>");
        }
        xs_puts(k, "</w:pPr>");
    }

    int wrote = 0;
    for (uint32_t r = 0; r < p->run_count; r++) {
        const ao_run_t *run = &m->runs[p->first_run + r];
        uint32_t n = run->len;
        if (run->off + n > p->off + len) n = (p->off + len) - run->off;
        /* A run that held only the paragraph's newline has no text; emitting
         * "<w:r/>" for it would make two documents that mean the same thing
         * serialise differently. */
        if (n == 0) continue;
        wrote = 1;
        xs_puts(k, "<w:r>");
        put_run_pr(k, run->style, run->size, run->font);
        put_text_element(k, m->text + run->off, n);
        xs_puts(k, "</w:r>");
    }
    if (!wrote) xs_puts(k, "<w:r><w:t xml:space=\"preserve\"></w:t></w:r>");
    xs_puts(k, "</w:p>\n");
}

/* Consecutive AO_PARA_TABLE paragraphs are one w:tbl: the model stores a row
 * per paragraph with cells separated by '\t' (aomodel.h), so a run of them is
 * exactly a table, and a cell's own paragraph markup is regenerated here. */
static void put_table(xsink_t *k, const ao_model_t *m, uint32_t from, uint32_t to) {
    xs_puts(k, "<w:tbl>\n");
    for (uint32_t pi = from; pi < to; pi++) {
        const ao_para_t *p = &m->paras[pi];
        uint32_t len = p->len;
        if (len && m->text[p->off + len - 1u] == '\n') len--;
        xs_puts(k, "<w:tr>");
        uint32_t cell = 0;
        for (uint32_t i = 0; i <= len; i++) {
            if (i < len && m->text[p->off + i] != '\t') continue;
            xs_puts(k, "<w:tc><w:p><w:r>");
            put_text_element(k, m->text + p->off + cell, i - cell);
            xs_puts(k, "</w:r></w:p></w:tc>");
            cell = i + 1u;
        }
        xs_puts(k, "</w:tr>\n");
    }
    xs_puts(k, "</w:tbl>\n");
}

static int write_document_xml(const ao_model_t *m, xsink_t *k,
                              const ao_docx_sect_t *sect,
                              media_list_t *ml, uint32_t *skipped) {
    xs_puts(k, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    xs_puts(k, "<w:document xmlns:w=\"" NS_W "\" xmlns:r=\"" NS_R "\""
               " xmlns:wp=\"" NS_WP "\" xmlns:a=\"" NS_A "\""
               " xmlns:pic=\"" NS_PIC "\">\n<w:body>\n");

    uint32_t pi = 0;
    while (pi < m->para_len && !k->error) {
        const ao_para_t *p = &m->paras[pi];
        if ((p->style & AO_PARA_KIND_MASK) == AO_PARA_TABLE) {
            uint32_t from = pi;
            while (pi < m->para_len &&
                   (m->paras[pi].style & AO_PARA_KIND_MASK) == AO_PARA_TABLE) pi++;
            put_table(k, m, from, pi);
            continue;
        }
        uint32_t len = p->len;
        if (len && m->text[p->off + len - 1u] == '\n') len--;
        put_paragraph(k, m, pi, m->text + p->off, len, ml, skipped);
        pi++;
    }

    uint32_t pw = AO_DOCX_A4_W, ph = AO_DOCX_A4_H;
    uint32_t mlf = AO_DOCX_MARGIN, mr = AO_DOCX_MARGIN;
    uint32_t mt = AO_DOCX_MARGIN, mb = AO_DOCX_MARGIN;
    if (sect && sect->seen) {
        pw = sect->page_w; ph = sect->page_h;
        mlf = sect->margin_l; mr = sect->margin_r;
        mt = sect->margin_t; mb = sect->margin_b;
    }
    xs_puts(k, "<w:sectPr><w:pgSz w:w=\"");
    xs_put_u32(k, pw);
    xs_puts(k, "\" w:h=\"");
    xs_put_u32(k, ph);
    xs_puts(k, "\"/><w:pgMar w:top=\"");
    xs_put_u32(k, mt);
    xs_puts(k, "\" w:right=\"");
    xs_put_u32(k, mr);
    xs_puts(k, "\" w:bottom=\"");
    xs_put_u32(k, mb);
    xs_puts(k, "\" w:left=\"");
    xs_put_u32(k, mlf);
    xs_puts(k, "\" w:header=\"0\" w:footer=\"0\" w:gutter=\"0\"/></w:sectPr>\n");
    xs_puts(k, "</w:body></w:document>\n");
    return k->error ? AO_DOCX_E_IO : AO_OK;
}

/* ---------------------------------------------------------- export: driver */

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

size_t ao_docx_export_bound(const ao_model_t *m) {
    if (!m) return 0;
    size_t n = 8192u;                        /* the fixed parts */
    n += (size_t)m->text_len * 6u;           /* worst-case XML escaping */
    n += (size_t)m->para_len * 384u;         /* pPr + wrappers */
    n += (size_t)m->run_len * 384u;          /* rPr + w:t wrappers */
    return n;
}

int ao_docx_export(const ao_model_t *m, ao_zip_write_fn write, void *context,
                   size_t *archive_size) {
    return ao_docx_export_ex(m, write, context, archive_size,
                             NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL);
}

int ao_docx_export_ex(const ao_model_t *m, ao_zip_write_fn write, void *context,
                      size_t *archive_size,
                      const ao_docx_sect_t *sect,
                      ao_docx_media_fn media, void *media_context,
                      uint8_t *media_buf, size_t media_cap,
                      ao_docx_note_fn note, void *note_context,
                      uint32_t *skipped_out) {
    if (!m || !write) return AO_E_ARG;
    if (skipped_out) *skipped_out = 0;

    /* Pass 1: measure the document part and discover the media it references.
     * Both are needed before a byte of the archive is written, because a local
     * header carries its size and CRC ahead of its payload. */
    media_list_t ml;
    memset(&ml, 0, sizeof ml);
    uint32_t skipped = 0;
    xml_measure_t measure = { 0, AO_ZIP_CRC32_INITIAL };
    size_t xml_len = 0;
    {
        xsink_t k = { measure_write, &measure, 0, 0 };
        int rc = write_document_xml(m, &k, sect, &ml, &skipped);
        if (rc != AO_OK) return rc;
        xml_len = k.used;
    }
    if (xml_len != measure.length) return AO_E_FORMAT;
    uint32_t xml_crc = ao_zip_crc32_finish(measure.crc_state);

    ao_zip_writer_t writer;
    int rc = ao_zip_writer_init(&writer, write, context);
    if (rc != AO_ZIP_OK) return rc;

    if (rc == AO_ZIP_OK)
        rc = ao_zip_writer_add(&writer, "[Content_Types].xml",
                               sizeof("[Content_Types].xml") - 1u,
                               CONTENT_TYPES_BODY,
                               sizeof(CONTENT_TYPES_BODY) - 1u);

    if (rc == AO_ZIP_OK)
        rc = ao_zip_writer_add(&writer, "_rels/.rels",
                               sizeof("_rels/.rels") - 1u,
                               PACKAGE_RELS, sizeof(PACKAGE_RELS) - 1u);

    /* word/_rels/document.xml.rels: styles, numbering, then the media in
     * encounter order.  Fixed ids, so the package is reproducible. */
    if (rc == AO_ZIP_OK) {
        char rels[640 + AO_DOCX_MAX_MEDIA * 192u];
        size_t n = 0;
        n = cat_str(rels, sizeof(rels), n,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
            "2006/relationships\">\n"
            " <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
            "officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>\n"
            " <Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/"
            "officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>\n");
        if (n == 0) return AO_E_FULL;
        for (uint32_t i = 0; i < ml.count; i++) {
            n = cat_str(rels, sizeof(rels), n,
                " <Relationship Id=\"rId%u\" Type=\"http://schemas.openxmlformats"
                ".org/officeDocument/2006/relationships/image\""
                " Target=\"media/image%u.png\"/>\n",
                MEDIA_RID_BASE + i, i + 1u);
            if (n == 0) return AO_E_FULL;
        }
        n = cat_str(rels, sizeof(rels), n, "</Relationships>\n");
        if (n == 0) return AO_E_FULL;
        rc = ao_zip_writer_add(&writer, RELS_PART, sizeof(RELS_PART) - 1u,
                               rels, n);
    }

    /* word/document.xml: streamed, so no full copy is staged in RAM. */
    size_t streamed = 0;
    if (rc == AO_ZIP_OK)
        rc = ao_zip_writer_begin_entry(&writer, DOCUMENT_PART,
                                       sizeof(DOCUMENT_PART) - 1u,
                                       xml_len, xml_crc);
    if (rc == AO_ZIP_OK) {
        zip_stream_t s = { &writer };
        xsink_t k = { zip_write_xml, &s, 0, 0 };
        media_list_t ml2;
        memset(&ml2, 0, sizeof ml2);
        uint32_t skipped2 = 0;
        int rc2 = write_document_xml(m, &k, sect, &ml2, &skipped2);
        if (rc2 != AO_OK) rc = rc2;
        else if (k.used != xml_len) rc = AO_ZIP_E_SIZE;
        else streamed = k.used;
        if (rc == AO_OK && writer.error) rc = writer.error;
    }
    if (rc == AO_ZIP_OK) rc = ao_zip_writer_end_entry(&writer);

    if (rc == AO_ZIP_OK)
        rc = ao_zip_writer_add(&writer, "word/styles.xml",
                               sizeof("word/styles.xml") - 1u,
                               STYLES_XML, sizeof(STYLES_XML) - 1u);
    if (rc == AO_ZIP_OK)
        rc = ao_zip_writer_add(&writer, NUMBERING_PART,
                               sizeof(NUMBERING_PART) - 1u,
                               NUMBERING_XML, sizeof(NUMBERING_XML) - 1u);

    /* word/media/imageN.png.  Without a callback and a buffer to stage the
     * bytes in, the image is skipped with a receipt and the text is untouched. */
    for (uint32_t i = 0; i < ml.count && rc == AO_ZIP_OK; i++) {
        if (!media || !media_buf || media_cap == 0) {
            skipped++;
            note_call(note, note_context, "skipped image (no media source)");
            continue;
        }
        size_t got = 0;
        int mrc = media(media_context, ml.path[i], media_buf, media_cap, &got);
        if (mrc != AO_OK || got == 0 || got > media_cap) {
            skipped++;
            note_call(note, note_context, "skipped image (unreadable)");
            continue;
        }
        char part[AO_DOCX_MAX_PATH];
        size_t pn = cat_str(part, sizeof(part), 0, "word/media/image%u.png", i + 1u);
        if (pn == 0) { rc = AO_E_FULL; break; }
        rc = ao_zip_writer_add(&writer, part, pn, media_buf, got);
    }

    size_t total = 0;
    if (rc == AO_ZIP_OK) rc = ao_zip_writer_finish(&writer, &total);
    if (rc != AO_ZIP_OK) return rc;
    if (skipped_out) *skipped_out += skipped;
    if (archive_size) *archive_size = total;
    (void)streamed;
    return AO_OK;
}

/* ---------------------------------------------------------- import: driver */

typedef struct {
    ao_model_t *m;
    /* the paragraph under construction */
    uint32_t para_index;
    int      para_open;
    int      started;      /* the model's first paragraph already exists */
    uint32_t para_text_at; /* text length when this paragraph opened */
    uint8_t  align, style;
    int16_t  indent;
    /* the run under construction */
    int      in_run;
    uint8_t  run_style, run_size, run_font;
    /* where we are in the tree */
    int      seen_document, in_body;
    int      in_ppr, in_rpr, in_numpr, in_sectpr;
    int      in_t, in_drawing, in_instr;
    int      in_tbl, in_tr, in_tc, in_cell_p;
    /* tables */
    char     cell[AO_DOCX_MAX_TEXT_BYTES];
    size_t   cell_len;
    char     row[AO_DOCX_MAX_ROW_BYTES];
    size_t   row_len;
    /* numbering: numId -> 1 when decimal, 0 when bullet */
    uint8_t  num_decimal[AO_DOCX_MAX_RELS];
    /* receipts */
    char     noted[AO_DOCX_MAX_NOTES][AO_DOCX_MAX_NOTE_BYTES];
    uint32_t noted_count;
    uint32_t skipped;
    ao_docx_note_fn note;
    void    *note_context;
    const ao_docx_rels_t *rels;
    ao_docx_sect_t *sect;
    char    *dec;
    size_t   dec_cap;
} docx_imp_t;

/* "skipped <name>", once per distinct name: a document with four thousand
 * w:sdt elements yields one receipt, not a flood that hides the real error. */
static void note_skip_named(docx_imp_t *st, const char *name, size_t name_len) {
    st->skipped++;
    for (uint32_t i = 0; i < st->noted_count; i++)
        if (!strncmp(st->noted[i], name, name_len) && !st->noted[i][name_len]) return;
    if (!st->note) return;
    if (st->noted_count < AO_DOCX_MAX_NOTES) {
        if (copy_str(st->noted[st->noted_count], AO_DOCX_MAX_NOTE_BYTES,
                     name, name_len) != AO_OK) return;
        st->noted_count++;
    }
    char msg[AO_DOCX_MAX_NOTE_BYTES];
    size_t n = cat_str(msg, sizeof(msg), 0, "skipped <%.*s>",
                       (int)name_len, name);
    if (n) st->note(st->note_context, msg, strlen(msg));
}

static int apply_para_props(docx_imp_t *st) {
    if (!st->para_open) return AO_OK;
    ao_model_t *m = st->m;
    if (st->para_index >= m->para_len) return AO_E_FORMAT;
    uint32_t pos = m->paras[st->para_index].off;
    if (ao_model_set_align(m, pos, st->align) != AO_OK) return AO_E_FORMAT;
    if (ao_model_set_para_style(m, pos, st->style) != AO_OK) return AO_E_FORMAT;
    if (ao_model_set_indent(m, pos, st->indent) != AO_OK) return AO_E_FORMAT;
    return AO_OK;
}

/* Close the paragraph under construction and open the next one.  The newline
 * IS the separator in this model, so "new paragraph" and "insert '\n'" are
 * one operation; the properties are re-applied afterwards, which is what lets
 * a w:br split a paragraph without losing its alignment or style. */
static int begin_para(docx_imp_t *st) {
    ao_model_t *m = st->m;
    if (st->para_open) {
        int rc = apply_para_props(st);
        if (rc != AO_OK) return rc;
    }
    /* A cleared model already holds one empty paragraph -- a caret has to
     * have somewhere to be -- so the FIRST paragraph adopts it instead of
     * being created by a newline.  Only later paragraphs are separated by an
     * inserted '\n', and the new paragraph is then the last one. */
    if (st->started) {
        if (m->para_len >= m->para_cap) return AO_E_FULL;
        int rc = ao_model_insert(m, ao_model_text_len(m), "\n", 1);
        if (rc != AO_OK) return rc == AO_E_FULL ? AO_E_FULL : AO_E_FORMAT;
    }
    if (m->para_len == 0) return AO_E_FORMAT;
    st->para_index = m->para_len - 1u;
    st->para_open = 1;
    st->started = 1;
    st->para_text_at = ao_model_text_len(m);
    return AO_OK;
}

static int end_para(docx_imp_t *st) {
    if (!st->para_open) return AO_OK;
    int rc = apply_para_props(st);
    st->para_open = 0;
    return rc;
}

static int append_para_text(docx_imp_t *st, const char *s, uint32_t n) {
    if (n == 0) return AO_OK;
    if (!st->para_open) {
        int rc = begin_para(st);
        if (rc != AO_OK) return rc;
    }
    ao_model_t *m = st->m;
    uint32_t at = ao_model_text_len(m);
    int rc = ao_model_insert(m, at, s, n);
    if (rc != AO_OK) return rc == AO_E_FULL ? AO_E_FULL : AO_E_FORMAT;
    static const uint8_t bits[3] = {
        AO_STYLE_BOLD, AO_STYLE_ITALIC, AO_STYLE_UNDERLINE
    };
    for (size_t i = 0; i < 3; i++)
        if (ao_model_set_style(m, at, n, bits[i],
                               (st->run_style & bits[i]) != 0) != AO_OK)
            return AO_E_FORMAT;
    if (ao_model_set_size_font(m, at, n, st->run_size, st->run_font) != AO_OK)
        return AO_E_FORMAT;
    return AO_OK;
}

/* Text goes to the cell buffer inside a table cell, to the model otherwise. */
static int append_text(docx_imp_t *st, const char *s, size_t n) {
    if (n == 0) return AO_OK;
    if (st->in_cell_p) {
        if (st->cell_len + n > AO_DOCX_MAX_TEXT_BYTES) return AO_E_FULL;
        memcpy(st->cell + st->cell_len, s, n);
        st->cell_len += n;
        return AO_OK;
    }
    if (n > UINT32_MAX) return AO_E_FULL;
    return append_para_text(st, s, (uint32_t)n);
}

static int append_run_text(docx_imp_t *st, const ao_xml_token_t *t) {
    size_t n = 0;
    int rc = ao_xml_decode(t->text, t->text_len, st->dec, st->dec_cap, &n);
    if (rc != AO_XML_OK) return xml_error_to_doc(rc);
    if (!ao_utf8_valid(st->dec, n)) return AO_E_UTF8;
    return append_text(st, st->dec, n);
}

static void resolve_blip(docx_imp_t *st, const ao_xml_token_t *t) {
    const ao_xml_attr_t *a = find_attr(t, "embed");
    if (!a || !st->rels) { note_skip_named(st, "blip", 4); return; }
    char rid[AO_DOCX_MAX_REL_ID];
    if (attr_value(a, rid, sizeof(rid)) != AO_OK) {
        note_skip_named(st, "blip", 4);
        return;
    }
    for (uint32_t i = 0; i < st->rels->count; i++) {
        if (strncmp(st->rels->id[i], rid, AO_DOCX_MAX_REL_ID - 1u)) continue;
        /* An image occupies its own paragraph: the model has no in-line
         * anchored object, and splitting the run here keeps the text before
         * and after it intact.  Word wraps a drawing in a <w:p> of its own, so
         * when that wrapper is still empty we adopt it -- otherwise every
         * imported picture would leave a blank paragraph in front of itself. */
        int reuse = st->para_open && ao_model_text_len(st->m) == st->para_text_at;
        if (!reuse) {
            (void)end_para(st);
            if (begin_para(st) != AO_OK) { note_skip_named(st, "blip", 4); return; }
        }
        st->align = AO_ALIGN_LEFT;
        st->style = AO_PARA_IMAGE;
        st->indent = 0;
        {
            st->run_style = AO_STYLE_OFF;
            st->run_size = AO_SIZE_DEFAULT;
            st->run_font = AO_FONT_SANS;
            (void)append_para_text(st, st->rels->target[i],
                                   (uint32_t)strlen(st->rels->target[i]));
        }
        (void)end_para(st);
        return;
    }
    note_skip_named(st, "blip", 4);
}

static int on_off_default_on(const ao_xml_token_t *t) {
    const ao_xml_attr_t *a = find_attr(t, "val");
    if (!a) return 1;
    char buf[32];
    if (attr_value(a, buf, sizeof(buf)) != AO_OK) return 1;
    return !(ascii_ieq(buf, "0") || ascii_ieq(buf, "false") ||
             ascii_ieq(buf, "off") || ascii_ieq(buf, "none"));
}

static void read_sect_size(docx_imp_t *st, const ao_xml_token_t *t) {
    if (!st->sect) return;
    uint32_t v = 0;
    if (attr_u32(t, "w", 0xFFFFFFu, &v) == AO_OK) { st->sect->page_w = v; st->sect->seen = 1u; }
    if (attr_u32(t, "h", 0xFFFFFFu, &v) == AO_OK) { st->sect->page_h = v; st->sect->seen = 1u; }
}

static void read_sect_margins(docx_imp_t *st, const ao_xml_token_t *t) {
    if (!st->sect) return;
    uint32_t v = 0;
    if (attr_u32(t, "left", 0xFFFFFFu, &v) == AO_OK)  { st->sect->margin_l = v; st->sect->seen = 1u; }
    if (attr_u32(t, "right", 0xFFFFFFu, &v) == AO_OK) { st->sect->margin_r = v; st->sect->seen = 1u; }
    if (attr_u32(t, "top", 0xFFFFFFu, &v) == AO_OK)   { st->sect->margin_t = v; st->sect->seen = 1u; }
    if (attr_u32(t, "bottom", 0xFFFFFFu, &v) == AO_OK){ st->sect->margin_b = v; st->sect->seen = 1u; }
}

static int handle_start(docx_imp_t *st, const ao_xml_token_t *t) {
    const int empty = t->empty != 0;

    if (!st->seen_document) {
        if (!token_is(t, "document")) return AO_E_FORMAT;
        st->seen_document = 1;
        return AO_OK;
    }
    if (token_is(t, "body")) { if (!empty) st->in_body = 1; return AO_OK; }
    if (!st->in_body) return AO_OK;

    /* ---- paragraph properties ---- */
    if (token_is(t, "pPr")) { if (!empty) st->in_ppr = 1; return AO_OK; }
    if (st->in_ppr) {
        if (token_is(t, "jc")) {
            const ao_xml_attr_t *a = find_attr(t, "val");
            char buf[32];
            if (a && attr_value(a, buf, sizeof(buf)) == AO_OK) {
                if (ascii_ieq(buf, "center")) st->align = AO_ALIGN_CENTER;
                else if (ascii_ieq(buf, "right") || ascii_ieq(buf, "end")) st->align = AO_ALIGN_RIGHT;
                else if (ascii_ieq(buf, "both")) st->align = AO_ALIGN_JUSTIFY;
                else st->align = AO_ALIGN_LEFT;
            }
            return AO_OK;
        }
        if (token_is(t, "pStyle")) {
            const ao_xml_attr_t *a = find_attr(t, "val");
            char buf[64];
            if (a && attr_value(a, buf, sizeof(buf)) == AO_OK) {
                if (ascii_ieq(buf, "Heading1")) st->style = AO_PARA_H1;
                else if (ascii_ieq(buf, "Heading2")) st->style = AO_PARA_H2;
                else if (ascii_ieq(buf, "Heading3")) st->style = AO_PARA_H3;
                else if (ascii_ieq(buf, "Quote") || ascii_ieq(buf, "IntenseQuote"))
                    st->style = AO_PARA_QUOTE;
                /* ListParagraph says nothing on its own: w:numPr decides
                 * bullet vs decimal, below. */
            }
            return AO_OK;
        }
        if (token_is(t, "ind")) {
            uint32_t v = 0;
            if (attr_u32(t, "left", 0xFFFFu, &v) == AO_OK) st->indent = TWIPS_TO_PX(v);
            return AO_OK;
        }
        if (token_is(t, "numPr")) {
            /* Without a numId we can only say "this is a list". */
            st->style = AO_PARA_BULLET;
            if (!empty) st->in_numpr = 1;
            return AO_OK;
        }
        if (st->in_numpr && token_is(t, "numId")) {
            uint32_t v = 0;
            if (attr_u32(t, "val", 0xFFFFu, &v) == AO_OK)
                st->style = st->num_decimal[v % AO_DOCX_MAX_RELS] ? AO_PARA_NUMBER
                                                                  : AO_PARA_BULLET;
            return AO_OK;
        }
        if (st->in_numpr && token_is(t, "ilvl")) return AO_OK;
        if (token_is(t, "sectPr")) { if (!empty) st->in_sectpr = 1; return AO_OK; }
        if (st->in_sectpr && token_is(t, "pgSz")) { read_sect_size(st, t); return AO_OK; }
        if (st->in_sectpr && token_is(t, "pgMar")) { read_sect_margins(st, t); return AO_OK; }
        /* Any other pPr child is formatting we do not model. */
        if (!st->in_drawing) note_skip_named(st, t->name, t->name_len);
        return AO_OK;
    }

    /* ---- runs ---- */
    if (token_is(t, "r")) {
        st->in_run = 1;
        st->run_style = AO_STYLE_OFF;
        st->run_size = AO_SIZE_DEFAULT;
        st->run_font = AO_FONT_SANS;
        return AO_OK;
    }
    if (token_is(t, "rPr")) { if (!empty) st->in_rpr = 1; return AO_OK; }
    if (st->in_rpr) {
        if (token_is(t, "b")) { if (on_off_default_on(t)) st->run_style |= AO_STYLE_BOLD; return AO_OK; }
        if (token_is(t, "i")) { if (on_off_default_on(t)) st->run_style |= AO_STYLE_ITALIC; return AO_OK; }
        if (token_is(t, "u")) { if (on_off_default_on(t)) st->run_style |= AO_STYLE_UNDERLINE; return AO_OK; }
        if (token_is(t, "sz")) {
            uint32_t v = 0;
            if (attr_u32(t, "val", 0xFFFFu, &v) == AO_OK) {
                uint32_t px = v / 2u;                 /* half-points */
                if (px < 6u) px = 6u;
                if (px > 64u) px = 64u;
                st->run_size = (uint8_t)px;
            }
            return AO_OK;
        }
        if (token_is(t, "rFonts")) {
            const ao_xml_attr_t *a = find_attr(t, "ascii");
            char buf[64];
            if (a && attr_value(a, buf, sizeof(buf)) == AO_OK) {
                /* Two faces is all the suite has; anything monospaced is the
                 * mono one, everything else is sans. */
                for (char *p = buf; *p; p++)
                    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
                if (strstr(buf, "mono") || strstr(buf, "courier") ||
                    strstr(buf, "consolas")) st->run_font = AO_FONT_MONO;
            }
            return AO_OK;
        }
        if (!st->in_drawing) note_skip_named(st, t->name, t->name_len);
        return AO_OK;
    }
    if (token_is(t, "t")) {
        if (!empty) st->in_t = 1;
        return AO_OK;
    }
    if (token_is(t, "tab")) return append_text(st, "\t", 1);
    if (token_is(t, "br") || token_is(t, "cr")) {
        /* A line break becomes a paragraph break: this model's only break is
         * the newline, and inventing an in-run marker would corrupt it. */
        if (st->in_cell_p) return append_text(st, " ", 1);
        int rc = end_para(st);
        if (rc == AO_OK) rc = begin_para(st);
        return rc;
    }
    if (token_is(t, "noBreakHyphen")) return append_text(st, "-", 1);
    if (token_is(t, "instrText")) { if (!empty) st->in_instr = 1; return AO_OK; }

    /* ---- tables ---- */
    if (token_is(t, "tbl")) { if (!empty) st->in_tbl = 1; return AO_OK; }
    if (token_is(t, "tr"))  { st->in_tr = 1; st->row_len = 0; return AO_OK; }
    if (token_is(t, "tc"))  { st->in_tc = 1; st->cell_len = 0; return AO_OK; }

    /* ---- images ---- */
    if (token_is(t, "drawing")) { if (!empty) st->in_drawing++; return AO_OK; }
    if (token_is(t, "blip")) { resolve_blip(st, t); return AO_OK; }

    /* ---- paragraphs ---- */
    if (token_is(t, "p")) {
        if (st->in_tc) {
            /* "<w:tc><w:p/></w:tc>" is an empty cell: the flag must not
             * outlive the paragraph, or the next document text would be
             * appended to a cell that is already closed. */
            st->in_cell_p = empty ? 0 : 1;
            st->cell_len = 0;
            return AO_OK;
        }
        st->align = AO_ALIGN_LEFT;
        st->style = AO_PARA_NORMAL;
        st->indent = 0;
        int rc = begin_para(st);
        if (rc == AO_OK && empty) rc = end_para(st);
        return rc;
    }

    /* ---- section properties at document level ---- */
    if (token_is(t, "sectPr")) { if (!empty) st->in_sectpr = 1; return AO_OK; }
    if (st->in_sectpr && token_is(t, "pgSz")) { read_sect_size(st, t); return AO_OK; }
    if (st->in_sectpr && token_is(t, "pgMar")) { read_sect_margins(st, t); return AO_OK; }

    if (!st->in_drawing) note_skip_named(st, t->name, t->name_len);
    return AO_OK;
}

static int handle_end(docx_imp_t *st, const ao_xml_token_t *t) {
    if (token_is(t, "t")) { st->in_t = 0; return AO_OK; }
    if (token_is(t, "instrText")) { st->in_instr = 0; return AO_OK; }
    if (token_is(t, "rPr")) { st->in_rpr = 0; return AO_OK; }
    if (token_is(t, "pPr")) { st->in_ppr = 0; return AO_OK; }
    if (token_is(t, "numPr")) { st->in_numpr = 0; return AO_OK; }
    if (token_is(t, "r")) { st->in_run = 0; return AO_OK; }
    if (token_is(t, "drawing")) { if (st->in_drawing) st->in_drawing--; return AO_OK; }
    if (token_is(t, "sectPr")) { st->in_sectpr = 0; return AO_OK; }
    if (token_is(t, "body")) { st->in_body = 0; return AO_OK; }

    if (token_is(t, "p")) {
        if (st->in_cell_p) { st->in_cell_p = 0; return AO_OK; }
        return end_para(st);
    }
    if (token_is(t, "tc")) {
        if (st->row_len + st->cell_len + 1u > AO_DOCX_MAX_ROW_BYTES) return AO_E_FULL;
        memcpy(st->row + st->row_len, st->cell, st->cell_len);
        st->row_len += st->cell_len;
        st->row[st->row_len++] = '\t';
        st->cell_len = 0;
        st->in_tc = 0;
        st->in_cell_p = 0;
        return AO_OK;
    }
    if (token_is(t, "tr")) {
        if (st->row_len && st->row[st->row_len - 1u] == '\t') st->row_len--;
        st->align = AO_ALIGN_LEFT;
        st->style = AO_PARA_TABLE;
        st->indent = 0;
        int rc = begin_para(st);
        if (rc == AO_OK) {
            st->run_style = AO_STYLE_OFF;
            st->run_size = AO_SIZE_DEFAULT;
            st->run_font = AO_FONT_SANS;
            rc = append_para_text(st, st->row, (uint32_t)st->row_len);
        }
        if (rc == AO_OK) rc = end_para(st);
        st->row_len = 0;
        st->in_tr = 0;
        return rc;
    }
    if (token_is(t, "tbl")) { st->in_tbl = 0; return AO_OK; }
    return AO_OK;
}

static int parse_document(docx_imp_t *st, const char *xml, size_t len) {
    ao_xml_reader_t r;
    int rc = ao_xml_reader_init(&r, xml, len);
    if (rc != AO_XML_OK) return xml_error_to_doc(rc);
    ao_xml_token_t t;
    for (;;) {
        rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type == AO_XML_T_TEXT || t.type == AO_XML_T_CDATA) {
            if (st->in_t && !st->in_instr) {
                rc = append_run_text(st, &t);
                if (rc != AO_OK) return rc;
            }
            /* Text anywhere else is indentation betweenelements, or a field
             * instruction; neither is document content. */
            continue;
        }
        if (t.type == AO_XML_T_END) {
            rc = handle_end(st, &t);
            if (rc != AO_OK) return rc;
            continue;
        }
        rc = handle_start(st, &t);
        if (rc != AO_OK) return rc;
    }
    return end_para(st);
}

/* word/_rels/document.xml.rels -> the caller's table. */
static int parse_rels(const char *xml, size_t len, ao_docx_rels_t *out) {
    ao_xml_reader_t r;
    int rc = ao_xml_reader_init(&r, xml, len);
    if (rc != AO_XML_OK) return xml_error_to_doc(rc);
    ao_xml_token_t t;
    out->count = 0;
    out->overflow = 0;
    for (;;) {
        rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type != AO_XML_T_START) continue;
        if (!token_is(&t, "Relationship")) continue;
        if (out->count >= AO_DOCX_MAX_RELS) { out->overflow++; continue; }
        const ao_xml_attr_t *id = find_attr(&t, "Id");
        const ao_xml_attr_t *tg = find_attr(&t, "Target");
        if (!id || !tg) continue;
        if (attr_value(id, out->id[out->count], AO_DOCX_MAX_REL_ID) != AO_OK) continue;
        if (attr_value(tg, out->target[out->count], AO_DOCX_MAX_PATH) != AO_OK) continue;
        out->count++;
    }
    return AO_OK;
}

/* Pass 1 of numbering: abstractNumId -> is-decimal. */
static int scan_abstract(const char *xml, size_t len, uint8_t *abs_dec) {
    ao_xml_reader_t r;
    int rc = ao_xml_reader_init(&r, xml, len);
    if (rc != AO_XML_OK) return xml_error_to_doc(rc);
    ao_xml_token_t t;
    int in_abs = 0;
    uint32_t cur = 0;
    for (;;) {
        rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type == AO_XML_T_END) {
            if (in_abs && token_is(&t, "abstractNum")) in_abs = 0;
            continue;
        }
        if (t.type != AO_XML_T_START) continue;
        if (token_is(&t, "abstractNum")) {
            uint32_t v = 0;
            if (attr_u32(&t, "abstractNumId", 0xFFFFu, &v) == AO_OK) { cur = v; in_abs = 1; }
            continue;
        }
        if (in_abs && token_is(&t, "numFmt")) {
            const ao_xml_attr_t *a = find_attr(&t, "val");
            char buf[32];
            if (a && attr_value(a, buf, sizeof(buf)) == AO_OK)
                abs_dec[cur % AO_DOCX_MAX_RELS] =
                    (uint8_t)(!(ascii_ieq(buf, "bullet") || ascii_ieq(buf, "none")));
            in_abs = 0;   /* only the first level's format decides */
            continue;
        }
    }
    return AO_OK;
}

/* Pass 2 of numbering: numId -> abstractNumId -> is-decimal. */
static int scan_num_map(const char *xml, size_t len, const uint8_t *abs_dec,
                        uint8_t *num_dec) {
    ao_xml_reader_t r;
    int rc = ao_xml_reader_init(&r, xml, len);
    if (rc != AO_XML_OK) return xml_error_to_doc(rc);
    ao_xml_token_t t;
    int in_num = 0;
    uint32_t cur = 0;
    for (;;) {
        rc = ao_xml_next(&r, &t);
        if (rc != AO_XML_OK) return xml_error_to_doc(rc);
        if (t.type == AO_XML_T_EOF) break;
        if (t.type == AO_XML_T_END) {
            if (in_num && token_is(&t, "num")) in_num = 0;
            continue;
        }
        if (t.type != AO_XML_T_START) continue;
        if (token_is(&t, "num")) {
            uint32_t v = 0;
            if (attr_u32(&t, "numId", 0xFFFFu, &v) == AO_OK) { cur = v; in_num = 1; }
            continue;
        }
        if (in_num && token_is(&t, "abstractNumId")) {
            uint32_t v = 0;
            if (attr_u32(&t, "val", 0xFFFFu, &v) == AO_OK)
                num_dec[cur % AO_DOCX_MAX_RELS] = abs_dec[v % AO_DOCX_MAX_RELS];
            in_num = 0;
            continue;
        }
    }
    return AO_OK;
}

static int extract_part(const ao_zip_reader_t *r, const ao_zip_entry_t *e,
                        const uint8_t **data, size_t *len,
                        char *scratch, size_t scratch_cap) {
    if (e->method == AO_ZIP_STORE) return ao_zip_entry_view(r, e, data, len);
    if (!scratch || scratch_cap < e->uncompressed_size) return AO_ZIP_E_FULL;
    size_t got = 0;
    int rc = ao_zip_extract(r, e, scratch, scratch_cap, &got);
    if (rc != AO_ZIP_OK) return rc;
    *data = (const uint8_t *)scratch;
    *len = got;
    return AO_ZIP_OK;
}

int ao_docx_import(ao_model_t *m, const void *bytes, size_t length,
                   char *scratch, size_t scratch_cap,
                   ao_docx_rels_t *rels,
                   ao_docx_sect_t *sect_out,
                   ao_docx_note_fn note, void *note_context,
                   uint32_t *skipped_out) {
    if (!m || !bytes) return AO_E_ARG;
    if (skipped_out) *skipped_out = 0;
    ao_model_clear(m);
    if (length < 4u || memcmp(bytes, "PK\x03\x04", 4u) != 0) return AO_E_FORMAT;

    ao_zip_reader_t reader;
    int rc = ao_zip_reader_init(&reader, bytes, length);
    if (rc != AO_ZIP_OK) return rc;

    /* Relationships are parsed whether or not the caller asked for the table:
     * an image is part of the document, and resolving it must not depend on a
     * caller opting in.  The out-parameter is a convenience, not a switch. */
    ao_docx_rels_t internal_rels;
    memset(&internal_rels, 0, sizeof internal_rels);
    {
        ao_zip_entry_t e;
        const uint8_t *d = NULL;
        size_t n = 0;
        if (ao_zip_reader_find(&reader, RELS_PART, sizeof(RELS_PART) - 1u, &e) == AO_ZIP_OK &&
            extract_part(&reader, &e, &d, &n, scratch, scratch_cap) == AO_ZIP_OK &&
            n <= AO_XML_MAX_INPUT_BYTES)
            (void)parse_rels((const char *)d, n, &internal_rels);
    }
    if (rels) *rels = internal_rels;

    /* Numbering is parsed even when the caller did not ask for it: without it
     * every list would import as a bullet, and a numbered list is a different
     * document from a bulleted one. */
    uint8_t abs_dec[AO_DOCX_MAX_RELS];
    uint8_t num_dec[AO_DOCX_MAX_RELS];
    memset(abs_dec, 0, sizeof abs_dec);
    memset(num_dec, 0, sizeof num_dec);
    {
        ao_zip_entry_t e;
        const uint8_t *d = NULL;
        size_t n = 0;
        if (ao_zip_reader_find(&reader, NUMBERING_PART, sizeof(NUMBERING_PART) - 1u, &e) == AO_ZIP_OK &&
            extract_part(&reader, &e, &d, &n, scratch, scratch_cap) == AO_ZIP_OK &&
            n <= AO_XML_MAX_INPUT_BYTES) {
            /* Two passes over a part already in memory: abstract definitions
             * first, then the numId -> abstractNumId indirection.  Doing it in
             * one pass would need a map we do not have room for. */
            (void)scan_abstract((const char *)d, n, abs_dec);
            (void)scan_num_map((const char *)d, n, abs_dec, num_dec);
        }
    }

    ao_zip_entry_t entry;
    if (ao_zip_reader_find(&reader, DOCUMENT_PART, sizeof(DOCUMENT_PART) - 1u,
                           &entry) != AO_ZIP_OK)
        return AO_E_FORMAT;                 /* not a .docx we can read */
    const uint8_t *doc = NULL;
    size_t doc_len = 0;
    rc = extract_part(&reader, &entry, &doc, &doc_len, scratch, scratch_cap);
    if (rc != AO_ZIP_OK) return rc;
    if (doc_len == 0 || doc_len > AO_XML_MAX_INPUT_BYTES) return AO_E_FULL;

    /* The decode buffer is separate from the cell and row buffers: a table
     * cell is assembled while text nodes are still being decoded. */
    char decbuf[AO_DOCX_MAX_TEXT_BYTES];

    docx_imp_t st;
    memset(&st, 0, sizeof st);
    st.dec = decbuf;
    st.m = m;
    st.rels = &internal_rels;
    st.sect = sect_out;
    st.note = note;
    st.note_context = note_context;
    st.dec_cap = sizeof decbuf;
    memcpy(st.num_decimal, num_dec, sizeof num_dec);
    st.run_size = AO_SIZE_DEFAULT;
    st.align = AO_ALIGN_LEFT;
    st.style = AO_PARA_NORMAL;
    if (sect_out) {
        sect_out->seen = 0;
        sect_out->page_w = AO_DOCX_A4_W;
        sect_out->page_h = AO_DOCX_A4_H;
        sect_out->margin_l = AO_DOCX_MARGIN;
        sect_out->margin_r = AO_DOCX_MARGIN;
        sect_out->margin_t = AO_DOCX_MARGIN;
        sect_out->margin_b = AO_DOCX_MARGIN;
    }

    rc = parse_document(&st, (const char *)doc, doc_len);
    if (rc != AO_OK) { ao_model_clear(m); return rc; }
    if (skipped_out) *skipped_out = st.skipped;
    return AO_OK;
}
