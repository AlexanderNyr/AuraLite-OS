/* userspace/office/libaodoc/aomodel.c — the document model (OFFICE_PLAN.md OF2).
 *
 * The whole file is built around one decision: an edit is applied to the TEXT
 * FIRST, and the paragraph/run structure is rebuilt from the surviving runs
 * afterwards.  The alternative — patching paragraph and run records in place —
 * is where this kind of code usually goes wrong: a deletion that spans a
 * paragraph boundary has to shrink one run, drop two, splice an array and merge
 * a paragraph, and every one of those steps is easy to get subtly wrong and
 * hard to see.
 *
 * What makes the rebuild cheap enough to do per keystroke:
 *
 *   - the run array is CLIPPED, never re-derived: each old run keeps its
 *     attributes and loses the bytes that were deleted (a run can straddle a
 *     deletion boundary, which yields at most two pieces, so the array grows by
 *     at most two entries and no scratch buffer is needed);
 *   - paragraph boundaries are, by definition, the '\n' bytes in the new text,
 *     so the paragraph list is a scan; its ATTRIBUTES come from the old
 *     paragraph that used to hold that position;
 *   - normalisation (drop empty runs, merge equal neighbours) is one pass.
 *
 * 64 KiB of text costs well under a keystroke's budget to scan, and the fuzz
 * gate re-checks every invariant after every operation.
 */

#include "aomodel.h"
#include "aodoc.h"      /* ao_utf8_decode/valid/prev: one UTF-8 rule set */

#include <string.h>

/* --------------------------------------------------------------- helpers */

int ao_model_init(ao_model_t *m, char *text, size_t text_cap,
                  ao_run_t *runs, size_t run_cap,
                  ao_para_t *paras, size_t para_cap) {
    if (!m || !text || !runs || !paras) return AO_E_ARG;
    if (text_cap == 0 || run_cap == 0 || para_cap == 0) return AO_E_ARG;
    if (text_cap > 0xFFFFFFFFu || run_cap > 0xFFFFFFFFu || para_cap > 0xFFFFFFFFu)
        return AO_E_ARG;
    memset(m, 0, sizeof(*m));
    m->text = text;
    m->text_cap = (uint32_t)text_cap;
    m->runs = runs;
    m->run_cap = (uint32_t)run_cap;
    m->paras = paras;
    m->para_cap = (uint32_t)para_cap;
    ao_model_clear(m);
    return AO_OK;
}

void ao_model_clear(ao_model_t *m) {
    if (!m) return;
    m->text_len = 0;
    m->run_len = 1;
    m->para_len = 1;
    m->runs[0].off = 0;
    m->runs[0].len = 0;
    m->runs[0].style = AO_STYLE_OFF;
    m->runs[0].size = AO_SIZE_DEFAULT;
    m->runs[0].font = AO_FONT_SANS;
    m->runs[0]._pad = 0;
    memset(&m->paras[0], 0, sizeof(m->paras[0]));
    m->paras[0].first_run = 0;
    m->paras[0].run_count = 1;
    m->rev++;
}

/* ---------------------------------------------------------- invariants */

int ao_model_valid(const ao_model_t *m, const char **why) {
    const char *junk = NULL;
    if (!why) why = &junk;
    *why = "ok";
    if (!m) { *why = "null model"; return 0; }
    if (!m->text || !m->runs || !m->paras) { *why = "null buffers"; return 0; }
    if (m->text_len > m->text_cap) { *why = "text_len > text_cap"; return 0; }
    if (m->run_len == 0) { *why = "no runs"; return 0; }
    if (m->run_len > m->run_cap) { *why = "run_len > run_cap"; return 0; }
    if (m->para_len == 0) { *why = "no paragraphs"; return 0; }
    if (m->text_len && !ao_utf8_valid(m->text, m->text_len)) {
        *why = "text is not valid UTF-8"; return 0;
    }
    if (m->para_len > m->para_cap) { *why = "para_len > para_cap"; return 0; }

    /* Paragraphs: ordered, contiguous, covering the text, and each one ends
     * with '\n' unless it is the last. */
    uint32_t expect = 0;
    uint32_t run_cursor = 0;
    for (uint32_t i = 0; i < m->para_len; i++) {
        const ao_para_t *p = &m->paras[i];
        if (p->off != expect) { *why = "paragraphs not contiguous"; return 0; }
        if (p->len > m->text_len - p->off) { *why = "paragraph past the text"; return 0; }
        uint32_t end = p->off + p->len;
        if (i + 1 < m->para_len) {
            if (end == 0 || m->text[end - 1] != '\n') {
                *why = "a paragraph that is not last does not end with newline";
                return 0;
            }
        } else if (end != m->text_len) {
            *why = "paragraphs do not cover the text";
            return 0;
        }
        /* Runs: contiguous within the paragraph, covering it exactly. */
        if (p->run_count == 0) { *why = "paragraph with no runs"; return 0; }
        if (p->first_run != run_cursor) { *why = "run spans not contiguous"; return 0; }
        if (p->run_count > m->run_len - p->first_run) { *why = "run span past the array"; return 0; }
        uint32_t r_expect = p->off;
        for (uint32_t r = 0; r < p->run_count; r++) {
            const ao_run_t *run = &m->runs[p->first_run + r];
            if (run->off != r_expect) { *why = "runs not contiguous in paragraph"; return 0; }
            if (run->len > m->text_len - run->off) { *why = "run past the text"; return 0; }
            if (run->size < 6 || run->size > 64) { *why = "run size out of range"; return 0; }
            if (run->font > AO_FONT_MONO) { *why = "run font out of range"; return 0; }
            /* Canonical form: an empty run exists only as the sole run of an
             * empty paragraph, and no two neighbours share attributes. */
            if (run->len == 0 && !(p->len == 0 && p->run_count == 1)) {
                *why = "empty run inside a non-empty paragraph"; return 0;
            }
            if (r > 0) {
                const ao_run_t *prev = &m->runs[p->first_run + r - 1];
                if (prev->style == run->style && prev->size == run->size &&
                    prev->font == run->font) {
                    *why = "two adjacent runs with identical attributes"; return 0;
                }
            }
            r_expect += run->len;
        }
        if (r_expect != end) { *why = "runs do not cover their paragraph"; return 0; }
        if (p->align > AO_ALIGN_JUSTIFY) { *why = "paragraph align out of range"; return 0; }
        run_cursor += p->run_count;
        expect = end;
    }
    if (run_cursor != m->run_len) { *why = "runs outside any paragraph"; return 0; }
    return 1;
}

