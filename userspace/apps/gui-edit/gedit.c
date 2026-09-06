/* gedit — GUI text editor (RESIDUE2 T7: a real multi-line editor).
 *
 * The first version was a single 256-char ag textbox with Load/Save: you
 * could not even type a newline.  This version separates the EDITOR
 * ENGINE (a line-array buffer with insert/split/join/navigation, all
 * plain functions) from the window chrome:
 *
 *   - engine: ed_insert_char / ed_newline / ed_backspace / ed_delete /
 *     ed_move (LEFT/RIGHT/UP/DOWN/HOME/END), ed_load / ed_save;
 *   - GUI: custom loop like gterm's (the ag_view_run repaint order
 *     erases custom content), content area with line numbers, caret,
 *     auto-scrolling viewport, "Ln/Col + dirty" status; clicking the
 *     content area focuses the editor, clicking the path box focuses it;
 *   - `gedit --selftest` drives the ENGINE over a scripted edit session
 *     and a /tmp save-load round-trip -- headless, no window needed.
 */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"

#define MAXL    128
#define MAXCOL  96
#define AREA_X  12
#define AREA_Y  64
#define AREA_W  456
#define LINE_H  15
#define VISIBLE 11

/* Key codes (match the ag textbox / PS2 set the kernel emits). */
#define K_LEFT  0x100
#define K_RIGHT 0x101
#define K_UP    0x102
#define K_DOWN  0x103
#define K_HOME  0x104
#define K_END   0x105
#define K_DEL   0x109

static int wid;
static ag_widget_t widgets[12];
static ag_view_t view;
static ag_widget_t *path_box, *status;

static char lines[MAXL][MAXCOL];
static int  n_lines = 0;
static int  cur_line = 0, cur_col = 0;
static int  dirty = 0;
static int  ed_focused = 1;    /* editor owns the keyboard by default */

/* ======================= editor engine ======================= */

static void ed_reset(void) {
    n_lines = 1;
    lines[0][0] = 0;
    cur_line = cur_col = 0;
    dirty = 0;
}

static int ed_insert_char(char c) {
    if (cur_line >= n_lines) return -1;
    int len = (int)strlen(lines[cur_line]);
    if (len >= MAXCOL - 1) return -1;           /* line full */
    for (int i = len; i > cur_col; i--)
        lines[cur_line][i] = lines[cur_line][i - 1];
    lines[cur_line][cur_col] = c;
    lines[cur_line][len + 1] = 0;
    cur_col++;
    dirty = 1;
    return 0;
}

static int ed_newline(void) {
    if (n_lines >= MAXL) return -1;
    if (cur_line >= n_lines) return -1;
    /* Split: tail moves to a new line below. */
    char tail[MAXCOL];
    strcpy(tail, lines[cur_line] + cur_col);
    lines[cur_line][cur_col] = 0;
    for (int i = n_lines; i > cur_line + 1; i--)
        strcpy(lines[i], lines[i - 1]);
    strcpy(lines[cur_line + 1], tail);
    n_lines++;
    cur_line++;
    cur_col = 0;
    dirty = 1;
    return 0;
}

static int ed_backspace(void) {
    if (cur_col > 0) {
        int len = (int)strlen(lines[cur_line]);
        for (int i = cur_col - 1; i < len; i++)
            lines[cur_line][i] = lines[cur_line][i + 1];
        cur_col--;
        dirty = 1;
        return 0;
    }
    if (cur_line == 0) return 0;                /* nothing to join */
    int plen = (int)strlen(lines[cur_line - 1]);
    if (plen + (int)strlen(lines[cur_line]) >= MAXCOL - 1)
        return -1;                              /* join would overflow */
    strcat(lines[cur_line - 1], lines[cur_line]);
    for (int i = cur_line; i < n_lines - 1; i++)
        strcpy(lines[i], lines[i + 1]);
    n_lines--;
    cur_line--;
    cur_col = plen;
    dirty = 1;
    return 0;
}

static int ed_delete(void) {
    int len = (int)strlen(lines[cur_line]);
    if (cur_col < len) {
        for (int i = cur_col; i < len; i++)
            lines[cur_line][i] = lines[cur_line][i + 1];
        dirty = 1;
        return 0;
    }
    if (cur_line + 1 >= n_lines) return 0;      /* last char of buffer */
    if (len + (int)strlen(lines[cur_line + 1]) >= MAXCOL - 1)
        return -1;
    strcat(lines[cur_line], lines[cur_line + 1]);
    for (int i = cur_line + 1; i < n_lines - 1; i++)
        strcpy(lines[i], lines[i + 1]);
    n_lines--;
    dirty = 1;
    return 0;
}

