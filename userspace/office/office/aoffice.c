/* userspace/office/office/aoffice.c — the office suite's start centre, and
 * OF0's definition of done.
 *
 * OFFICE_PLAN.md OF0 ends with: "A window that draws a ruler, a margin box and
 * a caret, types text into itself with **no full-window repaint**, and reports
 * its repaint costs."
 *
 * That is what this program is, today.  It is deliberately not a word
 * processor: the document model arrives in OF2, the layout engine in OF3.  It
 * is the FLOOR those phases stand on, and the thing that proves the floor is
 * level:
 *
 *   - the page furniture (ruler, margin box, text area, caret) is drawn by the
 *     suite, in userspace, with its own glyph blits (OFFICE_PLAN D2);
 *   - every edit is painted through libaodoc's dirty-rect canvas, so a
 *     keystroke repaints two caret cells and the line that changed — never the
 *     window (D5);
 *   - `--bench` measures the three repaint classes and prints them, and the
 *     integration gate asserts the RATIOS, not the milliseconds, because two
 *     boots of this same code differed by 4x on the same benchmark
 *     (docs/office.md §4);
 *   - `--selftest` grades the document core headlessly, with no window.
 *
 * Keys: printable ASCII inserts, Enter splits a line, Backspace deletes,
 * arrows/Home/End move, ESC closes.  Clicking the text area focuses it.
 */

#define _POSIX_C_SOURCE 200809L   /* clock_gettime */

#include "auragui.h"
#include "aodoc.h"
#include "aoglyph.h"
#include "aomodel.h"
#include "aoundo.h"
#include "aoio.h"
#include "aolayout.h"
#include "wv_image.h"   /* OF3 closure: the gbrowser PNG decoder, shared */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---- geometry (content-area coordinates) -------------------------------- */

#define WIN_W        640
#define WIN_H        440

#define RULER_X      12
#define RULER_Y      10
#define RULER_H      18
#define RULER_CM     24          /* pixels per labelled tick */

#define PAGE_X       40
#define PAGE_Y       36
#define PAGE_W       560
#define PAGE_H       300

#define TEXT_X       (PAGE_X + 10)
#define TEXT_Y       (PAGE_Y + 10)
#define LINE_H       16
#define MAX_LINES    64
/* Bytes per line.  It is a BYTE cap, and Cyrillic costs two or three bytes a
 * letter: the demo line «Кириллица: съешь ещё этих мягких французских булок.»
 * is 30 characters but 94 bytes, so the old 96 left no room to type after it.
 * 512 holds a long Russian sentence; OF3 replaces the fixed rows with the
 * document buffer proper. */
#define MAX_COLS     512

/* OF1: the specimen strip at the top of the page — the phrase from the plan,
 * once per style, in both faces.  It is what makes "we can set mixed-script
 * text in three styles and two sizes" a thing you can SEE rather than a claim
 * in a document. */
#define SPEC_ROWS    4
#define SPEC_H       (SPEC_ROWS * LINE_H)
#define SPEC_Y       (PAGE_Y + 6)
#define EDIT_Y       (SPEC_Y + SPEC_H + 6)
#define VISIBLE      13           /* lines that fit below the specimen */

#define CARET_W      2

/* Key codes, matching the ag event set (see gui-edit/gedit.c). */
#define K_LEFT  0x100
#define K_RIGHT 0x101
#define K_UP    0x102
#define K_DOWN  0x103
#define K_HOME  0x104
#define K_END   0x105
#define K_PGUP  0x106
#define K_PGDN  0x107
#define K_DEL   0x109
#define K_F5    0x114   /* was 0x113 = F4: find-next sat on the wrong key */
#define K_F6    0x115
#define K_F7    0x116
#define K_F8    0x117
#define K_F9    0x118
#define K_F10   0x119

/* Modifier bits as the compositor delivers them (KB_MOD_*). */
#define M_SHIFT 0x01
#define M_CTRL  0x02
#define M_ALT   0x04

/* Where this program saves when the user does not say otherwise.  /documents
 * is the suite's volume (OF0 created it; docs/office.md records whether it
 * survives a reboot on the machine asking). */
#define DOC_PATH "/documents/Untitled.aod"
/* The headless gate writes its receipt where the integration case can cat it. */
#define WRITER_TEST_PATH "/documents/writer-gate.aod"

/* ---- document state ----------------------------------------------------- */

static ao_arena_t arena;
static uint8_t    arena_mem[64 * 1024];       /* the document's whole world */
static ao_doc_t  *doc;
static ao_canvas_t canvas;

static int  wid = -1;
static int  focused = 1;

static char lines[MAX_LINES][MAX_COLS];
static int  n_lines = 1;
static int  cur_line = 0, cur_col = 0;
static int  top_line = 0;

/* ---- OF1: the text layer ------------------------------------------------
 *
 * The faces are DATA, loaded at runtime from /lib/fonts through libaoglyph.  A
 * user can drop a third BDF there and the suite picks it up without a rebuild,
 * which is the point of shipping a documented format instead of a blob.  If
 * neither face loads (a stripped image), the window falls back to the console's
 * CP437 face and says so in its status line: refusal, not a blank page.
 */
#define FONT_SANS   "/lib/fonts/ao_sans_11.bdf"
#define FONT_MONO   "/lib/fonts/ao_mono_16.bdf"
#define FONT_ARENA  (192 * 1024)
#define FONT_FILE   (128 * 1024)

static ao_font_t  font_sans, font_mono;
static ao_arena_t font_arena_sans, font_arena_mono;
static uint8_t    font_mem_sans[FONT_ARENA], font_mem_mono[FONT_ARENA];
static uint8_t    font_file[FONT_FILE];
static const ao_font_t *font_body;      /* the face the page is set in */

/* One text line, rendered in this process and handed to the compositor as a
 * band: 540x16 pixels per line instead of a syscall per glyph. */
static uint32_t band[(PAGE_W - 20) * LINE_H];

/* ---- timing ------------------------------------------------------------- */

static long now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000000L + (long)(ts.tv_nsec / 1000L);
}

/* The dirty area of the last present, in pixels.  This — not the syscall
 * time — is what decides how much work the compositor is handed, and it is
 * exact on any machine. */
static long last_pixels;

/* ---- OF2: the document model -------------------------------------------
 *
 * OF0 kept the text in `lines[][]` and called that the edit model.  It was a
 * fine floor and it is the wrong foundation: a word processor needs
 * paragraphs, runs of style, a byte-exact file format and undo that cannot
 * drift out of step with either.  All four now live in libaodoc, and this file
 * is the VIEW: `md` owns the text, `lines[][]` is a display cache rebuilt from
 * it, and every keystroke goes through the model rather than around it.
 *
 * The caret is a BYTE OFFSET into the model, never a (line, column) pair --
 * that is the bug this rewrite exists to avoid: with Cyrillic at two bytes a
 * letter, a column index is one edit away from landing inside a codepoint.
 *
 * Undo stores snapshots (aoundo.h), so the stack costs memory rather than
 * cleverness; 256 KiB a side keeps hundreds of edits of a two-page document.
 */
#define MD_TEXT_BYTES (48u * 1024u)
#define MD_RUNS       2048u
#define MD_PARAS      1024u
#define IO_MAX        (96u * 1024u)

static char       md_text[MD_TEXT_BYTES];
static ao_run_t   md_runs[MD_RUNS];
static ao_para_t  md_paras[MD_PARAS];
static ao_model_t md;
static ao_undo_t  undo;
static uint8_t    undo_mem[AO_UNDO_DEFAULT_BYTES];
static uint8_t    redo_mem[AO_UNDO_DEFAULT_BYTES];

static uint32_t caret = 0;            /* byte offset into md_text */
static uint32_t sel_anchor = 0;
static int      sel_on = 0;
static int      drag_select = 0;   /* mouse button held in the text area */
static uint8_t  type_style = AO_STYLE_OFF;   /* what the next keystroke gets */
static int      dirty = 0;
static char     doc_path[96] = DOC_PATH;

/* A one-line prompt: find and replace need input, and a modal strip costs far
 * less than a dialog. */
static int  prompt = 0;               /* 0 none, 1 find, 2 replace */
static char prompt_buf[64];
static int  prompt_len = 0;
static char last_find[64] = "";

static uint32_t line_off[MAX_LINES];  /* byte offset of each display line */

/* ---- the display cache -------------------------------------------------- */

static void md_sync(void) {
    uint32_t len = ao_model_text_len(&md);
    if (caret > len) caret = len;

    /* caret -> (line, column), the only place a column is derived */
    int li = 0, col = 0;
    for (uint32_t i = 0; i < caret; i++) {
        if (md_text[i] == '\n') { li++; col = 0; } else col++;
    }

    n_lines = 0;
    uint32_t off = 0;
    while (n_lines < MAX_LINES) {
        uint32_t e = off;
        while (e < len && md_text[e] != '\n') e++;
        line_off[n_lines] = off;
        uint32_t n = e - off;
        if (n > MAX_COLS - 1) {
            /* A byte cap, cut on a codepoint edge: half a Cyrillic letter would
             * render as a replacement box for the rest of the session. */
            n = MAX_COLS - 1;
            while (n > 0 && ((unsigned char)md_text[off + n] & 0xC0) == 0x80) n--;
        }
        memcpy(lines[n_lines], md_text + off, n);
        lines[n_lines][n] = '\0';
        n_lines++;
        if (e >= len) break;
        off = e + 1;
    }
    if (n_lines == 0) { n_lines = 1; line_off[0] = 0; lines[0][0] = '\0'; }
    cur_line = li < n_lines ? li : n_lines - 1;
    cur_col  = col;
}

/* Byte length of display line `idx`, terminator excluded. */
static uint32_t line_len(int idx) {
    if (idx < 0 || idx >= n_lines) return 0;
    return (uint32_t)strlen(lines[idx]);
}

static void md_dirty(void) {
    dirty = 1;
    if (doc) ao_doc_touch(doc);
}

/* ---- selection ---------------------------------------------------------- */

static void sel_bounds(uint32_t *a, uint32_t *b) {
    if (!sel_on) { *a = *b = caret; return; }
    *a = caret < sel_anchor ? caret : sel_anchor;
    *b = caret < sel_anchor ? sel_anchor : caret;
}
static void sel_clear(void) { sel_on = 0; sel_anchor = caret; }
static int  sel_len(void) { uint32_t a, b; sel_bounds(&a, &b); return (int)(b - a); }

/* Select [a, b) and put the caret at b. */
static void sel_set(uint32_t a, uint32_t b) {
    sel_anchor = a;
    caret = b;
    sel_on = (a != b);
    md_sync();
}

/* ---- editing ------------------------------------------------------------
 * One rule: the model changes first, the cache follows.  Nothing here writes
 * to `lines[][]` directly, which is why the two can never disagree.
 */

static void ed_reset(void) {
    ao_model_clear(&md);
    ao_undo_reset(&undo);
    caret = sel_anchor = 0;
    sel_on = 0;
    type_style = AO_STYLE_OFF;
    dirty = 0;
    top_line = 0;
    memset(lines, 0, sizeof(lines));
    md_sync();
}

/* Replace the selection (or insert at the caret) with n bytes of UTF-8. */
static void ed_replace(const char *s, uint32_t n, uint8_t kind) {
    uint32_t a, b;
    sel_bounds(&a, &b);
    ao_undo_push(&undo, &md, kind);
    if (b > a) {
        ao_model_delete(&md, a, b - a);
        caret = a;
    }
    if (n) {
        if (ao_model_insert(&md, caret, s, n) != AO_OK) { md_sync(); return; }
        if (type_style) ao_model_set_style(&md, caret, n, type_style, 1);
        caret += n;
    }
    sel_clear();
    md_dirty();
    md_sync();
}

static void ed_insert(char c) { ed_replace(&c, 1, AO_UNDO_TYPING); }

static void ed_insert_cp(uint32_t cp) {
    char out[4];
    int n = ao_utf8_encode(cp, out);
    if (n > 0) ed_replace(out, (uint32_t)n, AO_UNDO_TYPING);
}

static void ed_insert_utf8(const char *s) {
    ed_replace(s, (uint32_t)strlen(s), AO_UNDO_EDIT);
}

static void ed_newline(void) { ed_replace("\n", 1, AO_UNDO_EDIT); }

/* Delete the selection, or the codepoint before the caret. */
static void ed_backspace(void) {
    uint32_t a, b;
    sel_bounds(&a, &b);
    if (b > a) {
        ao_undo_push(&undo, &md, AO_UNDO_DELETE);
        ao_model_delete(&md, a, b - a);
        caret = a;
    } else if (caret > 0) {
        uint32_t p = ao_model_prev_cp(&md, caret);
        ao_undo_push(&undo, &md, AO_UNDO_DELETE);
        ao_model_delete(&md, p, caret - p);
        caret = p;
    } else {
        return;
    }
    sel_clear();
    md_dirty();
    md_sync();
}