/* ------------------------------------------------------------ inspection */

uint32_t ao_model_text_len(const ao_model_t *m) { return m ? m->text_len : 0; }
uint32_t ao_model_para_count(const ao_model_t *m) { return m ? m->para_len : 0; }
uint32_t ao_model_run_count(const ao_model_t *m) { return m ? m->run_len : 0; }

uint32_t ao_model_para_of(const ao_model_t *m, uint32_t pos) {
    if (!m || m->para_len == 0) return 0;
    if (pos > m->text_len) pos = m->text_len;
    for (uint32_t i = 0; i < m->para_len; i++) {
        uint32_t end = m->paras[i].off + m->paras[i].len;
        if (pos < end) return i;
        /* A position exactly at the end of a paragraph belongs to the next one
         * (which is what "the caret is at the start of the next line" means),
         * except for the last paragraph. */
    }
    return m->para_len - 1;
}

uint32_t ao_model_para_start(const ao_model_t *m, uint32_t pi) {
    if (!m || pi >= m->para_len) return 0;
    return m->paras[pi].off;
}

uint32_t ao_model_run_of(const ao_model_t *m, uint32_t pos) {
    if (!m) return 0;
    uint32_t pi = ao_model_para_of(m, pos);
    const ao_para_t *p = &m->paras[pi];
    uint32_t best = p->first_run;
    for (uint32_t r = 0; r < p->run_count; r++) {
        uint32_t idx = p->first_run + r;
        if (m->runs[idx].off <= pos) best = idx; else break;
    }
    return best;
}

uint8_t ao_model_style_at(const ao_model_t *m, uint32_t pos) {
    if (!m) return AO_STYLE_OFF;
    return m->runs[ao_model_run_of(m, pos)].style;
}
uint8_t ao_model_size_at(const ao_model_t *m, uint32_t pos) {
    if (!m) return AO_SIZE_DEFAULT;
    return m->runs[ao_model_run_of(m, pos)].size;
}
uint8_t ao_model_font_at(const ao_model_t *m, uint32_t pos) {
    if (!m) return AO_FONT_SANS;
    return m->runs[ao_model_run_of(m, pos)].font;
}

uint32_t ao_model_next_cp(const ao_model_t *m, uint32_t pos) {
    if (!m || pos >= m->text_len) return m ? m->text_len : 0;
    uint32_t cp = 0;
    int n = ao_utf8_decode(m->text + pos, m->text_len - pos, &cp);
    if (n <= 0) return pos + 1;        /* damaged byte: step over it */
    return pos + (uint32_t)n;
}

uint32_t ao_model_prev_cp(const ao_model_t *m, uint32_t pos) {
    if (!m || pos == 0) return 0;
    if (pos > m->text_len) pos = m->text_len;
    return (uint32_t)ao_utf8_prev(m->text, pos);
}

int ao_model_copy_text(const ao_model_t *m, uint32_t pos, uint32_t n,
                       char *out, uint32_t cap) {
    if (!m || !out) return AO_E_ARG;
    if (pos > m->text_len) return AO_E_RANGE;
    if (n > m->text_len - pos) n = m->text_len - pos;
    if (n + 1 > cap) return AO_E_FULL;
    memcpy(out, m->text + pos, n);
    out[n] = '\0';
    return (int)n;
}