static void ed_move(uint32_t key) {
    int len = (int)strlen(lines[cur_line]);
    switch (key) {
    case K_LEFT:  if (cur_col > 0) cur_col--; break;
    case K_RIGHT: if (cur_col < len) cur_col++; break;
    case K_UP:    if (cur_line > 0) { cur_line--; if (cur_col > (int)strlen(lines[cur_line])) cur_col = (int)strlen(lines[cur_line]); } break;
    case K_DOWN:  if (cur_line < n_lines - 1) { cur_line++; if (cur_col > (int)strlen(lines[cur_line])) cur_col = (int)strlen(lines[cur_line]); } break;
    case K_HOME:  cur_col = 0; break;
    case K_END:   cur_col = len; break;
    }
}

static int ed_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ed_reset();
    int n = 0;              /* chars in current line */
    int truncated = 0;
    int saw_nl = 0;         /* last byte consumed was a newline */
    for (;;) {
        char b[256];
        int64_t r = read(fd, b, sizeof(b));
        if (r <= 0) break;
        for (int i = 0; i < r; i++) {
            char c = b[i];
            if (c == '\r') { saw_nl = 0; continue; }
            if (c == '\n') {
                lines[n_lines - 1][n] = 0;
                if (n_lines >= MAXL) { truncated = 1; n = 0; saw_nl = 1; break; }
                n_lines++;
                n = 0;
                saw_nl = 1;
                continue;
            }
            saw_nl = 0;
            if (c == '\t') c = ' ';
            if (n < MAXCOL - 1) { lines[n_lines - 1][n++] = c; }
            else truncated = 1;
        }
    }
    /* POSIX text-file semantics: a trailing newline TERMINATES the last
     * line instead of starting an empty one, so "alpha\n" is one line
     * and a save/load round-trip returns exactly what was saved. */
    if (saw_nl && n_lines > 1) {
        /* The pending line after the final newline is not real; the line
         * below it was already NUL-terminated when its newline was read. */
        n_lines--;
    } else {
        lines[n_lines - 1][n] = 0;
    }
    close(fd);
    cur_line = cur_col = 0;
    dirty = 0;
    return truncated ? 1 : 0;   /* 1 = loaded with clamping */
}

static int ed_save(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    for (int i = 0; i < n_lines; i++) {
        int len = (int)strlen(lines[i]);
        if (write(fd, lines[i], (size_t)len) != len) { close(fd); return -1; }
        if (write(fd, "\n", 1) != 1) { close(fd); return -1; }
    }
    if (fsync(fd) != 0) { close(fd); return -1; }
    close(fd);
    dirty = 0;
    return 0;
}

/* ======================= GUI chrome ======================= */

static void draw_editor(void) {
    /* auto-scroll the viewport so the caret stays visible */
    int top = cur_line - VISIBLE + 1;
    if (top < 0) top = 0;
    ag_fill_rect(wid, AREA_X, AREA_Y, AREA_W, VISIBLE * LINE_H + 4, AG_WHITE);
    ag_draw_rect(wid, AREA_X, AREA_Y, AREA_W, VISIBLE * LINE_H + 4,
                 ed_focused ? AG_ACCENT : AG_DARK);
    char no[8];
    for (int i = 0; i < VISIBLE && top + i < n_lines; i++) {
        int y = AREA_Y + 4 + i * LINE_H;
        snprintf(no, sizeof no, "%3d", top + i + 1);
        ag_draw_text(wid, AREA_X + 4, y, no, AG_GRAY);
        ag_draw_text(wid, AREA_X + 36, y, lines[top + i],
                     top + i == cur_line ? AG_BLACK : AG_DARK);
        if (top + i == cur_line && ed_focused) {
            int32_t cx = AREA_X + 36 + cur_col * 8;
            ag_draw_line(wid, cx, y, cx, y + LINE_H - 3, AG_ACCENT);
        }
    }
}

static void update_status(void) {
    char s[AG_MAX_WIDGET_TEXT];
    snprintf(s, sizeof s, "Ln %d, Col %d%s  [%s]", cur_line + 1, cur_col + 1,
             dirty ? " *" : "",
             ed_focused ? "editor" : "path box");
    ag_textbox_set(status, s);
}

static void repaint(void) {
    ag_view_render(&view);
    draw_editor();
    update_status();
    ag_render_now();
}

static void on_load(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    int r = ed_load(path_box->text);
    if (r < 0) { ag_textbox_set(status, "load: open failed"); return; }
    printf("GEDIT: loaded %d lines (%s)\n", n_lines, path_box->text);
    repaint();
}

static void on_save(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    if (ed_save(path_box->text) != 0) { ag_textbox_set(status, "save: failed"); return; }
    printf("GEDIT: saved %d lines (%s)\n", n_lines, path_box->text);
    repaint();
}