/* Delete forward: the selection, or the codepoint under the caret. */
static void ed_delete_forward(void) {
    uint32_t a, b;
    sel_bounds(&a, &b);
    if (b > a) { ed_backspace(); return; }
    uint32_t nx = ao_model_next_cp(&md, caret);
    if (nx == caret) return;
    ao_undo_push(&undo, &md, AO_UNDO_DELETE);
    ao_model_delete(&md, caret, nx - caret);
    sel_clear();
    md_dirty();
    md_sync();
}

/* Move the caret.  With `extend` the anchor stays put and the caret drags a
 * selection behind it; without it the selection collapses. */
static void ed_move_ext(uint32_t key, int extend) {
    uint32_t len = ao_model_text_len(&md);
    uint32_t target = caret;
    uint32_t col = (uint32_t)cur_col;

    switch (key) {
        case K_LEFT:  target = ao_model_prev_cp(&md, caret); break;
        case K_RIGHT: target = ao_model_next_cp(&md, caret); break;
        case K_UP:    if (cur_line > 0) target = line_off[cur_line - 1];
                      if (cur_line > 0) {
                          uint32_t l = (uint32_t)strlen(lines[cur_line - 1]);
                          target += col < l ? col : l;
                          while (target > line_off[cur_line - 1] &&
                                 ((unsigned char)md_text[target] & 0xC0) == 0x80) target--;
                      }
                      break;
        case K_DOWN:  if (cur_line + 1 < n_lines) {
                          target = line_off[cur_line + 1];
                          uint32_t l = (uint32_t)strlen(lines[cur_line + 1]);
                          target += col < l ? col : l;
                          while (target > line_off[cur_line + 1] &&
                                 ((unsigned char)md_text[target] & 0xC0) == 0x80) target--;
                      }
                      break;
        case K_PGUP:  { int t = cur_line - VISIBLE; if (t < 0) t = 0;
                        target = line_off[t] + (col < (uint32_t)strlen(lines[t]) ? col : (uint32_t)strlen(lines[t])); }
                      break;
        case K_PGDN:  { int t = cur_line + VISIBLE; if (t > n_lines - 1) t = n_lines - 1;
                        target = line_off[t] + (col < (uint32_t)strlen(lines[t]) ? col : (uint32_t)strlen(lines[t])); }
                      break;
        case K_HOME:  target = line_off[cur_line]; break;
        case K_END:   target = line_off[cur_line] + (uint32_t)strlen(lines[cur_line]); break;
        default:      return;
    }
    if (target > len) target = len;

    if (extend) {
        if (!sel_on) { sel_on = 1; sel_anchor = caret; }
        caret = target;
    } else {
        caret = target;
        sel_clear();
    }
    /* A caret move ends the coalescing run: typing before and after an arrow
     * key are two undo steps, which is what every editor does. */
    ao_undo_break(&undo);
    md_sync();
}

static void ed_move(uint32_t key) { ed_move_ext(key, 0); }

/* Move to an absolute byte offset (word jumps, Ctrl+Home/End, mouse). */
static void ed_move_to(uint32_t target, int extend) {
    uint32_t len = ao_model_text_len(&md);
    if (target > len) target = len;
    if (extend) {
        if (!sel_on) { sel_on = 1; sel_anchor = caret; }
        caret = target;
        if (caret == sel_anchor) sel_on = 0;
    } else {
        caret = target;
        sel_clear();
    }
    ao_undo_break(&undo);
    md_sync();
}

/* ---- OF2 closure: clipboard (kernel text clipboard, 512-byte cap) ------- */
#define CLIP_MAX 512
static char clip_buf[CLIP_MAX];

static int ed_copy(void) {
    uint32_t a, b;
    sel_bounds(&a, &b);
    if (b <= a) { printf("[writer] clipboard: nothing selected\n"); return -1; }
    if (b - a >= CLIP_MAX) {
        printf("[writer] clipboard: selection of %u bytes exceeds the %d-byte text clipboard; refused, not truncated\n",
               b - a, CLIP_MAX);
        return -1;
    }
    if (ao_model_copy_text(&md, a, b - a, clip_buf, sizeof clip_buf) < 0) return -1;
    if (ag_set_clipboard(clip_buf) != 0) {
        printf("[writer] clipboard: the compositor refused the text\n");
        return -1;
    }
    printf("[writer] clipboard: copied %u bytes\n", b - a);
    return 0;
}

static void ed_cut(void) {
    if (ed_copy() != 0) return;
    ed_replace("", 0, AO_UNDO_EDIT);
    printf("[writer] clipboard: cut\n");
}

static void ed_paste(void) {
    if (ag_get_clipboard(clip_buf, sizeof clip_buf) != 0) {
        printf("[writer] clipboard: nothing to paste\n");
        return;
    }
    size_t n = strlen(clip_buf);
    if (!ao_utf8_valid(clip_buf, n)) {
        printf("[writer] clipboard: clipboard text is not valid UTF-8; refused\n");
        return;
    }
    ed_replace(clip_buf, (uint32_t)n, AO_UNDO_EDIT);
    printf("[writer] clipboard: pasted %u bytes\n", (unsigned)n);
}

/* ---- OF2 closure: pixel -> caret (mouse) -------------------------------- */
static uint32_t caret_from_xy(int x, int y) {
    int row = top_line;
    if (y >= EDIT_Y) row = top_line + (y - EDIT_Y) / LINE_H;
    if (row < 0) row = 0;
    if (row >= n_lines) row = n_lines - 1;
    const char *text = lines[row];
    size_t len = strlen(text);
    int want = x - TEXT_X;
    if (want <= 0) return line_off[row];
    size_t pos = 0, best = 0;
    if (!font_body) {
        best = (size_t)(want / 8);
        if (best > len) best = len;
        while (best > 0 && best < len && ((unsigned char)text[best] & 0xC0) == 0x80) best--;
        return line_off[row] + (uint32_t)best;
    }
    int wbest = 0;
    while (pos < len) {
        size_t nxt = pos + 1;
        while (nxt < len && ((unsigned char)text[nxt] & 0xC0) == 0x80) nxt++;
        int wn = ao_text_width(font_body, text, nxt);
        if (want <= wbest + (wn - wbest) / 2) break;
        best = nxt; wbest = wn; pos = nxt;
    }
    return line_off[row] + (uint32_t)best;
}


/* ---- style, undo, select ------------------------------------------------ */

static void ed_style(uint8_t bits) {
    uint32_t a, b;
    sel_bounds(&a, &b);
    if (b > a) {
        int on = (ao_model_style_at(&md, a) & bits) == 0;
        ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
        ao_model_set_style(&md, a, b - a, bits, on);
        md_dirty();
    } else {
        /* Nothing selected: the bit applies to what is typed next, which is
         * what pressing Ctrl+B before typing does in every word processor. */
        type_style ^= bits;
    }
    ao_undo_break(&undo);
    md_sync();
}

static void ed_select_all(void) {
    sel_anchor = 0;
    caret = ao_model_text_len(&md);
    sel_on = (caret != 0);
    ao_undo_break(&undo);
    md_sync();
}

static void ed_align(uint8_t align) {
    ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
    ao_model_set_align(&md, caret, align);
    ao_undo_break(&undo);
    md_dirty();
    md_sync();
}

static void ed_undo(void) {
    if (ao_undo_undo(&undo, &md) != AO_OK) return;
    sel_clear();
    md_dirty();
    md_sync();
}

static void ed_redo(void) {
    if (ao_undo_redo(&undo, &md) != AO_OK) return;
    sel_clear();
    md_dirty();
    md_sync();
}

/* ---- find and replace --------------------------------------------------- */

/* Find the next occurrence of `needle` after the caret, wrapping once.  A hit
 * is SELECTED, so Ctrl+H replaces exactly what the user can see. */
static int ed_find(const char *needle, int from_caret) {
    if (!needle || !*needle) return 0;
    printf("[writer] find \"%s\": %u match(es)\n", needle,
           ao_model_count(&md, needle, 1));
    int64_t at = ao_model_find(&md, from_caret ? caret : 0, needle, 1);
    if (at < 0 && caret > 0) at = ao_model_find(&md, 0, needle, 1);   /* wrap */
    if (at < 0) return 0;
    sel_set((uint32_t)at, (uint32_t)at + (uint32_t)strlen(needle));
    return 1;
}

static void ed_replace_sel(const char *with) {
    if (!sel_on) return;
    ed_replace(with, (uint32_t)strlen(with), AO_UNDO_EDIT);
}

/* ---- files -------------------------------------------------------------
 * The whole point of the XML format: a document this program saves is a file
 * `cat` can print and a second instance can open, with no private state.
 */
static char io_buf[IO_MAX];
static char io_buf2[IO_MAX];

static int ed_save_as(const char *path) {
    size_t n = 0;
    if (ao_io_export(&md, io_buf, sizeof io_buf, &n) != AO_OK) {
        printf("[writer] export refused (document too large?)\n");
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("[writer] cannot open %s for writing\n", path); return -1; }
    ssize_t w = write(fd, io_buf, n);
    close(fd);
    if (w != (ssize_t)n) { printf("[writer] short write on %s\n", path); return -1; }
    printf("[writer] saved %s: %u bytes of XML, %u paragraphs, %u runs, %u undo steps\n",
           path, (unsigned)n, ao_model_para_count(&md), ao_model_run_count(&md),
           ao_undo_depth(&undo));
    dirty = 0;
    return 0;
}

static int ed_load_from(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("[writer] cannot open %s\n", path); return -1; }
    ssize_t n = read(fd, io_buf, sizeof io_buf - 1);
    close(fd);
    if (n <= 0) { printf("[writer] %s is empty or unreadable\n", path); return -1; }
    io_buf[n] = '\0';
    int rc = ao_io_import(&md, io_buf, (size_t)n);
    if (rc != AO_OK) {
        /* ao_io_import leaves the document EMPTY on failure by contract, so the
         * old one is gone; say so rather than show a half-loaded page. */
        printf("[writer] %s is not a document this version reads (rc=%d)\n", path, rc);
        md_sync();
        return -1;
    }
    ao_undo_reset(&undo);
    ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
    caret = 0;
    sel_clear();
    dirty = 0;
    snprintf(doc_path, sizeof doc_path, "%s", path);
    printf("[writer] opened %s: %u bytes, %u paragraphs, %u runs\n", path,
           ao_model_text_len(&md), ao_model_para_count(&md), ao_model_run_count(&md));
    md_sync();
    return 0;
}


/* ---- OF3: page layout ---------------------------------------------------
 * The same ao_layout_build the host gate grades, with the window's face as
 * the measuring function.  Layout runs on demand (documents are <= 48 KiB)
 * and the result is what the page view paints and the status line counts. */
static ao_layout_line_t lay_lines[2048];
static ao_layout_page_t lay_pages[128];
static ao_layout_t lay;
static ao_page_geom_t lay_geom;
static int view_page = 0;            /* 0 draft rows, 1 page view/preview */

static int lay_measure(void *ctx, const char *text, size_t len, uint8_t size_px) {
    (void)ctx;
    if (!font_body) return (int)len * (size_px ? size_px : 11);
    int w = ao_text_width(font_body, text, len);
    return w * (size_px ? size_px : 11) / 11;
}

static int lay_image_size(void *ctx, const char *path, uint32_t len,
                          uint32_t *w, uint32_t *h) {
    (void)ctx;
    char pth[128];
    if (len >= sizeof pth) return -1;
    memcpy(pth, path, len);
    pth[len] = 0;
    int fd = open(pth, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t hdr[24];
    ssize_t n = read(fd, hdr, sizeof hdr);
    close(fd);
    if (n < (ssize_t)sizeof hdr) return -1;
    return ao_png_size(hdr, (size_t)n, w, h);
}

static int lay_rebuild(void) {
    ao_layout_init(&lay, lay_lines, 2048, lay_pages, 128);
    return ao_layout_build(&lay, &md, &lay_geom, lay_measure, NULL,
                           lay_image_size, NULL);
}

/* Apply a paragraph style to the paragraph at the caret (the style picker:
 * Ctrl+1..8).  One undo step, like every other structural verb. */
static void ed_para_style(uint8_t style) {
    ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
    ao_model_set_para_style(&md, caret, style);
    ao_undo_break(&undo);
    md_dirty();
    md_sync();
    printf("[writer] paragraph style -> %u (page %s)\n", style & AO_PARA_KIND_MASK,
           (style & AO_PARA_PAGEBREAK) ? "break" : "-");
}

/* Insert a rows x cols TABLE block at the caret: one TABLE paragraph per
 * row, header text in the first. */
static void ed_insert_table(int rows, int cols) {
    char buf[1024];
    int k = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s%c %d",
                          c ? "\t" : "", r ? 'R' : 'H', c + 1);
        }
        if (r + 1 < rows) buf[k++] = '\n';
    }
    buf[k] = 0;
    uint32_t at = caret;
    ed_replace(buf, (uint32_t)k, AO_UNDO_EDIT);
    /* Style every row paragraph of the block: scan the inserted bytes for
     * the newlines that separate its rows. */
    uint32_t pos = at;
    for (int r = 0; r < rows; r++) {
        ao_model_set_para_style(&md, pos, AO_PARA_TABLE);
        while (pos < at + (uint32_t)k && md_text[pos] != '\n') pos++;
        pos++;   /* the newline (or one past the block on the last row) */
    }
    md_dirty();
    md_sync();
    printf("[writer] inserted a %dx%d table at byte %u\n", rows, cols, at);
}