int64_t ao_model_find(const ao_model_t *m, uint32_t from, const char *needle,
                      int fold_ascii) {
    if (!m || !needle) return -1;
    size_t nl = strlen(needle);
    if (nl == 0 || m->text_len < nl) return -1;
    if (from > m->text_len - nl) return -1;
    for (uint32_t i = from; i + nl <= m->text_len; i++) {
        size_t k = 0;
        for (; k < nl; k++) {
            char a = m->text[i + k], b = needle[k];
            if (fold_ascii) {
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            }
            if (a != b) break;
        }
        if (k == nl) return (int64_t)i;
    }
    return -1;
}

/* ------------------------------------------- OF2 closure: word navigation */

enum { AO_CLS_SPACE = 0, AO_CLS_WORD = 1, AO_CLS_PUNCT = 2 };

/* Class of the codepoint STARTING at byte `pos`.  A multi-byte codepoint is a
 * word character: letters of every script the OF1 faces carry are >= 0x80 in
 * UTF-8 lead position.  Decoding (not the lead byte alone) decides, so a
 * combining mark or a damaged byte cannot masquerade as a letter. */
static int ao_cp_class(const ao_model_t *m, uint32_t pos) {
    if (pos >= m->text_len) return AO_CLS_SPACE;
    unsigned char b = (unsigned char)m->text[pos];
    if (b == ' ' || b == '\t' || b == '\n' || b == '\r') return AO_CLS_SPACE;
    if (b >= 0x80) return AO_CLS_WORD;
    if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
        (b >= '0' && b <= '9') || b == '_') return AO_CLS_WORD;
    return AO_CLS_PUNCT;
}

uint32_t ao_model_word_left(const ao_model_t *m, uint32_t pos) {
    if (!m) return 0;
    if (pos > m->text_len) pos = m->text_len;
    if (pos == 0) return 0;
    /* Skip whitespace/punctuation-class run immediately left, then the word. */
    uint32_t p = ao_model_prev_cp(m, pos);
    int cls = ao_cp_class(m, p);
    if (cls != AO_CLS_WORD) {
        while (p > 0 && ao_cp_class(m, ao_model_prev_cp(m, p)) == cls)
            p = ao_model_prev_cp(m, p);
        if (p == 0) return 0;
        /* Now at the start of the separator run: if a word ends here, the
         * target is that word's start, not the separator's. */
        uint32_t q = ao_model_prev_cp(m, p);
        if (ao_cp_class(m, q) != AO_CLS_WORD) return p;
        p = q; cls = AO_CLS_WORD;
    }
    while (p > 0 && ao_cp_class(m, ao_model_prev_cp(m, p)) == AO_CLS_WORD)
        p = ao_model_prev_cp(m, p);
    return p;
}

uint32_t ao_model_word_right(const ao_model_t *m, uint32_t pos) {
    if (!m) return 0;
    if (pos >= m->text_len) return m ? m->text_len : 0;
    int cls = ao_cp_class(m, pos);
    uint32_t p = pos;
    if (cls != AO_CLS_WORD) {
        while (p < m->text_len && ao_cp_class(m, p) == cls)
            p = ao_model_next_cp(m, p);
        if (p >= m->text_len) return p;
    }
    while (p < m->text_len && ao_cp_class(m, p) == AO_CLS_WORD)
        p = ao_model_next_cp(m, p);
    return p;
}

uint32_t ao_model_count(const ao_model_t *m, const char *needle, int fold_ascii) {
    if (!m || !needle || !*needle) return 0;
    size_t nl = strlen(needle);
    uint32_t n = 0, from = 0;
    for (;;) {
        int64_t at = ao_model_find(m, from, needle, fold_ascii);
        if (at < 0) break;
        n++;
        from = (uint32_t)at + (uint32_t)nl;
    }
    return n;
}

int ao_model_replace_all(ao_model_t *m, const char *needle, const char *with) {
    if (!m || !needle || !with) return AO_E_ARG;
    size_t nl = strlen(needle), wl = strlen(with);
    if (nl == 0) return AO_E_ARG;
    if (!ao_utf8_valid(with, wl)) return AO_E_UTF8;
    uint32_t from = 0, done = 0;
    for (;;) {
        int64_t at = ao_model_find(m, from, needle, 1);
        if (at < 0) break;
        int rc = ao_model_delete(m, (uint32_t)at, (uint32_t)nl);
        if (rc != AO_OK) return rc;
        if (wl) {
            rc = ao_model_insert(m, (uint32_t)at, with, (uint32_t)wl);
            if (rc != AO_OK) return rc;
        }
        done++;
        from = (uint32_t)at + (uint32_t)wl;   /* past the replacement: no rescan */
    }
    return (int)done;
}

/* --------------------------------------------------------------- rebuild */