/* ======================= engine selftest ======================= */

static int st_fails = 0, st_total = 0;
static void st_check(const char *name, int cond) {
    st_total++;
    if (cond) printf("GEDIT PASS: %s\n", name);
    else { printf("GEDIT FAIL: %s\n", name); st_fails++; }
}

static int selftest(void) {
    const char *path = "/tmp/gedit.selftest";
    ed_reset();

    /* --- scripted session: type, split, navigate, edit, join --- */
    for (const char *p = "alpha"; *p; p++) ed_insert_char(*p);
    st_check("typing builds a line", strcmp(lines[0], "alpha") == 0);

    ed_newline();
    for (const char *p = "beta"; *p; p++) ed_insert_char(*p);
    st_check("newline splits the buffer",
             n_lines == 2 && strcmp(lines[0], "alpha") == 0 &&
             strcmp(lines[1], "beta") == 0);

    ed_move(K_UP); ed_move(K_END);
    ed_insert_char('!');
    st_check("UP/END navigate, insert at line end",
             strcmp(lines[0], "alpha!") == 0 && cur_line == 0);

    ed_move(K_HOME); ed_move(K_RIGHT); ed_move(K_RIGHT);  /* col 2 */
    ed_backspace();                       /* delete the char before caret: 'l' */
    st_check("backspace mid-line", strcmp(lines[0], "apha!") == 0 && cur_col == 1);

    ed_move(K_END); ed_delete();          /* join 'beta' onto line 0 */
    st_check("delete joins lines",
             n_lines == 1 && strcmp(lines[0], "apha!beta") == 0);

    /* --- save / reload round-trip --- */
    unlink(path);
    st_check("save ok", ed_save(path) == 0);
    /* static: 12 KB on the user stack would overflow it (engine arrays
     * live in BSS for the same reason). */
    static char before[MAXL][MAXCOL];
    memcpy(before, lines, sizeof(lines));
    ed_reset();
    st_check("load ok", ed_load(path) == 0);
    /* Compare the LIVE lines only: bytes past each NUL are scratch. */
    int same = n_lines == 1;
    for (int i = 0; same && i < n_lines; i++)
        same = strcmp(before[i], lines[i]) == 0;
    st_check("round-trip preserves every line",
             same && strcmp(lines[0], "apha!beta") == 0);
    unlink(path);

    if (st_fails == 0) printf("GEDIT DONE: %d/%d\n", st_total, st_total);
    else printf("GEDIT DONE: %d/%d (%d FAILED)\n", st_total - st_fails, st_total, st_fails);
    return st_fails ? 1 : 0;
}

/* ======================= main ======================= */

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return selftest();

    wid = ag_window_create(120, 90, 480, 300, "Text Editor", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 12, AG_PANEL);

    ag_add_label  (&view, 12, 14, "File:", AG_BLACK);
    path_box = ag_add_textbox(&view, 60, 8, 280, 24, "/tmp/notes.txt");
    ag_add_button (&view, 350, 8, 56, 24, "Load", on_load, 0);
    ag_add_button (&view, 412, 8, 56, 24, "Save", on_save, 0);
    ag_add_label  (&view, 12, 48, "Content (click to edit):", AG_BLACK);
    status = ag_add_textbox(&view, 12, AREA_Y + VISIBLE * LINE_H + 12,
                            456, 24, "Ln 1, Col 1");

    ed_reset();
    repaint();

    for (;;) {
        ag_event_t e;
        if (!ag_poll_event(wid, &e)) {
            for (volatile int i = 0; i < 200000; i++) {}
            continue;
        }
        if (e.type == AG_EVT_MOUSE_DOWN &&
            e.x >= AREA_X && e.x < AREA_X + AREA_W &&
            e.y >= AREA_Y && e.y < AREA_Y + VISIBLE * LINE_H + 4) {
            ed_focused = 1;             /* click content -> edit */
            view.focused_widget = -1;
        } else if (e.type == AG_EVT_MOUSE_DOWN) {
            ed_focused = 0;             /* any other click -> chrome */
        }
        (void)ag_view_dispatch(&view, &e);
        if (e.type == AG_EVT_CLOSE_REQ) break;
        if (e.type == AG_EVT_KEY_DOWN && ed_focused &&
            !(e.mods & 0x02)) {
            if (e.key >= 0x20 && e.key < 0x7F)      ed_insert_char((char)e.key);
            else if (e.key == '\n')                 ed_newline();
            else if (e.key == '\b')                 ed_backspace();
            else if (e.key == K_DEL)                ed_delete();
            else                                    ed_move(e.key);
        }
        if (e.type == AG_EVT_KEY_DOWN && e.key == 0x1B) break;  /* ESC quits */
        repaint();
    }
    return 0;
}