/* Insert/delete a row of the table the caret sits in (TABLE paragraphs). */
static void ed_table_row(int delete_row) {
    if ((ao_model_para_style_at(&md, caret) & AO_PARA_KIND_MASK) != AO_PARA_TABLE) {
        printf("[writer] table row: the caret is not in a table\n");
        return;
    }
    uint32_t pi = ao_model_para_of(&md, caret);
    uint32_t start = md_paras[pi].off;
    if (delete_row) {
        uint32_t n = md_paras[pi].len;
        ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
        if (pi + 1 >= md.para_len && start > 0) { start--; n++; } /* eat '\n' */
        ao_model_delete(&md, start, n);
        caret = start;
    } else {
        /* A fresh empty row below this one, same column count. */
        uint32_t cols = 1;
        for (uint32_t i = 0; i < md_paras[pi].len; i++)
            if (md_text[md_paras[pi].off + i] == '\t') cols++;
        char row[256]; int k = 0;
        for (uint32_t c = 1; c < cols; c++) row[k++] = '\t';
        row[k] = 0;
        uint32_t at = md_paras[pi].off + md_paras[pi].len;
        int had_nl = (at > 0 && md_text[at - 1] == '\n');
        ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
        const char *ins = had_nl ? row : "\n";
        if (had_nl) {
            ao_model_insert(&md, at, row, (uint32_t)k);
        } else {
            char tmp[260];
            snprintf(tmp, sizeof tmp, "\n%s", row);
            ao_model_insert(&md, at, tmp, (uint32_t)strlen(tmp));
            (void)ins;
        }
        ao_model_set_para_style(&md, at + (had_nl ? 0 : 1), AO_PARA_TABLE);
        caret = at;
    }
    ao_undo_break(&undo);
    sel_clear();
    md_dirty();
    md_sync();
    printf("[writer] table row %s\n", delete_row ? "deleted" : "inserted");
}

/* OF3 closure: insert (1) or delete (0) the table column the caret sits
 * in, in every row of the contiguous TABLE block.  A column is a tab slot:
 * inserting adds an empty slot at the same column index in each row,
 * deleting removes that slot (with one adjacent tab). */
static void ed_table_col(int insert) {
    if ((ao_model_para_style_at(&md, caret) & AO_PARA_KIND_MASK) != AO_PARA_TABLE) {
        printf("[writer] table column: the caret is not in a table\n");
        return;
    }
    uint32_t pi = ao_model_para_of(&md, caret);
    uint32_t col = 0;
    for (uint32_t i = md_paras[pi].off; i < caret && i < md_paras[pi].off + md_paras[pi].len; i++)
        if (md_text[i] == '\t') col++;
    /* The contiguous block of TABLE paragraphs around the caret's row. */
    uint32_t first = pi, last = pi;
    while (first > 0 && (md_paras[first - 1].style & AO_PARA_KIND_MASK) == AO_PARA_TABLE) first--;
    while (last + 1 < md.para_len && (md_paras[last + 1].style & AO_PARA_KIND_MASK) == AO_PARA_TABLE) last++;
    ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
    /* Bottom-up, so offsets of rows not yet edited stay valid. */
    for (uint32_t r = last + 1; r-- > first; ) {
        uint32_t off = md_paras[r].off, len = md_paras[r].len;
        if (len && md_text[off + len - 1] == '\n') len--;
        uint32_t cstart = off, cend = off + len, c = 0, i = off;
        while (c < col && i < off + len) { if (md_text[i] == '\t') c++; i++; }
        cstart = i;
        while (i < off + len && md_text[i] != '\t') i++;
        cend = i;
        if (insert) {
            /* New empty cell before column `col` (or appended at row end). */
            if (cstart >= off + len) ao_model_insert(&md, off + len, "\t", 1);
            else                     ao_model_insert(&md, cstart, "\t", 1);
        } else {
            if (cstart == cend && col == 0 && cend >= off + len) continue;
            if (cstart > off)      ao_model_delete(&md, cstart - 1, cend - cstart + 1);
            else if (cend < off + len) ao_model_delete(&md, cstart, cend - cstart + 1);
            else                   ao_model_delete(&md, cstart, cend - cstart);
        }
    }
    ao_undo_break(&undo);
    sel_clear();
    md_dirty();
    md_sync();
    printf("[writer] table column %s at column %u\n", insert ? "inserted" : "deleted", col);
}

/* OF3 closure: merge the caret's cell into the cell on its left -- the
 * AO_TABLE_MERGE_MARK byte the layout's row parser folds into a colspan. */
static void ed_table_merge(void) {
    if ((ao_model_para_style_at(&md, caret) & AO_PARA_KIND_MASK) != AO_PARA_TABLE) {
        printf("[writer] table merge: the caret is not in a table\n");
        return;
    }
    uint32_t pi = ao_model_para_of(&md, caret);
    uint32_t off = md_paras[pi].off, len = md_paras[pi].len;
    uint32_t cstart = off;
    for (uint32_t i = off; i < caret && i < off + len; i++)
        if (md_text[i] == '\t') cstart = i + 1;
    if (cstart == off) {
        printf("[writer] table merge: the first cell has no left neighbour\n");
        return;
    }
    if (md_text[cstart] == AO_TABLE_MERGE_MARK) {
        printf("[writer] table merge: already merged\n");
        return;
    }
    ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
    char mark = AO_TABLE_MERGE_MARK;
    ao_model_insert(&md, cstart, &mark, 1);
    ao_undo_break(&undo);
    sel_clear();
    md_dirty();
    md_sync();
    printf("[writer] table cell merged at byte %u\n", cstart);
}

/* Insert an IMAGE paragraph: the prompt (mode 4) collects the PNG path. */
static void ed_insert_image(const char *path) {
    char buf[160];
    snprintf(buf, sizeof buf, "\n%s", path);
    uint32_t at = caret;
    ed_replace(buf, (uint32_t)strlen(buf), AO_UNDO_EDIT);
    ao_model_set_para_style(&md, at + 1, AO_PARA_IMAGE);
    md_dirty();
    md_sync();
    printf("[writer] image paragraph: %s\n", path);
}

static int ed_save(void) { return ed_save_as(doc_path); }

/* ---- OF2 closure: the shared open/save dialog ----------------------------
 * One modal strip over the page, used by both verbs: a path box (the text
 * being edited), a listbox of /documents the Up/Down keys walk, and -- for
 * Save -- an overwrite confirmation when the named file already exists.
 * Ctrl+S stays the quick save to the current path; Ctrl+Shift+S and Ctrl+O
 * come here.  The dialog is state plus key handling, so the headless writer
 * gate can drive it without a window. */
#define DLG_MAX_ITEMS 32
static int  dialog = 0;              /* 0 none, 1 open, 2 save-as */
static int  dlg_confirm = 0;         /* save would overwrite: y/n pending */
static char dlg_buf[128] = "";
static int  dlg_sel = -1;
static char dlg_items[DLG_MAX_ITEMS][64];
static int  dlg_count = 0;

static void dlg_refresh(void) {
    /* aura_readdir, not opendir: the GUI file manager's own pattern
     * (gfiles.c).  opendir() first opens the directory O_DIRECTORY, which
     * the /documents tmpfs refuses -- the dialog listed nothing in the
     * guest while `ls /documents` worked, and this is why. */
    static struct aura_dirent ents[DLG_MAX_ITEMS];
    dlg_count = 0;
    dlg_sel = -1;
    int n = aura_readdir("/documents", ents, DLG_MAX_ITEMS);
    if (n <= 0) n = aura_readdir("/documents/", ents, DLG_MAX_ITEMS);
    printf("[writer] dialog: aura_readdir(/documents) -> %d entr%s\n",
           n, n == 1 ? "y" : "ies");
    if (n < 0) { printf("[writer] dialog: /documents cannot be listed\n"); return; }
    for (int i = 0; i < n && dlg_count < DLG_MAX_ITEMS; i++) {
        if (ents[i].name[0] == '.') continue;
        snprintf(dlg_items[dlg_count], sizeof dlg_items[0], "%s", ents[i].name);
        dlg_count++;
    }
    if (dlg_count > 0) dlg_sel = 0;
}

static void repaint_full(void);   /* defined with the painters below */
static void paint_dialog(void);
static void paint_status(void);
static int  publish(void);

static void dlg_open(int mode) {
    dialog = mode;
    dlg_confirm = 0;
    snprintf(dlg_buf, sizeof dlg_buf, "%s", mode == 2 ? doc_path : "/documents/");
    dlg_refresh();
    printf("[writer] dialog: %s (%d file(s) in /documents)\n",
           mode == 2 ? "save as" : "open", dlg_count);
    /* Present the dialog at once, as its own frame on top of a full repaint.
     * In the live VNC pass a dialog drawn only by the keystroke tail that
     * opened it never reached the screen (its serial receipts printed, its
     * pixels did not); presenting it here does.  The tail then redraws the
     * lines + dialog in its usual frame, which keeps the overlay up. */
    if (wid >= 0) {
        repaint_full();
        ao_canvas_clear(&canvas);
        paint_dialog();
        paint_status();
        publish();
    }
}

static void dlg_pick(void) {
    if (dlg_sel >= 0 && dlg_sel < dlg_count)
        snprintf(dlg_buf, sizeof dlg_buf, "/documents/%s", dlg_items[dlg_sel]);
}

/* 1 while the dialog is open (the key was consumed). */
static int dlg_key(uint32_t key, uint32_t cp) {
    if (!dialog) return 0;
    if (dlg_confirm) {
        if (key == 'y' || key == 'Y') {
            dlg_confirm = 0; dialog = 0;
            if (ed_save_as(dlg_buf) == 0)
                snprintf(doc_path, sizeof doc_path, "%s", dlg_buf);
        } else if (key == 'n' || key == 'N' || key == 0x1B) {
            dlg_confirm = 0;
            printf("[writer] dialog: overwrite refused, file untouched\n");
        }
        return 1;
    }
    if (key == 0x1B) { dialog = 0; printf("[writer] dialog: cancelled\n"); return 1; }
    if (key == '\n' || key == '\r') {
        if (dialog == 1) {
            dialog = 0;
            ed_load_from(dlg_buf);
        } else {
            int fd = open(dlg_buf, O_RDONLY);
            if (fd >= 0) {   /* exists: ask before truncating somebody's file */
                close(fd);
                dlg_confirm = 1;
                printf("[writer] dialog: %s exists -- overwrite? (y/n)\n", dlg_buf);
            } else {
                dialog = 0;
                if (ed_save_as(dlg_buf) == 0)
                    snprintf(doc_path, sizeof doc_path, "%s", dlg_buf);
            }
        }
        return 1;
    }
    if (key == K_UP || key == K_DOWN) {
        if (dlg_count > 0) {
            dlg_sel += (key == K_DOWN) ? 1 : -1;
            if (dlg_sel < 0) dlg_sel = 0;
            if (dlg_sel >= dlg_count) dlg_sel = dlg_count - 1;
            dlg_pick();
        }
        return 1;
    }
    if (key == '\b') {
        size_t n = strlen(dlg_buf);
        if (n) dlg_buf[ao_utf8_prev(dlg_buf, n)] = '\0';
        return 1;
    }
    char enc[4]; int n = 0;
    if (cp >= 0x20 && cp != 0x7F) n = ao_utf8_encode(cp, enc);
    else if (key >= 0x20 && key < 0x7F) { enc[0] = (char)key; n = 1; }
    if (n > 0 && strlen(dlg_buf) + (size_t)n < sizeof dlg_buf) {
        size_t at = strlen(dlg_buf);
        memcpy(dlg_buf + at, enc, (size_t)n);
        dlg_buf[at + (size_t)n] = '\0';
    }
    return 1;
}