/* Map a byte offset through an edit.  `pos`/`dn` describe the deletion (dn may
 * be 0 for a pure insertion at pos) and `ins` the insertion length. */
static uint32_t map_off(uint32_t off, uint32_t pos, uint32_t dn, uint32_t ins) {
    if (off <= pos) return off;
    if (off >= pos + dn) return off - dn + ins;
    return pos + ins;                   /* inside the deleted range */
}

/* The run the inserted bytes belong to.
 *
 * A caret at a paragraph boundary belongs to the paragraph that STARTS there
 * (that is what ao_model_para_of() says, and it is what "type at the start of
 * the next line" means), so the run starting at `pos` is the owner.  The one
 * exception is the end of the document, where no run starts and the last run
 * must take the bytes instead. */
static uint32_t owner_run_for(const ao_model_t *m, uint32_t pos) {
    if (m->run_len == 0) return 0;
    if (pos >= m->text_len) {
        for (uint32_t r = m->run_len; r-- > 0; )
            if (m->runs[r].off + m->runs[r].len == m->text_len) return r;
        return m->run_len - 1;
    }
    return ao_model_run_of(m, pos);
}

/* Rebuild paragraphs from the '\n' bytes in the text, carrying attributes from
 * the paragraph that used to contain each new paragraph's first byte. */
static int rebuild_paragraphs(ao_model_t *m, const ao_para_t *old_paras, uint32_t old_n,
                              uint32_t pos, uint32_t dn, uint32_t ins, uint32_t *out_n) {
    uint32_t count = 1;
    for (uint32_t i = 0; i < m->text_len; i++)
        if (m->text[i] == '\n') count++;
    if (count > m->para_cap) return AO_E_FULL;

    uint32_t start = 0;
    uint32_t pi = 0;
    for (uint32_t i = 0; i <= m->text_len; i++) {
        if (i < m->text_len && m->text[i] != '\n') continue;
        if (pi >= count) return AO_E_FORMAT;
        ao_para_t *p = &m->paras[pi];
        uint32_t len = i - start;
        if (i < m->text_len) len++;               /* include the '\n' */

        /* Attributes: whatever paragraph used to sit at this text position. */
        uint32_t src = map_off(start, pos, dn, ins);
        const ao_para_t *from = old_n ? &old_paras[0] : NULL;
        for (uint32_t k = 0; k < old_n; k++) {
            uint32_t o_end = old_paras[k].off + old_paras[k].len;
            if (src >= old_paras[k].off && src < o_end) { from = &old_paras[k]; break; }
            if (src >= o_end) from = &old_paras[k];
        }
        uint8_t align = from ? from->align : AO_ALIGN_LEFT;
        uint8_t pstyle = from ? from->style : AO_PARA_NORMAL;
        int16_t indent = from ? from->indent : 0;
        int16_t sb = from ? from->space_before : 0;
        int16_t sa = from ? from->space_after : 0;

        memset(p, 0, sizeof(*p));
        p->off = start;
        p->len = len;
        p->align = align;
        p->style = pstyle;
        p->indent = indent;
        p->space_before = sb;
        p->space_after = sa;
        start = i + 1;
        pi++;
    }
    *out_n = pi;
    return AO_OK;
}

/* Split any run that straddles a paragraph boundary.  A run may not span
 * paragraphs, so a newline typed inside a bold run cuts it in two. */
static int split_runs_at_boundaries(ao_model_t *m, uint32_t *n_io, uint32_t cap) {
    uint32_t n = *n_io;
    for (uint32_t pi = 0; pi + 1 < m->para_len; pi++) {
        uint32_t b = m->paras[pi].off + m->paras[pi].len;   /* the next paragraph's first byte */
        uint32_t r = 0;
        for (; r < n; r++) {
            if (m->runs[r].off < b && m->runs[r].off + m->runs[r].len > b) break;
            if (m->runs[r].off >= b) { r = n; break; }
        }
        if (r >= n) continue;                               /* already a boundary */
        if (n + 1 > cap) return AO_E_FULL;
        memmove(&m->runs[r + 2], &m->runs[r + 1], (n - r - 1) * sizeof(m->runs[0]));
        ao_run_t right = m->runs[r];
        right.off = b;
        right.len = m->runs[r].off + m->runs[r].len - b;
        m->runs[r].len = b - m->runs[r].off;
        m->runs[r + 1] = right;
        n++;
    }
    *n_io = n;
    return AO_OK;
}

/* Merge adjacent runs with identical attributes and shift everything down into
 * one tight array.
 *
 * The shift is the part that bit: compacting INSIDE each paragraph leaves stale
 * copies of the last runs behind, so the next paragraph's first_run points at a
 * duplicate of the previous paragraph's tail instead of at its own run.  The
 * document then fails the contiguity invariant, which is how it was caught.  So
 * the whole array is compacted in one pass, and a merge never crosses a
 * paragraph boundary. */