static void paint_dialog(void) {
    if (!dialog) return;
    ag_fill_rect(wid, PAGE_X, PAGE_Y, PAGE_W, PAGE_H, AG_WHITE);
    ag_draw_rect(wid, PAGE_X, PAGE_Y, PAGE_W, PAGE_H, AG_DARK);
    char hdr[96];
    snprintf(hdr, sizeof hdr, "%s  (Enter confirms, Esc cancels%s)",
             dialog == 2 ? "Save as:" : "Open:",
             dialog == 2 ? ", existing file asks first" : "");
    ag_draw_text(wid, TEXT_X, PAGE_Y + 8, hdr, AG_DARK);
    ag_draw_text(wid, TEXT_X, PAGE_Y + 28, dlg_buf, AG_BLACK);
    ag_draw_line(wid, TEXT_X, PAGE_Y + 44, TEXT_X + PAGE_W - 20, PAGE_Y + 44, AG_GRAY);
    for (int i = 0; i < dlg_count && i < 12; i++) {
        int y = PAGE_Y + 52 + i * LINE_H;
        if (i == dlg_sel) ag_fill_rect(wid, TEXT_X, y, PAGE_W - 20, LINE_H, AG_ACCENT);
        ag_draw_text(wid, TEXT_X + 4, y + 2, dlg_items[i], i == dlg_sel ? AG_WHITE : AG_BLACK);
    }
    if (dlg_confirm) {
        char q[160];
        snprintf(q, sizeof q, "%s exists -- overwrite? (y/n)", dlg_buf);
        ag_fill_rect(wid, TEXT_X, PAGE_Y + PAGE_H - 24, PAGE_W - 20, 18, AG_YELLOW);
        ag_draw_text(wid, TEXT_X + 4, PAGE_Y + PAGE_H - 22, q, AG_BLACK);
    }
    ao_canvas_mark(&canvas, PAGE_X, PAGE_Y, PAGE_W, PAGE_H);
}


/* ---- geometry of the caret --------------------------------------------- */

static void scroll_to_caret(void) {
    if (cur_line < top_line) top_line = cur_line;
    if (cur_line >= top_line + VISIBLE) top_line = cur_line - VISIBLE + 1;
    if (top_line < 0) top_line = 0;
}

/* The caret sits where the text layer says the pen ended up: a proportional
 * face makes "column times eight" a lie, and on a Cyrillic line it is a
 * visible one. */
static int caret_x(void) {
    if (!font_body) return TEXT_X + cur_col * 8;
    int len = (int)line_len(cur_line);
    int at = cur_col < len ? cur_col : len;
    return TEXT_X + ao_text_width(font_body, lines[cur_line], (size_t)at);
}
static int caret_y(void) { return EDIT_Y + (cur_line - top_line) * LINE_H; }

/* ---- painting -----------------------------------------------------------
 *
 * Each routine draws into the window and marks what it touched in the canvas.
 * Presenting is a separate, single call (see publish()).
 */

/* Load one BDF face into its own arena.  Returns 0 or a negative receipt. */
static int load_face(const char *path, ao_font_t *f, ao_arena_t *a, uint8_t *mem) {
    ao_arena_init(a, mem, FONT_ARENA);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long r = (long)read(fd, font_file + total, FONT_FILE - total);
        if (r <= 0) break;
        total += r;
        if (total >= FONT_FILE) break;
    }
    close(fd);
    if (total <= 0) return -2;
    int rc = ao_font_load_bdf(f, a, (const char *)font_file, (size_t)total);
    if (rc != AO_FONT_OK) return rc;
    return 0;
}

/* Load both faces once.  Called by every mode that paints, so the budget
 * benchmark measures the SAME paint path the window uses -- a benchmark of a
 * fallback path nobody sees would be a number about nothing. */
static void ensure_faces(void) {
    if (!font_sans.n &&
        load_face(FONT_SANS, &font_sans, &font_arena_sans, font_mem_sans) == 0)
        font_body = &font_sans;
    if (!font_mono.n)
        load_face(FONT_MONO, &font_mono, &font_arena_mono, font_mem_mono);
}

/* Render one run of UTF-8 into the band and hand the band to the compositor.
 * `x` is relative to TEXT_X, `y` is a window coordinate. */
static void band_run(const ao_font_t *f, int x, int y, const char *s, size_t len,
                     int style, uint32_t fg) {
    if (!f) return;
    int bw = PAGE_W - 20;
    for (int i = 0; i < bw * LINE_H; i++) band[i] = AG_WHITE;
    int baseline = (LINE_H + f->ascent - f->descent) / 2;
    ao_text_draw(f, s, len, style, band, bw, bw, LINE_H, x, baseline, fg, NULL);
    ag_blit(wid, TEXT_X, y, (uint32_t)bw, LINE_H, band, (uint32_t)bw);
}

/* The console face, for the fallback path: exactly what OF0 did. */
static void console_run(int x, int y, const char *s, uint32_t fg) {
    ag_draw_text(wid, TEXT_X + x, y, s, fg);
}

static void paint_specimen(void) {
    /* Literal UTF-8, not \xDD escapes: a hand-written escape string silently
     * dropped characters the first time this was written ("французских" came
     * out as "фра"), and a specimen that is missing letters is worse than no
     * specimen at all.  The compiler has no opinion about the encoding. */
    static const char *phrase =
        "Привет, мир — 2026: съешь ещё этих мягких французских булок";
    size_t plen = strlen(phrase);
    ag_fill_rect(wid, TEXT_X, SPEC_Y, PAGE_W - 20, SPEC_H, AG_WHITE);
    if (font_body) {
        band_run(&font_sans, 0, SPEC_Y + 0 * LINE_H, phrase, plen, AO_STYLE_NONE,   AG_BLACK);
        band_run(&font_sans, 0, SPEC_Y + 1 * LINE_H, phrase, plen, AO_STYLE_BOLD,   AG_BLACK);
        band_run(&font_sans, 0, SPEC_Y + 2 * LINE_H, phrase, plen, AO_STYLE_ITALIC, AG_BLACK);
        band_run(&font_mono, 0, SPEC_Y + 3 * LINE_H, phrase, plen, AO_STYLE_NONE,   AG_BLACK);
    } else {
        console_run(0, SPEC_Y, "no /lib/fonts face: console text only", AG_DARK);
    }
    /* A hairline under the specimen: it is a label, not the document. */
    ag_draw_line(wid, TEXT_X, SPEC_Y + SPEC_H + 2, TEXT_X + PAGE_W - 20,
                 SPEC_Y + SPEC_H + 2, AG_GRAY);
    ao_canvas_mark(&canvas, TEXT_X, SPEC_Y, PAGE_W - 20, SPEC_H + 4);
}

static void paint_page_furniture(void) {
    ag_fill_rect(wid, 0, 0, WIN_W, WIN_H, AG_PANEL);

    /* Margin box: the page. */
    ag_fill_rect(wid, PAGE_X, PAGE_Y, PAGE_W, PAGE_H, AG_WHITE);
    ag_draw_rect(wid, PAGE_X, PAGE_Y, PAGE_W, PAGE_H, AG_DARK);
    ao_canvas_mark(&canvas, PAGE_X, PAGE_Y, PAGE_W, PAGE_H);

    /* Ruler above it. */
    ag_fill_rect(wid, RULER_X, RULER_Y, PAGE_W, RULER_H, AG_PANEL);
    ag_draw_line(wid, RULER_X, RULER_Y + RULER_H - 1,
                 RULER_X + PAGE_W, RULER_Y + RULER_H - 1, AG_DARK);
    for (int i = 0; i * RULER_CM <= (int)PAGE_W; i++) {
        int x = RULER_X + i * RULER_CM;
        int tall = (i % 5 == 0);
        ag_draw_line(wid, x, RULER_Y + RULER_H - 1,
                     x, RULER_Y + RULER_H - 1 - (tall ? 9 : 4), AG_DARK);
        if (tall) {
            char lbl[8];
            snprintf(lbl, sizeof lbl, "%d", i / 5);
            ag_draw_text(wid, x + 2, RULER_Y + 1, lbl, AG_DARK);
        }
    }
    ao_canvas_mark(&canvas, RULER_X, RULER_Y, PAGE_W, RULER_H);

    /* Status strip: what this window is and what it costs. */
    ag_fill_rect(wid, 12, PAGE_Y + PAGE_H + 8, WIN_W - 24, 20, AG_PANEL);
    ao_canvas_mark(&canvas, 12, PAGE_Y + PAGE_H + 8, WIN_W - 24, 20);
}

/* One display line, drawn run by run so the document's own styling is what
 * the page shows, with the selection painted UNDER the glyphs as an accent bar
 * -- a bar that cannot hide a letter is worth more than a colour inversion that
 * can.  Everything goes into one band and one blit per line: the compositor is
 * handed 540x16 pixels, not a syscall per glyph. */
static void paint_line(int idx) {
    int screen_row = idx - top_line;
    if (screen_row < 0 || screen_row >= VISIBLE) return;
    int y = EDIT_Y + screen_row * LINE_H;
    const char *text = (idx < n_lines) ? lines[idx] : "";

    if (font_body) {
        int bw = PAGE_W - 20;
        int baseline = (LINE_H + font_body->ascent - font_body->descent) / 2;
        for (int i = 0; i < bw * LINE_H; i++) band[i] = AG_WHITE;

        size_t len = strlen(text);
        uint32_t lstart = line_off[idx < n_lines ? idx : 0];
        uint32_t salo = 0, sahi = 0;
        uint32_t a, b;
        sel_bounds(&a, &b);
        if (sel_on && b > a) { salo = a; sahi = b; }

        int x = 0;
        size_t q = 0;
        while (q < len) {
            /* The run in force at this byte decides style, size and face. */
            uint32_t at = lstart + (uint32_t)q;
            uint8_t st = ao_model_style_at(&md, at);
            uint8_t sz = ao_model_size_at(&md, at);
            uint8_t fo = ao_model_font_at(&md, at);
            size_t e = q;
            while (e < len && ao_model_style_at(&md, lstart + (uint32_t)e) == st &&
                   ao_model_size_at(&md, lstart + (uint32_t)e) == sz &&
                   ao_model_font_at(&md, lstart + (uint32_t)e) == fo) e++;
            int style = 0;
            if (st & AO_STYLE_BOLD) style |= AO_STYLE_BOLD;
            if (st & AO_STYLE_ITALIC) style |= AO_STYLE_ITALIC;
            if (st & AO_STYLE_UNDERLINE) style |= AO_STYLE_UNDERLINE;
            (void)sz; (void)fo;   /* one face per page until OF3's layout */

            /* Selection bar for the part of this run that is selected. */
            uint32_t s0 = lstart + (uint32_t)q, s1 = lstart + (uint32_t)e;
            if (sahi > salo && s1 > salo && s0 < sahi) {
                uint32_t lo = s0 < salo ? salo : s0;
                uint32_t hi = s1 > sahi ? sahi : s1;
                int x1 = x + ao_text_width(font_body, text + q, (size_t)(lo - s0));
                int x2 = x + ao_text_width(font_body, text + q, (size_t)(hi - s0));
                if (x2 > x1) {
                    for (int r = LINE_H - 4; r < LINE_H - 1; r++) {
                        for (int c = x1; c < x2 && c < bw; c++) band[r * bw + c] = AG_ACCENT;
                    }
                }
            }
            ao_text_draw(font_body, text + q, e - q, style, band, bw, bw, LINE_H,
                         x, baseline, AG_BLACK, NULL);
            x += ao_text_width(font_body, text + q, e - q);
            q = e;
        }
        ag_blit(wid, TEXT_X, y, (uint32_t)bw, LINE_H, band, (uint32_t)bw);
    } else {
        ag_fill_rect(wid, TEXT_X, y, PAGE_W - 20, LINE_H, AG_WHITE);
        ag_draw_text(wid, TEXT_X, y, text, AG_BLACK);
    }
    ao_canvas_mark(&canvas, TEXT_X, y, PAGE_W - 20, LINE_H);
}

static void paint_caret(void) {
    int y = caret_y();
    if (y < EDIT_Y || y >= EDIT_Y + VISIBLE * LINE_H) return;
    ag_fill_rect(wid, caret_x(), y, CARET_W, LINE_H - 2, AG_ACCENT);
    ao_canvas_mark(&canvas, caret_x(), y, CARET_W, LINE_H - 2);
}

static void paint_text(void) {
    ag_fill_rect(wid, TEXT_X, EDIT_Y, PAGE_W - 20, VISIBLE * LINE_H, AG_WHITE);
    ao_canvas_mark(&canvas, TEXT_X, EDIT_Y, PAGE_W - 20, VISIBLE * LINE_H);
    for (int i = 0; i < VISIBLE; i++) {
        int idx = top_line + i;
        if (idx >= n_lines) break;
        paint_line(idx);
    }
}

static void paint_status(void) {
    char s[160];
    if (prompt) {
        snprintf(s, sizeof s, "%s%s_", prompt == 1 ? "Find: " : (prompt == 3 ? "Replace all with: " : (prompt == 4 ? "Image PNG path: " : "Replace with: ")), prompt_buf);
    } else {
        char st[8] = "---";
        if (type_style & AO_STYLE_BOLD)      st[0] = 'B';
        if (type_style & AO_STYLE_ITALIC)    st[1] = 'I';
        if (type_style & AO_STYLE_UNDERLINE) st[2] = 'U';
        lay_geom = ao_page_geom_default();
        int lp = (lay_rebuild() == AO_OK) ? ao_layout_page_of(&lay, &md, caret) : -1;
        snprintf(s, sizeof s, "%s%s  |  %u:%d  sel %d  %s  undo %u/%u  %s p.%d/%u%s",
                 doc ? doc->title : "?", dirty ? " *" : "",
                 (unsigned)cur_line + 1, cur_col + 1, sel_len(), st,
                 ao_undo_depth(&undo), ao_undo_redo_depth(&undo),
                 view_page ? "PAGE" : "draft", lp + 1, lay.page_len,
                 font_body ? "" : "  (no font)");
    }
    ag_fill_rect(wid, 12, PAGE_Y + PAGE_H + 8, WIN_W - 24, 20, AG_PANEL);
    ag_draw_text(wid, 16, PAGE_Y + PAGE_H + 10, s, AG_DARK);
    ao_canvas_mark(&canvas, 12, PAGE_Y + PAGE_H + 8, WIN_W - 24, 20);
}

/* Present whatever the canvas planned.  This is the only place that talks to
 * the compositor, and it is why the plan is a pure function. */
static int publish(void) {
    ao_rect_t plan[AO_CANVAS_MAX_RECTS];
    int n = ao_canvas_plan(&canvas, plan, AO_CANVAS_MAX_RECTS);
    if (n <= 0) return 0;
    long px = 0;
    for (int i = 0; i < n; i++) {
        ag_window_invalidate_rect(wid, plan[i].x, plan[i].y, plan[i].w, plan[i].h);
        px += (long)plan[i].w * (long)plan[i].h;
    }
    last_pixels = px;
    ag_render_now();
    ao_canvas_clear(&canvas);
    return n;
}

/* OF3 closure: decoded pixels for the image paragraph on the page.
 * One decode, cached by path: wv_image_decode is the gbrowser decoder, so
 * the Writer and the browser read a PNG with the same code. */
static char     img_cache_path[128] = "";
static uint32_t *img_px = NULL;
static int      img_w = 0, img_h = 0;
static uint8_t  img_file[512 * 1024];
static uint32_t img_scaled[(PAGE_W - 20) * 120];

static int load_image_pixels(const char *path) {
    if (img_px && !strcmp(img_cache_path, path)) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long r = (long)read(fd, img_file + total, (long)sizeof img_file - total);
        if (r <= 0) break;
        total += r;
        if (total >= (long)sizeof img_file) break;
    }
    close(fd);
    if (total <= 0) return -1;
    free(img_px);
    img_px = NULL;
    if (wv_image_decode(img_file, (size_t)total, &img_px, &img_w, &img_h) != 0)
        return -1;
    snprintf(img_cache_path, sizeof img_cache_path, "%s", path);
    printf("[writer] image decoded: %s -> %dx%d pixels\n", path, img_w, img_h);
    return 0;
}

/* OF3: the page view (also the print preview: same layout tables).  One
 * page of the paginated document: header = title, footer = page number,
 * headings/markers from the layout, tables as bordered cells (header row
 * shaded), images as scaled boxes naming their file and size. */
static void paint_layout_page(void) {
    lay_geom = ao_page_geom_default();
    if (lay_rebuild() != AO_OK && lay.line_len == 0) return;
    int pg = ao_layout_page_of(&lay, &md, caret);
    if (pg < 0) pg = 0;
    char tmp[96];
    ao_layout_header(tmp, sizeof tmp, doc ? doc->title : "Untitled");
    ag_draw_text(wid, TEXT_X, PAGE_Y + 2, tmp, AG_DARK);
    if (pg < (int)lay.page_len && pg < (int)lay.page_cap) {
        int row = 0;
        for (uint32_t i = 0; i < lay.pages[pg].line_count; i++) {
            const ao_layout_line_t *ln = &lay.lines[lay.pages[pg].first_line + i];
            int y = EDIT_Y + row * LINE_H;
            if (y > PAGE_Y + PAGE_H - 40) break;
            row++;
            const ao_para_t *para = &md_paras[ln->para];
            char slice[256];
            uint32_t n = ln->len < sizeof slice - 1 ? ln->len : sizeof slice - 1;
            memcpy(slice, md_text + para->off + ln->start, n);
            slice[n] = 0;
            if (ln->kind == AO_PARA_TABLE) {
                ao_tcell_t cells[16];
                int header = !(ln->para > 0 &&
                    (md_paras[ln->para - 1].style & AO_PARA_KIND_MASK) == AO_PARA_TABLE);
                uint32_t nc = ao_table_parse_row(slice, n, cells, 16, header);
                if (nc) {
                    int cw = (PAGE_W - 20) / (int)nc;
                    for (uint32_t c = 0; c < nc; c++) {
                        int x = TEXT_X + (int)c * cw;
                        int wdt = (c + 1 == nc) ? (TEXT_X + PAGE_W - 20 - x) : cw;
                        if (cells[c].shade)
                            ag_fill_rect(wid, x, y, (uint32_t)wdt, LINE_H, AG_PANEL);
                        ag_draw_rect(wid, x, y, (uint32_t)wdt, LINE_H, AG_DARK);
                        char cell[64];
                        uint32_t cn = cells[c].len < sizeof cell - 1 ? cells[c].len : sizeof cell - 1;
                        /* a merge continuation draws nothing of its own */
                        const char *ct = cells[c].text;
                        if (cn && ct[0] == AO_TABLE_MERGE_MARK) { ct++; cn--; }
                        memcpy(cell, ct, cn); cell[cn] = 0;
                        ag_draw_text(wid, x + 3, y + 2, cell, AG_BLACK);
                    }
                }
            } else if (ln->kind == AO_PARA_IMAGE) {
                uint32_t iw = 0, ih = 0;
                int have = (lay_image_size(NULL, slice, n, &iw, &ih) == 0);
                if (have && load_image_pixels(slice) == 0) {
                    /* The real pixels, nearest-neighbour into the fit box --
                     * not a labelled placeholder. */
                    uint32_t dw = PAGE_W - 20, dh = 120;
                    ao_image_fit(&lay_geom, iw, ih, &dw, &dh);
                    if (dw > PAGE_W - 20) dw = PAGE_W - 20;
                    if (dh > 120) dh = 120;
                    for (uint32_t yy = 0; yy < dh; yy++)
                        for (uint32_t xx = 0; xx < dw; xx++)
                            img_scaled[yy * dw + xx] =
                                img_px[(yy * (uint32_t)img_h / dh) * (uint32_t)img_w +
                                       (xx * (uint32_t)img_w / dw)];
                    ag_blit(wid, TEXT_X, y, dw, dh, img_scaled, dw);
                    ag_draw_rect(wid, TEXT_X, y, dw, dh, AG_DARK);
                } else {
                    ag_fill_rect(wid, TEXT_X, y, PAGE_W - 20, LINE_H, AG_PANEL);
                    ag_draw_rect(wid, TEXT_X, y, PAGE_W - 20, LINE_H, AG_DARK);
                    char lbl[160];
                    if (have) snprintf(lbl, sizeof lbl, "[image %s: %ux%u, scaled to fit]", slice, iw, ih);
                    else      snprintf(lbl, sizeof lbl, "[image %s: not a readable PNG]", slice);
                    ag_draw_text(wid, TEXT_X + 4, y + 2, lbl, AG_DARK);
                }
            } else {
                if (ln->marker[0]) ag_draw_text(wid, TEXT_X, y, ln->marker, AG_ACCENT);
                int style = ((ln->kind >= AO_PARA_H1 && ln->kind <= AO_PARA_H3) ||
                             ao_model_style_at(&md, para->off + ln->start) & AO_STYLE_BOLD)
                                ? AO_STYLE_BOLD : AO_STYLE_NONE;
                if (font_body) band_run(font_body, ln->marker[0] ? 18 : 0, y,
                                        slice, n, style, AG_BLACK);
                else ag_draw_text(wid, TEXT_X + (ln->marker[0] ? 18 : 0), y, slice, AG_BLACK);
            }
        }
    }
    char foot[48];
    ao_layout_footer(foot, sizeof foot, (uint32_t)pg, lay.page_len);
    ag_draw_text(wid, TEXT_X, PAGE_Y + PAGE_H - 18, foot, AG_DARK);
    ao_canvas_mark(&canvas, PAGE_X, PAGE_Y, PAGE_W, PAGE_H);
}

static void repaint_full(void) {
    ao_canvas_clear(&canvas);
    ag_clear(wid, AG_PANEL);
    paint_page_furniture();
    if (view_page) {
        paint_layout_page();
        paint_status();
        publish();
        return;
    }
    paint_specimen();
    paint_text();
    paint_status();
    paint_caret();
    publish();
}

/* ---- headless selftest -------------------------------------------------- */

static int st_fails, st_total;
static void st_check(const char *name, int cond) {
    st_total++;
    if (cond) printf("AOFFICE PASS: %s\n", name);
    else { printf("AOFFICE FAIL: %s\n", name); st_fails++; }
}

static int selftest(void) {
    /* arena */
    ao_arena_t a;
    uint8_t mem[256];
    ao_arena_init(&a, mem, sizeof mem);
    void *p1 = ao_arena_alloc(&a, 10);
    void *p2 = ao_arena_alloc(&a, 10);
    st_check("arena: allocations are distinct and aligned",
             p1 && p2 && p1 != p2 && ((uintptr_t)p1 % AO_ARENA_ALIGN) == 0 &&
             ((uintptr_t)p2 % AO_ARENA_ALIGN) == 0);
    /* align8(10) == 16, so two gets consume 32 bytes. */
    st_check("arena: bump advances by aligned sizes", ao_arena_used(&a) == 32);
    st_check("arena: oversized request is refused, not fatal",
             ao_arena_alloc(&a, sizeof mem) == NULL);
    ao_arena_reset(&a);
    st_check("arena: reset returns the whole buffer",
             ao_arena_available(&a) == sizeof mem && a.resets == 1);
    st_check("arena: calloc zeroes", ao_arena_calloc(&a, 4, 4) != NULL &&
             ((unsigned char *)a.base)[0] == 0);

    /* document handle */
    ao_arena_init(&a, mem, sizeof mem);
    ao_doc_t *d = ao_doc_new(&a, "Untitled");
    st_check("doc: new sets title and rev", d && strcmp(d->title, "Untitled") == 0 && d->rev == 1);
    ao_doc_touch(d);
    st_check("doc: touch bumps rev and edits", d->rev == 2 && d->edits == 1);

    /* canvas: clipping, merging, saturation */
    ao_canvas_t c;
    ao_rect_t plan[AO_CANVAS_MAX_RECTS];
    ao_canvas_init(&c, 100, 50);
    st_check("canvas: empty plan is zero rects", ao_canvas_plan(&c, plan, 8) == 0);
    ao_canvas_mark(&c, -10, -10, 20, 20);          /* clips to a corner */
    int n = ao_canvas_plan(&c, plan, 8);
    st_check("canvas: out-of-box rect is clipped, not dropped",
             n == 1 && plan[0].x == 0 && plan[0].y == 0 && plan[0].w == 10 && plan[0].h == 10);
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 10, 10, 20, 10);
    ao_canvas_mark(&c, 30, 10, 20, 10);            /* touching: one rect */
    n = ao_canvas_plan(&c, plan, 8);
    st_check("canvas: touching rects merge into one",
             n == 1 && plan[0].x == 10 && plan[0].w == 40);
    ao_canvas_clear(&c);
    for (int i = 0; i < 6; i++) ao_canvas_mark(&c, i * 30, i * 12, 5, 5);
    n = ao_canvas_plan(&c, plan, 2);
    /* Six marks, but the 100x50 box clips the last two away; a budget of one
     * rect must still cover what is left, as a single union. */
    st_check("canvas: more rects than the budget yields one union",
             n == 1 && plan[0].x == 0 && plan[0].w >= 90);
    ao_canvas_clear(&c);
    ao_canvas_mark_all(&c);
    n = ao_canvas_plan(&c, plan, 8);
    st_check("canvas: mark_all plans the whole box",
             n == 1 && plan[0].w == 100 && plan[0].h == 50);
    ao_canvas_mark(&c, 1, 1, 2, 2);                /* saturated stays full */
    n = ao_canvas_plan(&c, plan, 8);
    st_check("canvas: saturation is sticky", n == 1 && plan[0].w == 100);

    /* edit model */
    ao_arena_init(&arena, arena_mem, sizeof arena_mem);
    doc = ao_doc_new(&arena, "Selftest");
    /* The edit verbs run through the document model: it must be initialised
     * here or ed_reset() dereferences a zeroed md (found by the OF0 gate
     * after the OF2 rewrite moved the edit path into libaodoc). */
    if (ao_model_init(&md, md_text, sizeof md_text, md_runs, MD_RUNS,
                      md_paras, MD_PARAS) != AO_OK ||
        ao_undo_init(&undo, undo_mem, sizeof undo_mem, redo_mem,
                     sizeof redo_mem) != AO_OK) {
        st_check("edit: the document model refused to initialise", 0);
        return 1;
    }
    ed_reset();
    for (const char *s = "hello"; *s; s++) ed_insert(*s);
    st_check("edit: typing builds a line", strcmp(lines[0], "hello") == 0 && cur_col == 5);
    ed_newline();
    for (const char *s = "world"; *s; s++) ed_insert(*s);
    st_check("edit: newline splits", n_lines == 2 && strcmp(lines[1], "world") == 0);
    ed_move(K_UP); ed_move(K_END); ed_insert('!');
    st_check("edit: navigation and insert", strcmp(lines[0], "hello!") == 0);
    ed_backspace();
    st_check("edit: backspace", strcmp(lines[0], "hello") == 0 && cur_col == 5);
    ed_move(K_DOWN); ed_move(K_HOME);
    ed_backspace();                                 /* joins line 1 into line 0 */
    st_check("edit: backspace at column 0 joins lines",
             n_lines == 1 && strcmp(lines[0], "helloworld") == 0);

    if (st_fails == 0) printf("AOFFICE DONE: %d/%d\n", st_total, st_total);
    else printf("AOFFICE DONE: %d/%d (%d FAILED)\n",
                st_total - st_fails, st_total, st_fails);
    return st_fails ? 1 : 0;
}