static void compact_runs(ao_model_t *m) {
    uint32_t w = 0, src = 0;
    for (uint32_t pi = 0; pi < m->para_len; pi++) {
        uint32_t cnt = m->paras[pi].run_count;
        m->paras[pi].first_run = w;
        uint32_t kept = 0;
        for (uint32_t r = 0; r < cnt; r++) {
            ao_run_t cur = m->runs[src + r];
            if (kept > 0) {
                ao_run_t *prev = &m->runs[w - 1];
                if (prev->style == cur.style && prev->size == cur.size &&
                    prev->font == cur.font) {
                    prev->len += cur.len;
                    continue;
                }
            }
            m->runs[w++] = cur;
            kept++;
        }
        m->paras[pi].run_count = kept;
        src += cnt;
    }
    m->run_len = w;
}

/* Assign runs to paragraphs, drop the empty ones, merge equal neighbours. */
static void normalize_runs(ao_model_t *m, ao_run_t *runs_in, uint32_t n_in) {
    uint32_t ri = 0;
    for (uint32_t pi = 0; pi < m->para_len; pi++) {
        ao_para_t *p = &m->paras[pi];
        uint32_t first = ri;
        uint32_t end = p->off + p->len;
        while (ri < n_in && runs_in[ri].off < end) {
            if (runs_in[ri].len == 0) { ri++; continue; }
            runs_in[ri].off = (ri == first) ? p->off : runs_in[ri].off;
            ri++;
        }
        p->first_run = first;
        p->run_count = ri - first;
        if (p->run_count == 0) {
            runs_in[ri].off = p->off;
            runs_in[ri].len = 0;
            runs_in[ri].style = AO_STYLE_OFF;
            runs_in[ri].size = AO_SIZE_DEFAULT;
            runs_in[ri].font = AO_FONT_SANS;
            runs_in[ri]._pad = 0;
            ri++;
            p->run_count = 1;
            p->first_run = ri - 1;
        }
    }
    m->run_len = ri;

    compact_runs(m);
}

/* The single entry point every text edit goes through. */
static int apply_edit(ao_model_t *m, uint32_t pos, uint32_t dn,
                      const char *ins, uint32_t ins_n) {
    if (!m) return AO_E_ARG;
    if (pos > m->text_len) return AO_E_RANGE;
    if (dn > m->text_len - pos) dn = m->text_len - pos;
    if (ins_n > m->text_cap - (m->text_len - dn)) return AO_E_FULL;

    /* Capacity: a deletion can cut a run in two (+2), and every paragraph
     * boundary the edit creates can split one more (+paragraphs).  Refusing up
     * front is what keeps the mutation below unable to fail half-way. */
    uint32_t worst = m->run_len + 2 + m->para_len + 1;
    if (worst > m->run_cap) return AO_E_FULL;
    /* Paragraph count can only grow by the number of newlines inserted. */
    uint32_t newlines = 0;
    for (uint32_t i = 0; i < ins_n; i++) if (ins[i] == '\n') newlines++;
    if (newlines > m->para_cap - m->para_len) return AO_E_FULL;

    uint32_t owner = (dn == 0 && ins_n) ? owner_run_for(m, pos) : 0;

    /* 1. the text. */
    uint32_t tail = m->text_len - pos - dn;
    if (tail) memmove(m->text + pos + ins_n, m->text + pos + dn, tail);
    if (ins_n) memcpy(m->text + pos, ins, ins_n);
    m->text_len = m->text_len - dn + ins_n;

    /* 2. clip the runs through the edit. */
    uint32_t out_len = 0;
    for (uint32_t r = 0; r < m->run_len; r++) {
        ao_run_t old = m->runs[r];
        uint32_t off = old.off, end = old.off + old.len;
        uint32_t n_off, n_len;
        if (dn == 0 && ins_n) {
            /* Pure insertion. */
            if (off == pos || (off < pos && end > pos)) {
                n_off = off;
                n_len = old.len + ins_n;             /* owner and straddle grow */
            } else if (off < pos && end == pos && r == owner) {
                n_off = off;
                n_len = old.len + ins_n;             /* caret at the very end */
            } else if (off >= pos) {
                n_off = off + ins_n;
                n_len = old.len;
            } else {
                n_off = off;
                n_len = old.len;
            }
        } else {
            /* Deletion (possibly with an insertion: only used by undo, which
             * calls insert and delete separately, so dn == 0 here in practice;
             * the general case is still handled). */
            uint32_t n_end = (end <= pos) ? end
                            : (end >= pos + dn) ? end - dn + ins_n : pos + ins_n;
            n_off = map_off(off, pos, dn, ins_n);
            if (n_end < n_off) n_end = n_off;
            n_len = n_end - n_off;
            if (n_len == 0 && old.len != 0) continue;   /* entirely deleted */
        }
        m->runs[out_len] = old;
        m->runs[out_len].off = n_off;
        m->runs[out_len].len = n_len;
        out_len++;
    }
    if (out_len == 0) {
        m->runs[0].off = 0;
        m->runs[0].len = 0;
        m->runs[0].style = AO_STYLE_OFF;
        m->runs[0].size = AO_SIZE_DEFAULT;
        m->runs[0].font = AO_FONT_SANS;
        m->runs[0]._pad = 0;
        out_len = 1;
    }

    /* 3. paragraphs, then runs. */
    static ao_para_t old_paras[AO_MODEL_PARAS_DEFAULT];
    uint32_t old_n = m->para_len;
    if (old_n > AO_MODEL_PARAS_DEFAULT) old_n = AO_MODEL_PARAS_DEFAULT;
    memcpy(old_paras, m->paras, old_n * sizeof(old_paras[0]));

    uint32_t new_n = 0;
    int rc = rebuild_paragraphs(m, old_paras, old_n, pos, dn, ins_n, &new_n);
    if (rc != AO_OK) {
        return rc;                       /* nothing mutated beyond the text; see below */
    }
    m->para_len = new_n;

    rc = split_runs_at_boundaries(m, &out_len, m->run_cap);
    if (rc != AO_OK) return rc;
    normalize_runs(m, m->runs, out_len);
    m->rev++;

    const char *why;
    if (!ao_model_valid(m, &why)) return AO_E_FORMAT;
    return AO_OK;
}

/* ----------------------------------------------------------------- edits */

int ao_model_insert(ao_model_t *m, uint32_t pos, const char *bytes, uint32_t n) {
    if (!m || (!bytes && n)) return AO_E_ARG;
    if (n == 0) return AO_OK;
    if (!ao_utf8_valid(bytes, n)) return AO_E_UTF8;
    /* A position inside a codepoint snaps FORWARD to the next boundary: an
     * insertion may not split a Cyrillic letter in half, because the result
     * would not be a valid document and every later save would carry the
     * damage.  The fuzz found this one too -- "text is not valid UTF-8" after
     * an innocuous one-character insert. */
    while (pos < m->text_len && ((unsigned char)m->text[pos] & 0xC0) == 0x80) pos++;
    return apply_edit(m, pos, 0, bytes, n);
}

int ao_model_delete(ao_model_t *m, uint32_t pos, uint32_t n) {
    if (!m) return AO_E_ARG;
    if (pos >= m->text_len) return AO_OK;
    /* Never leave half a codepoint behind, at either end.  The model stores
     * UTF-8, so a caller holding a byte position from somewhere else can easily
     * be one byte inside a Cyrillic letter: the START snaps back to the
     * codepoint boundary and the END clamps back to one.  Silently deleting a
     * partial character would make the document invalid -- the fuzz found
     * exactly this, as "delete rc" with no crash and no message. */
    while (pos > 0 && ((unsigned char)m->text[pos] & 0xC0) == 0x80) pos--;
    uint32_t end = pos + n;
    if (end > m->text_len) end = m->text_len;
    while (end < m->text_len && ((unsigned char)m->text[end] & 0xC0) == 0x80) end++;
    n = end - pos;
    if (n == 0) return AO_OK;
    return apply_edit(m, pos, n, NULL, 0);
}

/* Split the run containing `pos` so that `pos` is a run boundary. */
static int split_run_at(ao_model_t *m, uint32_t pos) {
    uint32_t ri = ao_model_run_of(m, pos);
    ao_run_t *r = &m->runs[ri];
    if (r->off >= pos || r->off + r->len <= pos) return AO_OK;   /* already a boundary */
    if (m->run_len + 1 > m->run_cap) return AO_E_FULL;

    /* Make room after this run. */
    memmove(&m->runs[ri + 2], &m->runs[ri + 1],
            (m->run_len - ri - 1) * sizeof(m->runs[0]));
    ao_run_t right = *r;
    right.off = pos;
    right.len = r->off + r->len - pos;
    r->len = pos - r->off;
    m->runs[ri + 1] = right;
    m->run_len++;

    /* Every paragraph whose runs start after `ri` shifts by one. */
    uint32_t pi = ao_model_para_of(m, pos);
    for (uint32_t k = 0; k < m->para_len; k++) {
        if (m->paras[k].first_run > ri) m->paras[k].first_run++;
    }
    m->paras[pi].run_count++;
    return AO_OK;
}

/* Style ranges are BYTE ranges, but a run boundary in the middle of a UTF-8
 * codepoint would make the document unexportable (and would let a caret land in
 * the middle of a character).  Widen/contract the request out to codepoint
 * edges instead of refusing it: callers that pass a column index in bytes get
 * the character that contains it. */
static void snap_range(const ao_model_t *m, uint32_t *pos, uint32_t *n) {
    uint32_t p = *pos, e = p + *n;
    if (e > m->text_len) e = m->text_len;
    while (p > 0 && ((unsigned char)m->text[p] & 0xC0) == 0x80) p--;
    while (e < m->text_len && ((unsigned char)m->text[e] & 0xC0) == 0x80) e++;
    *pos = p;
    *n = e - p;
}