/* ---- budget benchmark --------------------------------------------------
 *
 * Three classes of repaint, measured in the guest, reported as RATIOS:
 *   full  — paint_page_furniture + all text + caret (what opening a window or
 *           a resize costs);
 *   line  — one text line (what a keystroke at the end of a paragraph costs);
 *   caret — two caret cells (what an arrow key costs).
 * The gate is caret < 5% and line < 20% of full, on whatever machine runs it.
 */

/* Iteration counts chosen against the guest clock: it ticks at 1 ms, so a
 * batch must span several ticks before its per-iteration average means
 * anything (40 caret repaints measured 0.0 us -- the whole loop was inside one
 * tick).  1 000 caret repaints is still well under a second. */
#define BENCH_FULL  10
#define BENCH_LINE  200
#define BENCH_CARET 1000

static int bench(void) {
    ensure_faces();
    ao_arena_init(&arena, arena_mem, sizeof arena_mem);
    doc = ao_doc_new(&arena, "Budget");
    if (ao_model_init(&md, md_text, sizeof md_text, md_runs, MD_RUNS,
                      md_paras, MD_PARAS) != AO_OK ||
        ao_undo_init(&undo, undo_mem, sizeof undo_mem, redo_mem,
                     sizeof redo_mem) != AO_OK) {
        printf("AOFFICE BUDGET: FAIL (the document model refused to initialise)\n");
        return 1;
    }
    ed_reset();
    for (int l = 0; l < 12; l++) {
        for (int c = 0; c < 40; c++) ed_insert((char)('a' + ((l + c) % 26)));
        if (l != 11) ed_newline();
    }
    scroll_to_caret();

    wid = ag_window_create(60, 40, WIN_W, WIN_H, "Office - budget", AG_WIN_DEFAULT);
    if (wid < 0) {
        printf("AOFFICE BUDGET: FAIL (no window: ag_window_create returned %d)\n", wid);
        return 1;
    }
    ag_window_show(wid);
    ao_canvas_init(&canvas, WIN_W, WIN_H);

    /* Warm-up: the first frame pays for window allocation and font setup. */
    repaint_full();
    long full_px = last_pixels;

    long t0 = now_us();
    for (int i = 0; i < BENCH_FULL; i++) repaint_full();
    long full_ns = (now_us() - t0) * 1000L / BENCH_FULL;
    full_px = last_pixels;

    /* A keystroke: repaint the line it landed in, then the caret. */
    long line_px = 0;
    t0 = now_us();
    for (int i = 0; i < BENCH_LINE; i++) {
        ao_canvas_clear(&canvas);
        paint_line(cur_line);
        paint_caret();
        publish();
        line_px = last_pixels;
    }
    long line_ns = (now_us() - t0) * 1000L / BENCH_LINE;

    /* An arrow key: erase the old caret, draw the new one, nothing else. */
    long caret_px = 0;
    t0 = now_us();
    for (int i = 0; i < BENCH_CARET; i++) {
        ao_canvas_clear(&canvas);
        paint_caret();                     /* the cell being left */
        cur_col = (cur_col + 1) % 10;
        paint_caret();                     /* the cell being entered */
        publish();
        caret_px = last_pixels;
    }
    long caret_ns = (now_us() - t0) * 1000L / BENCH_CARET;

    /* Microseconds with a decimal: a keystroke repaint is a handful of
     * syscalls and lands well under a millisecond, so integer microseconds
     * printed "0" and hid the number the gate is about. */
    printf("[aoffice] budget: full=%.1f us line=%.1f us caret=%.1f us "
           "(n=%d/%d/%d)\n", (double)full_ns / 1000.0, (double)line_ns / 1000.0,
           (double)caret_ns / 1000.0, BENCH_FULL, BENCH_LINE, BENCH_CARET);
    printf("[aoffice] budget: dirty pixels full=%ld line=%ld caret=%ld "
           "(window %dx%d = %ld px)\n", full_px, line_px, caret_px,
           WIN_W, WIN_H, (long)WIN_W * WIN_H);

    /* Two gates, because they say different things.
     *
     * TIME is what the application spends asking the compositor to repaint;
     * it is machine-dependent, so it is compared only against itself.
     * PIXELS is what the compositor is asked to composite; it is exact, and it
     * is the number that decides whether the design scales to a 1920x1080
     * screen (a caret must not touch a thousandth of the window whatever the
     * machine is).  On this tree the caret repaint is 28 px against a 281 600
     * px window, i.e. 0.01 %. */
    int line_time_pct  = full_ns > 0 ? (int)(line_ns  * 100 / full_ns) : 100;
    int caret_time_pct = full_ns > 0 ? (int)(caret_ns * 100 / full_ns) : 100;
    long window_px = (long)WIN_W * WIN_H;
    int caret_px_pm  = window_px > 0 ? (int)(caret_px * 10000 / window_px) : 10000;
    int line_px_pm   = window_px > 0 ? (int)(line_px  * 10000 / window_px) : 10000;

    int ok_time   = (caret_time_pct < 5) && (line_time_pct < 20);
    int ok_pixels = (caret_px_pm <= 10) && (line_px_pm <= 500);   /* 0.1 % / 5 % */

    printf("AOFFICE BUDGET: %s (time: caret %d%% of full, line %d%% of full, "
           "limits 5%%/20%%; pixels: caret %.2f%% of the window, line %.2f%%, "
           "limits 0.1%%/5%%)\n",
           (ok_time && ok_pixels) ? "PASS" : "FAIL",
           caret_time_pct, line_time_pct,
           (double)caret_px_pm / 100.0, (double)line_px_pm / 100.0);

    ag_window_destroy(wid);
    wid = -1;
    return (ok_time && ok_pixels) ? 0 : 1;
}

/* ---- interactive window ------------------------------------------------- */

/* The prompt strip eats keys while it is open: Enter runs it, ESC drops it. */
static void prompt_key(uint32_t key, uint32_t cp) {
    if (key == 0x1B) { prompt = 0; prompt_len = 0; prompt_buf[0] = '\0'; return; }
    if (key == '\n' || key == '\r') {
        prompt_buf[prompt_len] = '\0';
        if (prompt == 1) {
            snprintf(last_find, sizeof last_find, "%s", prompt_buf);
            if (!ed_find(last_find, 1)) printf("[writer] \"%s\": no match\n", last_find);
        } else if (prompt == 2) {
            ed_replace_sel(prompt_buf);
        } else if (prompt == 4) {
            ed_insert_image(prompt_buf);
        } else if (prompt == 3) {
            if (!last_find[0]) {
                printf("[writer] replace-all: run Find first (Ctrl+F)\n");
            } else {
                ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
                int n = ao_model_replace_all(&md, last_find, prompt_buf);
                if (n < 0) printf("[writer] replace-all: refused (rc=%d)\n", n);
                else printf("[writer] replace-all \"%s\" -> \"%s\": %d replacement(s)\n",
                            last_find, prompt_buf, n);
                if (n > 0) md_dirty();
                if (caret > ao_model_text_len(&md)) caret = ao_model_text_len(&md);
                sel_clear();
                md_sync();
            }
        }
        prompt = 0;
        prompt_len = 0;
        prompt_buf[0] = '\0';
        return;
    }
    if (key == '\b') {
        if (prompt_len > 0) {
            prompt_len = (int)ao_utf8_prev(prompt_buf, (size_t)prompt_len);
            prompt_buf[prompt_len] = '\0';
        }
        return;
    }
    char enc[4];
    int n = 0;
    if (cp >= 0x20 && cp != 0x7F) n = ao_utf8_encode(cp, enc);
    else if (key >= 0x20 && key < 0x7F) { enc[0] = (char)key; n = 1; }
    if (n > 0 && prompt_len + n < (int)sizeof prompt_buf) {
        memcpy(prompt_buf + prompt_len, enc, (size_t)n);
        prompt_len += n;
        prompt_buf[prompt_len] = '\0';
    }
}

static void on_edit_key(uint32_t key, uint32_t cp, uint8_t mods) {
    int old_line = cur_line;
    uint32_t old_caret = caret;

    if (dialog) {
        dlg_key(key, cp);
        ao_canvas_clear(&canvas);
        if (dialog) paint_dialog(); else repaint_full();
        paint_status();
        publish();
        return;
    }

    if (prompt) {
        prompt_key(key, cp);
        paint_line(cur_line);
        paint_caret();
        paint_status();
        publish();
        return;
    }

    int shift = (mods & M_SHIFT) != 0;
    int ctrl  = (mods & M_CTRL)  != 0;

    if (ctrl && key == K_LEFT) {
        ed_move_to(ao_model_word_left(&md, caret), shift);
    } else if (ctrl && key == K_RIGHT) {
        ed_move_to(ao_model_word_right(&md, caret), shift);
    } else if (ctrl && key == K_HOME) {
        ed_move_to(0, shift);
    } else if (ctrl && key == K_END) {
        ed_move_to(ao_model_text_len(&md), shift);
    } else if (ctrl) {
        switch (key) {
            case '1': ed_para_style(AO_PARA_NORMAL); break;
            case '2': ed_para_style(AO_PARA_H1); break;
            case '3': ed_para_style(AO_PARA_H2); break;
            case '4': ed_para_style(AO_PARA_H3); break;
            case '5': ed_para_style(AO_PARA_QUOTE); break;
            case '6': ed_para_style(AO_PARA_BULLET); break;
            case '7': ed_para_style(AO_PARA_NUMBER); break;
            case '8': ed_para_style((uint8_t)(ao_model_para_style_at(&md, caret) ^ AO_PARA_PAGEBREAK)); break;
            case 'a': case 'A': ed_select_all(); break;
            case 'b': case 'B': ed_style(AO_STYLE_BOLD); break;
            case 'i': case 'I': ed_style(AO_STYLE_ITALIC); break;
            case 'u': case 'U': ed_style(AO_STYLE_UNDERLINE); break;
            case 'z': case 'Z': ed_undo(); break;
            case 'y': case 'Y': ed_redo(); break;
            case '=': case '+': if (shift) ed_table_col(1); break;
            case '-': case '_': if (shift) ed_table_col(0); break;
            case 'm': case 'M': if (shift) ed_table_merge(); break;
            case 'c': case 'C': ed_copy(); break;
            case 'x': case 'X': ed_cut(); break;
            case 'v': case 'V': ed_paste(); break;
            case 's': case 'S': if (shift) dlg_open(2); else ed_save(); break;
            case 'o': case 'O': dlg_open(1); break;
            case 'f': case 'F': prompt = 1; prompt_len = 0; prompt_buf[0] = '\0'; break;
            case 'h': case 'H': prompt = shift ? 3 : 2; prompt_len = 0; prompt_buf[0] = '\0'; break;
            case 'l': case 'L': ed_align(AO_ALIGN_LEFT); break;
            case 'e': case 'E': ed_align(AO_ALIGN_CENTER); break;
            case 'r': case 'R': ed_align(AO_ALIGN_RIGHT); break;
            case 'j': case 'J': ed_align(AO_ALIGN_JUSTIFY); break;
            default: break;
        }
    } else if (cp >= 0x20 && cp != 0x7F) {
        ed_insert_cp(cp);
    } else if (key >= 0x20 && key < 0x7F) {
        ed_insert((char)key);
    } else if (key == '\n' || key == '\r') {
        ed_newline();
    } else if (key == '\b') {
        ed_backspace();
    } else if (key == K_DEL) {
        ed_delete_forward();
    } else if (key == K_LEFT || key == K_RIGHT || key == K_UP || key == K_DOWN ||
               key == K_HOME || key == K_END || key == K_PGUP || key == K_PGDN) {
        ed_move_ext(key, shift);
    } else if (key == K_F6) {
        ed_insert_table(4, 4);
    } else if (key == K_F7) {
        prompt = 4; prompt_len = 0; prompt_buf[0] = '\0';
    } else if (key == K_F8) {
        view_page = !view_page;
        printf("[writer] view: %s\n", view_page ? "page (print preview)" : "draft");
        repaint_full();
        return;
    } else if (key == K_F9) {
        ed_table_row(0);
    } else if (key == K_F10) {
        ed_table_row(1);
    } else if (key == K_F5) {
        if (last_find[0] && ed_find(last_find, 1)) { /* find next */ }
    }

    scroll_to_caret();
    ao_canvas_clear(&canvas);

    /* What has to be repainted: the caret's line, the line the caret left, and
     * -- for a selection, which can reach across the page -- every visible
     * line when the selection changed.  Never the whole window. */
    paint_line(old_line);
    paint_line(cur_line);
    if (sel_on || old_caret != caret) {
        int first = top_line, last = top_line + VISIBLE - 1;
        if (last >= n_lines) last = n_lines - 1;
        for (int i = first; i <= last; i++) if (i != old_line && i != cur_line) paint_line(i);
    }
    paint_caret();
    if (dialog) paint_dialog();
    paint_status();
    publish();
}