int ao_model_set_style(ao_model_t *m, uint32_t pos, uint32_t n, uint8_t bits, int on) {
    if (!m) return AO_E_ARG;
    if (pos > m->text_len) return AO_E_RANGE;
    if (n > m->text_len - pos) n = m->text_len - pos;
    if (n == 0) return AO_OK;
    bits &= (AO_STYLE_BOLD | AO_STYLE_ITALIC | AO_STYLE_UNDERLINE);
    if (!bits) return AO_OK;
    snap_range(m, &pos, &n);
    if (n == 0) return AO_OK;
    int rc = split_run_at(m, pos);
    if (rc != AO_OK) return rc;
    rc = split_run_at(m, pos + n);
    if (rc != AO_OK) return rc;

    for (uint32_t r = 0; r < m->run_len; r++) {
        ao_run_t *run = &m->runs[r];
        if (run->off < pos || run->off >= pos + n) continue;
        if (on) run->style |= bits; else run->style = (uint8_t)(run->style & ~bits);
    }
    /* Re-normalise: setting bold on a run whose neighbour is already bold must
     * produce one run, or two identical documents would serialise differently. */
    compact_runs(m);
    m->rev++;

    const char *why;
    if (!ao_model_valid(m, &why)) return AO_E_FORMAT;
    return AO_OK;
}

int ao_model_set_size_font(ao_model_t *m, uint32_t pos, uint32_t n,
                           uint8_t size_px, uint8_t font) {
    if (!m) return AO_E_ARG;
    if (pos > m->text_len) return AO_E_RANGE;
    if (size_px < 6 || size_px > 64) return AO_E_ARG;
    if (font > AO_FONT_MONO) return AO_E_ARG;
    if (n > m->text_len - pos) n = m->text_len - pos;
    if (n == 0) return AO_OK;
    snap_range(m, &pos, &n);
    if (n == 0) return AO_OK;
    int rc = split_run_at(m, pos);
    if (rc != AO_OK) return rc;
    rc = split_run_at(m, pos + n);
    if (rc != AO_OK) return rc;
    for (uint32_t r = 0; r < m->run_len; r++) {
        ao_run_t *run = &m->runs[r];
        if (run->off < pos || run->off >= pos + n) continue;
        run->size = size_px;
        run->font = font;
    }
    compact_runs(m);
    m->rev++;
    return ao_model_valid(m, NULL) ? AO_OK : AO_E_FORMAT;
}

int ao_model_set_align(ao_model_t *m, uint32_t pos, uint8_t align) {
    if (!m) return AO_E_ARG;
    if (align > AO_ALIGN_JUSTIFY) return AO_E_ARG;
    if (pos > m->text_len) return AO_E_RANGE;
    m->paras[ao_model_para_of(m, pos)].align = align;
    m->rev++;
    return AO_OK;
}

int ao_model_set_para_style(ao_model_t *m, uint32_t pos, uint8_t style) {
    if (!m) return AO_E_ARG;
    if ((style & AO_PARA_KIND_MASK) > AO_PARA_IMAGE) return AO_E_ARG;
    if (pos > m->text_len) return AO_E_RANGE;
    m->paras[ao_model_para_of(m, pos)].style = style;
    m->rev++;
    return AO_OK;
}

uint8_t ao_model_para_style_at(const ao_model_t *m, uint32_t pos) {
    if (!m || pos > m->text_len) return AO_PARA_NORMAL;
    return m->paras[ao_model_para_of(m, pos)].style;
}

int ao_model_set_indent(ao_model_t *m, uint32_t pos, int16_t indent_px) {
    if (!m) return AO_E_ARG;
    if (pos > m->text_len) return AO_E_RANGE;
    m->paras[ao_model_para_of(m, pos)].indent = indent_px;
    m->rev++;
    return AO_OK;
}

/* --------------------------------------------------------- serialisation */

#define AO_MAGIC0 'A'
#define AO_MAGIC1 'O'
#define AO_MAGIC2 'M'
#define AO_MAGIC3 '1'

static void put_u32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)(v & 0xFF);
    (*p)[1] = (uint8_t)((v >> 8) & 0xFF);
    (*p)[2] = (uint8_t)((v >> 16) & 0xFF);
    (*p)[3] = (uint8_t)((v >> 24) & 0xFF);
    *p += 4;
}

static uint32_t get_u32(const uint8_t **p) {
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}

size_t ao_model_serialized_size(const ao_model_t *m) {
    if (!m) return 0;
    /* header + text + runs (12 B) + paragraphs (20 B) */
    return 16u + m->text_len + (size_t)m->run_len * 12u + (size_t)m->para_len * 20u;
}