/* ---- OF2: the writer gate, headless -------------------------------------
 *
 * What the integration case can assert with no window and no VNC: a document
 * typed through the same functions a keystroke calls, saved through the same
 * file path the Ctrl+S path uses, read back by a second model, and compared
 * byte for byte -- plus undo, which is the one feature that cannot be checked
 * by looking at the text.
 */
static int writer_test(void) {
    int fails = 0;
    printf("[writer] text buffer %u bytes, %u runs, %u paragraphs, undo %u+%u bytes\n",
           (unsigned)MD_TEXT_BYTES, (unsigned)MD_RUNS, (unsigned)MD_PARAS,
           (unsigned)AO_UNDO_DEFAULT_BYTES, (unsigned)AO_UNDO_DEFAULT_BYTES);

    ao_arena_init(&arena, arena_mem, sizeof arena_mem);
    doc = ao_doc_new(&arena, "Writer gate");
    if (ao_model_init(&md, md_text, sizeof md_text, md_runs, MD_RUNS, md_paras, MD_PARAS) != AO_OK ||
        ao_undo_init(&undo, undo_mem, sizeof undo_mem, redo_mem, sizeof redo_mem) != AO_OK) {
        printf("WRITER GATE: FAIL (model or history refused to initialise)\n");
        return 1;
    }
    ed_reset();

    /* Type through the keystroke path, Cyrillic included. */
    ed_insert_utf8("Первый абзац: 2026 — проверка. ");
    for (const char *q = "typed"; *q; q++) ed_insert(*q);
    ed_newline();
    ed_insert_utf8("Второй абзац с кириллицей ёЁ");

    uint32_t len = ao_model_text_len(&md);
    printf("[writer] typed %u bytes, %u paragraphs, %u runs\n",
           len, ao_model_para_count(&md), ao_model_run_count(&md));
    if (len == 0 || ao_model_para_count(&md) != 2) {
        printf("WRITER GATE: FAIL (typing did not build two paragraphs)\n");
        return 1;
    }

    /* Select all, bold it: the shortcut path, not a bespoke call. */
    ed_select_all();
    ed_style(AO_STYLE_BOLD);
    if (ao_model_style_at(&md, 0) != AO_STYLE_BOLD) {
        printf("WRITER GATE: FAIL (select-all + bold did not style the text)\n");
        fails++;
    }
    printf("[writer] select-all + bold: %u bytes bold, selection %d\n",
           ao_model_text_len(&md), sel_len());

    /* Undo the bold, redo it: the history must round trip the document, and
     * "round trip" here means byte identical when serialised, not "looks
     * carefully unbolded". */
    static uint8_t snap_a[8192], snap_b[8192];
    size_t na = 0, nb = 0;
    ao_model_serialize(&md, snap_a, sizeof snap_a, &na);
    ed_undo();
    if (ao_model_style_at(&md, 0) != AO_STYLE_OFF) {
        printf("WRITER GATE: FAIL (undo did not clear the bold)\n"); fails++;
    }
    ed_redo();
    ao_model_serialize(&md, snap_b, sizeof snap_b, &nb);
    if (na != nb || memcmp(snap_a, snap_b, na) != 0) {
        printf("WRITER GATE: FAIL (redo did not restore the document byte for byte)\n"); fails++;
    }
    printf("[writer] undo/redo: bold removed and restored, %u-byte snapshot, "
           "%u undo / %u redo steps\n", (unsigned)na,
           ao_undo_depth(&undo), ao_undo_redo_depth(&undo));

    /* Save, read back through a second model, compare the FILE bytes. */
    if (ed_save_as(WRITER_TEST_PATH) != 0) {
        printf("WRITER GATE: FAIL (save)\n");
        return 1;
    }
    int fd = open(WRITER_TEST_PATH, O_RDONLY);
    if (fd < 0) { printf("WRITER GATE: FAIL (the shell cannot see the saved file)\n"); return 1; }
    ssize_t got = read(fd, io_buf, sizeof io_buf - 1);
    close(fd);
    if (got <= 0) { printf("WRITER GATE: FAIL (saved file is empty)\n"); return 1; }

    static char t2[MD_TEXT_BYTES];
    static ao_run_t r2[MD_RUNS];
    static ao_para_t p2[MD_PARAS];
    ao_model_t back;
    ao_model_init(&back, t2, sizeof t2, r2, MD_RUNS, p2, MD_PARAS);
    int rc = ao_io_import(&back, io_buf, (size_t)got);
    if (rc != AO_OK) { printf("WRITER GATE: FAIL (the reader refused our own file, rc=%d)\n", rc); fails++; }
    if (ao_model_text_len(&back) != ao_model_text_len(&md) ||
        memcmp(t2, md_text, ao_model_text_len(&md)) != 0) {
        printf("WRITER GATE: FAIL (text did not survive the round trip)\n"); fails++;
    }
    if (ao_model_style_at(&back, 0) != AO_STYLE_BOLD) {
        printf("WRITER GATE: FAIL (bold did not survive the round trip)\n"); fails++;
    }
    size_t n2 = 0;
    if (ao_io_export(&back, io_buf2, sizeof io_buf2, &n2) != AO_OK || n2 != (size_t)got ||
        memcmp(io_buf, io_buf2, (size_t)got) != 0) {
        printf("WRITER GATE: FAIL (save -> load -> save is not byte identical)\n"); fails++;
    }

    /* Reopen it through the file system and the reader -- the same call Ctrl+O
     * makes, proving that a second instance sees what the first one wrote. */
    if (ed_load_from(WRITER_TEST_PATH) != 0) {
        printf("WRITER GATE: FAIL (reopen)\n"); fails++;
    } else if (ao_model_text_len(&md) != ao_model_text_len(&back) ||
               memcmp(md_text, t2, ao_model_text_len(&md)) != 0 ||
               ao_model_style_at(&md, 0) != AO_STYLE_BOLD) {
        printf("WRITER GATE: FAIL (the reopened document is not the one saved)\n"); fails++;
    }

    /* ---- OF2 closure, headless: word nav, mouse mapping, replace-all, dialog */
    {
        ed_reset();
        ed_insert_utf8("one two one");
        uint32_t wr = ao_model_word_right(&md, 0);
        ed_move_to(wr, 0);
        if (wr != 3 || caret != 3) { printf("WRITER GATE: FAIL (word-right)\n"); fails++; }
        ensure_faces();
        if (font_body) {
            uint32_t at0 = caret_from_xy(TEXT_X + 1, EDIT_Y);
            uint32_t atE = caret_from_xy(TEXT_X + PAGE_W, EDIT_Y);
            if (at0 != line_off[cur_line] || atE <= at0) {
                printf("WRITER GATE: FAIL (mouse caret mapping %u/%u)\n", at0, atE);
                fails++;
            }
        }
        ao_undo_push(&undo, &md, AO_UNDO_BARRIER);
        int nrep = ao_model_replace_all(&md, "one", "1");
        if (nrep != 2 || ao_model_text_len(&md) != 7 ||
            memcmp(md_text, "1 two 1", 7) != 0) {
            printf("WRITER GATE: FAIL (replace-all made %d)\n", nrep); fails++;
        }
        dlg_open(2);
        int dlg_files = dlg_count;
        if (dlg_files < 1 || !dlg_key(0x1B, 0) || dialog != 0) {
            printf("WRITER GATE: FAIL (file dialog state)\n"); fails++;
        }
        printf("[writer] of2 closure: word-nav ok, mouse-map ok, replace-all %d, dialog %d file(s)\n",
               nrep, dlg_files);
    }

    printf("[writer] round trip: %d bytes of XML -> %u bytes of text, %u paragraphs, "
           "%u runs, bold kept, undo %u steps\n",
           (int)got, ao_model_text_len(&back), ao_model_para_count(&back),
           ao_model_run_count(&back), ao_undo_depth(&undo));
    printf("WRITER GATE: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

/* ---- OF3: the layout gate, headless -------------------------------------
 * Builds the document the plan names (heading, bullets, numbers, a 4x4
 * table, an image), lays it out, saves it, reopens it, lays it out again:
 * the two layouts must agree line for line and page for page.  The image
 * size comes from a stub callback (1600x1200): the gate grades layout, and
 * ao_png_size itself is host-tested against real IHDR bytes. */
static int lt_img(void *ctx, const char *path, uint32_t len,
                  uint32_t *w, uint32_t *h) {
    (void)ctx; (void)path; (void)len; *w = 1600; *h = 1200; return 0;
}

static void lt_add(const char *text, uint8_t style) {
    caret = ao_model_text_len(&md);
    if (caret) ed_insert_utf8("\n");
    uint32_t at = ao_model_text_len(&md);
    ed_insert_utf8(text);
    ao_model_set_para_style(&md, at, style);
    md_sync();
}

static int layout_test(void) {
    int fails = 0;
    ao_arena_init(&arena, arena_mem, sizeof arena_mem);
    doc = ao_doc_new(&arena, "Layout gate");
    if (ao_model_init(&md, md_text, sizeof md_text, md_runs, MD_RUNS,
                      md_paras, MD_PARAS) != AO_OK ||
        ao_undo_init(&undo, undo_mem, sizeof undo_mem, redo_mem, sizeof redo_mem) != AO_OK) {
        printf("LAYOUT GATE: FAIL (model init)\n");
        return 1;
    }
    lay_geom = ao_page_geom_default();
    ed_reset();

    lt_add("Quarterly report", AO_PARA_H1);
    lt_add("Intro text that should wrap at the page margins when it grows long enough to matter.", AO_PARA_NORMAL);
    lt_add("first point", AO_PARA_BULLET);
    lt_add("second point", AO_PARA_BULLET);
    lt_add("step one", AO_PARA_NUMBER);
    lt_add("step two", AO_PARA_NUMBER);
    for (int r = 0; r < 4; r++) {
        char row[64];
        snprintf(row, sizeof row, "R%dA\tR%dB\tR%dC\tR%dD", r, r, r, r);
        lt_add(row, AO_PARA_TABLE);
    }
    lt_add("/lib/chart.png", AO_PARA_IMAGE);

    ao_layout_t l1;
    ao_layout_init(&l1, lay_lines, 2048, lay_pages, 128);
    int rc = ao_layout_build(&l1, &md, &lay_geom, lay_measure, NULL, lt_img, NULL);
    if (rc != AO_OK) { printf("LAYOUT GATE: FAIL (build rc=%d)\n", rc); return 1; }
    int have_h1 = 0, have_table = 0, have_image = 0, have_num = 0;
    for (uint32_t i = 0; i < l1.line_len; i++) {
        if (l1.lines[i].kind == AO_PARA_H1) have_h1 = 1;
        if (l1.lines[i].kind == AO_PARA_TABLE) have_table++;
        if (l1.lines[i].kind == AO_PARA_IMAGE) have_image = 1;
        if (l1.lines[i].kind == AO_PARA_NUMBER && l1.lines[i].marker[0] == '2') have_num = 1;
    }
    printf("[layout] built: %u lines on %u page(s); heading %d, table rows %d, image %d, numbering %d\n",
           l1.line_len, l1.page_len, have_h1, have_table, have_image, have_num);
    if (!have_h1 || have_table != 4 || !have_image || !have_num) fails++;

    if (ed_save_as("/documents/layout-gate.aod") != 0) {
        printf("LAYOUT GATE: FAIL (save)\n");
        return 1;
    }
    uint32_t lines1 = l1.line_len, pages1 = l1.page_len;
    if (ed_load_from("/documents/layout-gate.aod") != 0) {
        printf("LAYOUT GATE: FAIL (reopen)\n");
        return 1;
    }
    ao_layout_t l2;
    ao_layout_init(&l2, lay_lines, 2048, lay_pages, 128);
    rc = ao_layout_build(&l2, &md, &lay_geom, lay_measure, NULL, lt_img, NULL);
    if (rc != AO_OK || l2.line_len != lines1 || l2.page_len != pages1) {
        printf("LAYOUT GATE: FAIL (layout did not reproduce: %u/%u lines, %u/%u pages)\n",
               l2.line_len, lines1, l2.page_len, pages1);
        fails++;
    }
    /* The styles themselves must survive, not only the line count. */
    int styles_ok = 1;
    for (uint32_t i = 0; i < l2.line_len && i < l1.line_len; i++)
        if (l2.lines[i].kind != l1.lines[i].kind) styles_ok = 0;
    if (!styles_ok) { printf("LAYOUT GATE: FAIL (paragraph styles changed)\n"); fails++; }

    /* The pixel decode behind the page view, asserted headlessly: the
     * shipped chart's frame corner and first bar must come back with the
     * colours the generator put there. */
    if (load_image_pixels("/lib/chart.png") != 0 || !img_px) {
        printf("LAYOUT GATE: FAIL (chart.png did not decode)\n");
        fails++;
    } else {
        uint32_t corner = img_px[0];
        uint32_t bar = img_px[(size_t)(img_h / 2) * (size_t)img_w + 16];
        printf("[layout] chart.png decoded %dx%d: corner 0x%06x, bar 0x%06x\n",
               img_w, img_h, corner & 0xFFFFFFu, bar & 0xFFFFFFu);
        if (img_w != 320 || img_h != 200 || (corner & 0xFFFFFFu) != 0x282830u ||
            (bar & 0xFFFFFFu) != 0xC03020u) {
            printf("LAYOUT GATE: FAIL (decoded pixels are wrong)\n");
            fails++;
        }
    }

    printf("[layout] round trip: %u lines, %u page(s), styles intact\n",
           l2.line_len, l2.page_len);
    printf("LAYOUT GATE: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

/* ---- OF1: the font gate, headless ---------------------------------------
 *
 * Prints what the faces actually carry and what they cost, so the integration
 * case asserts numbers rather than the absence of a crash.  This is also the
 * receipt a user gets from `aoffice --font-test` on their own machine after
 * dropping a BDF into /lib/fonts.
 */
static int font_test(void) {
    int fails = 0;
    int rc_s = load_face(FONT_SANS, &font_sans, &font_arena_sans, font_mem_sans);
    int rc_m = load_face(FONT_MONO, &font_mono, &font_arena_mono, font_mem_mono);

    printf("[aoffice] face %s: %s\n", FONT_SANS,
           rc_s == 0 ? "loaded" : "NOT LOADED");
    printf("[aoffice] face %s: %s\n", FONT_MONO,
           rc_m == 0 ? "loaded" : "NOT LOADED");
    if (rc_s != 0 || rc_m != 0) {
        printf("AOFFICE FONT: FAIL (a face the suite promises is missing)\n");
        return 1;
    }

    struct { const char *path; ao_font_t *f; } faces[] = {
        { FONT_SANS, &font_sans }, { FONT_MONO, &font_mono },
    };
    for (int i = 0; i < 2; i++) {
        ao_font_t *f = faces[i].f;
        int cyr = ao_font_covers(f, 0x0410, 0x044F);
        int extra = ao_font_covers(f, 0x2013, 0x2014) +
                    ao_font_covers(f, 0x0451, 0x0451) +
                    ao_font_covers(f, 0x2116, 0x2116);
        printf("[aoffice] %s: %u glyphs, cell %ux%u, ascent %d, descent %d, "
               "cyrillic %d/64, typographic %d/4, %u bytes of arena\n",
               faces[i].path, (unsigned)f->n, f->cell_w, f->cell_h,
               f->ascent, f->descent, cyr, extra,
               (unsigned)ao_font_used_bytes(f));
        if (cyr != 64) fails++;
        if (f->n <= 0) fails++;
        if (f->cell_w == 0 || f->cell_h == 0) fails++;
    }

    /* The phrase from the plan, measured in both faces and drawn in three
     * styles.  A count of zero for bold would mean the synthesis never ran. */
    static const char sample[] = "Привет, мир — 2026";
    size_t slen = strlen(sample);
    int ws = ao_text_width(&font_sans, sample, slen);
    int wm = ao_text_width(&font_mono, sample, slen);
    printf("[aoffice] measure: \"%s\" = %d px (sans), %d px (mono)\n",
           sample, ws, wm);
    if (ws <= 0 || wm <= 0) fails++;
    if (ws >= wm) fails++;     /* the proportional face must be narrower */

    static uint32_t canvas_px[512 * 32];
    struct { const char *name; int style; } styles[] = {
        { "regular", AO_STYLE_NONE }, { "bold", AO_STYLE_BOLD },
        { "italic", AO_STYLE_ITALIC },
    };
    int painted[3];
    int leftmost[3];       /* leftmost ink in the top rows, per style */
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 512 * 32; k++) canvas_px[k] = 0;
        painted[i] = ao_text_draw(&font_sans, sample, slen, styles[i].style,
                                  canvas_px, 512, 512, 32, 6, 24, 0xFFFFFFFF, NULL);
        /* Top of the ink, then the leftmost pixel in its first four rows: the
         * sheared rows are the ones far from the baseline, so this is where an
         * italic must differ from a regular run. */
        int ymin = -1;
        for (int y = 0; y < 32 && ymin < 0; y++)
            for (int x = 0; x < 512; x++)
                if (canvas_px[y * 512 + x]) { ymin = y; break; }
        leftmost[i] = -1;
        for (int y = ymin; y >= 0 && y < ymin + 4; y++)
            for (int x = 0; x < 512; x++)
                if (canvas_px[y * 512 + x]) {
                    if (leftmost[i] < 0 || x < leftmost[i]) leftmost[i] = x;
                    break;
                }
        printf("[aoffice] style %-7s: %d pixels for the same %d glyphs\n",
               styles[i].name, painted[i], (int)ao_utf8_count(sample, slen));
        if (painted[i] <= 0) fails++;
    }
    /* Bold must be thicker: synthesis that produces the same bitmap is
     * synthesis that did not run. */
    if (painted[1] <= painted[0]) fails++;
    /* Italic must LEAN, and the pixel count cannot show that (a shifted row
     * has the same number of pixels), so check the geometry: the top of the
     * glyphs moves right. */
    printf("[aoffice] style italic : top row starts at x=%d vs %d regular\n",
           leftmost[2], leftmost[0]);
    if (leftmost[2] <= leftmost[0]) fails++;
    /* Per-glyph receipts.  A glyph that paints nothing is a font bug; a glyph
     * that paints nothing only IN THE GUEST is an environment bug, and the two
     * are indistinguishable from a screenshot -- which is exactly how a
     * "rendering" problem cost an afternoon once already. */
    for (size_t i = 0, k = 0; i < slen; i += (size_t)k) {
        uint32_t cp = 0;
        k = ao_utf8_decode(sample + i, slen - i, &cp);
        if (k <= 0) { cp = (uint32_t)'?'; k = 1; }
        const ao_glyph_t *g = ao_font_glyph(&font_sans, cp);
        int npx = ao_glyph_draw(&font_sans, cp, AO_STYLE_NONE, canvas_px, 512,
                                512, 32, 4, 24, 0xFFFFFFFF);
        printf("[aoffice] glyph U+%04X: w=%u h=%u adv=%u yoff=%d paints=%d\n",
               cp, g->w, g->h, g->adv, g->yoff, npx);
        if (npx <= 0 && cp != 0x20) fails++;
    }

    printf("AOFFICE FONT: %s (%d face(s), %d failure(s))\n",
           fails ? "FAIL" : "PASS", 2, fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) return bench();
    if (argc > 1 && strcmp(argv[1], "--font-test") == 0) return font_test();
    if (argc > 1 && strcmp(argv[1], "--writer-test") == 0) return writer_test();
    if (argc > 1 && strcmp(argv[1], "--layout-test") == 0) return layout_test();

    /* OF1: load the faces before anything is painted; the window falls back to
     * the console face (and says so) when they are absent. */
    ensure_faces();
    printf("[aoffice] faces: sans=%s mono=%s body=%s\n",
           font_sans.n ? "yes" : "no", font_mono.n ? "yes" : "no",
           font_body ? "sans 11" : "console");

    ao_arena_init(&arena, arena_mem, sizeof arena_mem);
    doc = ao_doc_new(&arena, "Untitled - Documents");
    /* OF2: the document lives in libaodoc, not in this file.  Initialise the
     * model and the history before the first keystroke can reach them. */
    if (ao_model_init(&md, md_text, sizeof md_text, md_runs, MD_RUNS,
                      md_paras, MD_PARAS) != AO_OK ||
        ao_undo_init(&undo, undo_mem, sizeof undo_mem, redo_mem, sizeof redo_mem) != AO_OK) {
        printf("[aoffice] the document model refused to initialise\n");
        return 1;
    }
    ed_reset();
    snprintf(doc->title, sizeof doc->title, "Untitled - Writer");
    /* OF1 content: mixed script, a real em dash, and the typographic characters
     * a Russian document needs.  Typed through ed_insert_utf8, i.e. the same
     * path a keystroke takes. */
    ed_insert_utf8("Привет, мир! Это документ — 2026 год.");
    ed_newline();
    ed_insert_utf8("Кириллица: съешь ещё этих мягких французских булок.");
    ed_move(K_HOME);
    scroll_to_caret();

    wid = ag_window_create(60, 40, WIN_W, WIN_H, "AuraLite Office", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    /* Ask for the keyboard.  A window opened from the serial shell's `run` does
     * not necessarily become the WM's focus, and an editor that draws a caret
     * but receives no keystrokes looks broken rather than unfocused. */
    ag_window_focus(wid);
    ao_canvas_init(&canvas, WIN_W, WIN_H);
    repaint_full();

    for (;;) {
        ag_event_t e;
        if (!ag_poll_event(wid, &e)) {
            for (volatile int i = 0; i < 200000; i++) {}
            continue;
        }
        if (e.type == AG_EVT_CLOSE_REQ) break;
        if (e.type == AG_EVT_MOUSE_DOWN) {
            focused = (e.x >= PAGE_X && e.x < PAGE_X + (int32_t)PAGE_W &&
                       e.y >= PAGE_Y && e.y < PAGE_Y + (int32_t)PAGE_H);
            /* OF2 closure: click places the caret, the drag that follows
             * selects.  Coordinates are content-area pixels, the same space
             * paint_line() draws in. */
            if (focused && e.y >= EDIT_Y && e.y < EDIT_Y + VISIBLE * LINE_H &&
                e.x >= TEXT_X && !dialog) {
                int old_line = cur_line;
                uint32_t at = caret_from_xy(e.x, e.y);
                sel_anchor = at;
                caret = at;
                sel_on = 0;
                drag_select = 1;
                ao_undo_break(&undo);
                md_sync();
                scroll_to_caret();
                ao_canvas_clear(&canvas);
                paint_line(old_line);
                paint_line(cur_line);
                paint_caret();
                paint_status();
                publish();
            }
            continue;
        }
        if (e.type == AG_EVT_MOUSE_MOVE && drag_select && (e.buttons & 1)) {
            int old_line = cur_line;
            caret = caret_from_xy(e.x, e.y);
            sel_on = (caret != sel_anchor);
            md_sync();
            scroll_to_caret();
            ao_canvas_clear(&canvas);
            for (int i = top_line; i < n_lines && i < top_line + VISIBLE; i++)
                paint_line(i);
            (void)old_line;
            paint_caret();
            paint_status();
            publish();
            continue;
        }
        if (e.type == AG_EVT_MOUSE_UP) { drag_select = 0; continue; }
        if (e.type == AG_EVT_MOUSE_DBLCLICK && focused && !dialog &&
            e.y >= EDIT_Y && e.y < EDIT_Y + VISIBLE * LINE_H && e.x >= TEXT_X) {
            uint32_t at = caret_from_xy(e.x, e.y);
            sel_set(ao_model_word_left(&md, ao_model_next_cp(&md, at)),
                    ao_model_word_right(&md, at));
            ao_canvas_clear(&canvas);
            for (int i = top_line; i < n_lines && i < top_line + VISIBLE; i++)
                paint_line(i);
            paint_caret();
            paint_status();
            publish();
            continue;
        }
        if (e.type != AG_EVT_KEY_DOWN) continue;
        /* ESC closes the window -- unless a prompt is open, where it means
         * "forget this search", which is what every editor does with it. */
        if (e.key == 0x1B && !prompt) break;
        /* Alt belongs to the window menu (the compositor's own shortcut), so
         * the page ignores it -- but Ctrl is OURS: Ctrl+A/B/I/S/Z/Y and the
         * rest of a word processor's alphabet live behind it. */
        if (!focused || (e.mods & M_ALT)) continue;
        /* `data` carries the Unicode codepoint (OFFICE_PLAN OF1); it is 0 for
         * keys that produce none, and the byte in `key` is then the answer. */
        on_edit_key(e.key, e.data, e.mods);
    }
    ag_window_destroy(wid);
    return 0;
}