int ao_model_serialize(const ao_model_t *m, void *out, size_t cap, size_t *len_out) {
    if (!m || !out) return AO_E_ARG;
    if (m->run_len > 0xFFFFu) return AO_E_FULL;
    size_t need = ao_model_serialized_size(m);
    if (cap < need) return AO_E_FULL;
    uint8_t *p = (uint8_t *)out;
    *p++ = AO_MAGIC0; *p++ = AO_MAGIC1; *p++ = AO_MAGIC2; *p++ = AO_MAGIC3;
    put_u32(&p, m->text_len);
    put_u32(&p, m->run_len);
    put_u32(&p, m->para_len);
    memcpy(p, m->text, m->text_len);
    p += m->text_len;
    for (uint32_t i = 0; i < m->run_len; i++) {
        put_u32(&p, m->runs[i].off);
        put_u32(&p, m->runs[i].len);
        *p++ = m->runs[i].style;
        *p++ = m->runs[i].size;
        *p++ = m->runs[i].font;
        *p++ = 0;
    }
    /* 20 bytes: off, len, first_run(u16), run_count(u16), align, pad,
     * indent, space_before, space_after (each i16).  The run spans are u16
     * because AO_MODEL_RUNS_DEFAULT is 4096: a document with more runs than
     * 65535 cannot be serialised, and serialize() refuses rather than
     * truncating. */
    for (uint32_t i = 0; i < m->para_len; i++) {
        put_u32(&p, m->paras[i].off);
        put_u32(&p, m->paras[i].len);
        p[0] = (uint8_t)(m->paras[i].first_run & 0xFF);
        p[1] = (uint8_t)((m->paras[i].first_run >> 8) & 0xFF);
        p[2] = (uint8_t)(m->paras[i].run_count & 0xFF);
        p[3] = (uint8_t)((m->paras[i].run_count >> 8) & 0xFF);
        p += 4;
        *p++ = m->paras[i].align;
        *p++ = m->paras[i].style;   /* OF3: was a zero pad byte before OF3 */
        p[0] = (uint8_t)((uint16_t)m->paras[i].indent & 0xFF);
        p[1] = (uint8_t)(((uint16_t)m->paras[i].indent >> 8) & 0xFF);
        p += 2;
        p[0] = (uint8_t)((uint16_t)m->paras[i].space_before & 0xFF);
        p[1] = (uint8_t)(((uint16_t)m->paras[i].space_before >> 8) & 0xFF);
        p += 2;
        p[0] = (uint8_t)((uint16_t)m->paras[i].space_after & 0xFF);
        p[1] = (uint8_t)(((uint16_t)m->paras[i].space_after >> 8) & 0xFF);
        p += 2;
    }
    if (len_out) *len_out = need;
    return AO_OK;
}

int ao_model_deserialize(ao_model_t *m, const void *in, size_t len) {
    if (!m || !in) return AO_E_ARG;
    const uint8_t *p = (const uint8_t *)in;
    const uint8_t *end = p + len;
    if (len < 16) return AO_E_FORMAT;
    if (p[0] != AO_MAGIC0 || p[1] != AO_MAGIC1 || p[2] != AO_MAGIC2 || p[3] != AO_MAGIC3)
        return AO_E_FORMAT;
    p += 4;
    uint32_t text_len = get_u32(&p);
    uint32_t run_len = get_u32(&p);
    uint32_t para_len = get_u32(&p);
    if (text_len > m->text_cap || run_len > m->run_cap || para_len > m->para_cap)
        return AO_E_FULL;
    if (run_len == 0 || para_len == 0) return AO_E_FORMAT;
    size_t need = (size_t)text_len + (size_t)run_len * 12u + (size_t)para_len * 20u;
    if ((size_t)(end - p) < need) return AO_E_FORMAT;
    if (text_len && !ao_utf8_valid((const char *)p, text_len)) return AO_E_UTF8;
    memcpy(m->text, p, text_len);
    p += text_len;
    for (uint32_t i = 0; i < run_len; i++) {
        m->runs[i].off = get_u32(&p);
        m->runs[i].len = get_u32(&p);
        m->runs[i].style = *p++;
        m->runs[i].size = *p++;
        m->runs[i].font = *p++;
        p++;
    }
    for (uint32_t i = 0; i < para_len; i++) {
        m->paras[i].off = get_u32(&p);
        m->paras[i].len = get_u32(&p);
        m->paras[i].first_run = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        m->paras[i].run_count = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
        p += 4;
        m->paras[i].align = *p++;
        m->paras[i].style = *p++;   /* pre-OF3 files wrote 0 = NORMAL here */

        m->paras[i].indent = (int16_t)(uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        p += 2;
        m->paras[i].space_before = (int16_t)(uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        p += 2;
        m->paras[i].space_after = (int16_t)(uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        p += 2;
    }
    m->text_len = text_len;
    m->run_len = run_len;
    m->para_len = para_len;
    m->rev++;
    const char *why;
    if (!ao_model_valid(m, &why)) return AO_E_FORMAT;
    return AO_OK;
}
