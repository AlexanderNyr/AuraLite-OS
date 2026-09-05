/* libc/src/readline.c — the T4 line editor (see readline.h).
 *
 * The editing core (rl_feed + history ring) is pure: no syscalls, no
 * termios, no globals beyond the history ring — tests/unit/test_readline.c
 * links this file directly and drives every editing rule byte by byte.
 * readline() is the thin loop layer on top.
 *
 * Contract note: rl_feed() returns the number of terminal-repaint bytes it
 * wrote, EXCEPT two in-band codes for the arrow keys it recognises but
 * cannot act on (history navigation is the loop's job):
 *   RL_KEY_HIST_PREV  — the user pressed Up   (ESC [ A)
 *   RL_KEY_HIST_NEXT  — the user pressed Down (ESC [ B)
 */

#include "readline.h"
#include "string.h"
#include "stdlib.h"
#include "unistd.h"
#include "stdio.h"
#include "termios.h"

#define RL_KEY_HIST_PREV (-1)
#define RL_KEY_HIST_NEXT (-2)

/* ---- local helpers ---- */

static int rl_put(char *out, int out_max, int *w, char c) {
    if (out && *w < out_max) out[*w] = c;
    (*w)++;
    return 0;
}

/* Erase one screen column: \b, space, \b. */
static void emit_bs(char *out, int out_max, int *w) {
    rl_put(out, out_max, w, '\b');
    rl_put(out, out_max, w, ' ');
    rl_put(out, out_max, w, '\b');
}

/* Redraw from cursor to end, blank one leftover column, park the cursor
 * back where it was.  This is what makes mid-line edits (erase, kill,
 * insert) repaint correctly.  Returns the byte count. */
static int emit_redraw_tail(rl_line_t *rl, char *out, int out_max) {
    int w = 0;
    for (int i = rl->pos; i < rl->len; i++)
        rl_put(out, out_max, &w, rl->buf[i]);
    rl_put(out, out_max, &w, ' ');                       /* clear leftover */
    for (int i = rl->pos; i <= rl->len; i++)
        rl_put(out, out_max, &w, '\b');                  /* park cursor */
    return w;
}

/* Drop the byte at pos-1 (used by erase/kill). */
static void rl_drop_before(rl_line_t *rl) {
    int i = --rl->pos;
    while (i < rl->len - 1) { rl->buf[i] = rl->buf[i + 1]; i++; }
    rl->len--;
    rl->buf[rl->len] = '\0';
}

/* ---- the editing state machine ---- */

/* Escape-sequence assembly.  One edit session at a time (the same
 * single-session contract glibc readline has; the libc's threads do not
 * read two prompts at once). */
static int rl_esc;          /* 1 after ESC, 2 after ESC [ / ESC O, 3 swallowing '~' */

int rl_feed(rl_line_t *rl, unsigned char c, char *out, int out_max) {
    int w = 0;
    if (out && out_max > 0) out[0] = '\0';
    if (!rl) return 0;
    rl->dirty = 1;

    /* --- escape sequence assembly --- */
    if (rl_esc == 1) {
        rl_esc = 0;
        if (c == '[' || c == 'O') rl_esc = 2;
        return 0;                       /* lone ESC: ignored */
    }
    if (rl_esc == 2) {
        rl_esc = 0;
        switch (c) {
        case 'D':                                     /* left */
            if (rl->pos > 0) { rl->pos--; rl_put(out, out_max, &w, '\b'); }
            break;
        case 'C':                                     /* right */
            if (rl->pos < rl->len) {
                rl_put(out, out_max, &w, rl->buf[rl->pos]);
                rl->pos++;
            }
            break;
        case 'A': return RL_KEY_HIST_PREV;            /* up    */
        case 'B': return RL_KEY_HIST_NEXT;            /* down  */
        case 'H':                                     /* home  */
            while (rl->pos > 0) { rl->pos--; rl_put(out, out_max, &w, '\b'); }
            break;
        case 'F':                                     /* end   */
            while (rl->pos < rl->len) {
                rl_put(out, out_max, &w, rl->buf[rl->pos]);
                rl->pos++;
            }
            break;
        case '1': case '7':                           /* ESC [ 1 ~ / 7 ~: home */
            while (rl->pos > 0) { rl->pos--; rl_put(out, out_max, &w, '\b'); }
            rl_esc = 3;                               /* swallow the '~' */
            if (out && w < out_max) out[w] = '\0';
            return w;
        case '4': case '8':                           /* ESC [ 4 ~ / 8 ~: end */
            while (rl->pos < rl->len) {
                rl_put(out, out_max, &w, rl->buf[rl->pos]);
                rl->pos++;
            }
            rl_esc = 3;                               /* swallow the '~' */
            if (out && w < out_max) out[w] = '\0';
            return w;
        default:
            break;                                    /* unknown: dropped */
        }
        if (out && w < out_max) out[w] = '\0';
        return w;
    }
    if (rl_esc == 3) { rl_esc = 0; return 0; }        /* the trailing '~' */

    /* --- plain bytes --- */
    if (c == 0x1B) { rl_esc = 1; return 0; }          /* ESC */
    if (c == '\r' || c == '\n') { rl->dirty = 0; return 0; }  /* loop layer */

    if (c == 0x7F || c == '\b') {                     /* erase before cursor */
        if (rl->pos > 0) {
            rl_drop_before(rl);
            emit_bs(out, out_max, &w);
            char tail[RL_BUF_MAX + 8];
            int t2 = emit_redraw_tail(rl, tail, (int)sizeof(tail) - 1);
            for (int k = 0; k < t2; k++) rl_put(out, out_max, &w, tail[k]);
        }
        if (out && w < out_max) out[w] = '\0';
        return w;
    }

    if (c == 0x15) {                                   /* ^U: kill to start */
        while (rl->pos > 0) {
            rl_drop_before(rl);
            emit_bs(out, out_max, &w);
        }
        char tail[RL_BUF_MAX + 8];
        int t2 = emit_redraw_tail(rl, tail, (int)sizeof(tail) - 1);
        for (int k = 0; k < t2; k++) rl_put(out, out_max, &w, tail[k]);
        if (out && w < out_max) out[w] = '\0';
        return w;
    }

    if (c == 0x17) {                                   /* ^W: kill word */
        while (rl->pos > 0 && rl->buf[rl->pos - 1] == ' ') {
            rl_drop_before(rl);
            emit_bs(out, out_max, &w);
        }
        while (rl->pos > 0 && rl->buf[rl->pos - 1] != ' ') {
            rl_drop_before(rl);
            emit_bs(out, out_max, &w);
        }
        char tail[RL_BUF_MAX + 8];
        int t2 = emit_redraw_tail(rl, tail, (int)sizeof(tail) - 1);
        for (int k = 0; k < t2; k++) rl_put(out, out_max, &w, tail[k]);
        if (out && w < out_max) out[w] = '\0';
        return w;
    }

    if (c == 0x01) {                                   /* ^A: home */
        while (rl->pos > 0) { rl->pos--; rl_put(out, out_max, &w, '\b'); }
        if (out && w < out_max) out[w] = '\0';
        return w;
    }
    if (c == 0x05) {                                   /* ^E: end */
        while (rl->pos < rl->len) {
            rl_put(out, out_max, &w, rl->buf[rl->pos]);
            rl->pos++;
        }
        if (out && w < out_max) out[w] = '\0';
        return w;
    }

    if (c < 0x20) { rl->dirty = 0; return 0; }         /* other control: drop */

    /* Printable: insert at cursor. */
    if (rl->len < RL_BUF_MAX - 1) {
        int i = rl->len;
        while (i > rl->pos) { rl->buf[i] = rl->buf[i - 1]; i--; }
        rl->buf[rl->pos] = (char)c;
        rl->pos++;
        rl->len++;
        rl->buf[rl->len] = '\0';
        rl_put(out, out_max, &w, (char)c);
        char tail[RL_BUF_MAX + 8];
        int t2 = emit_redraw_tail(rl, tail, (int)sizeof(tail) - 1);
        for (int k = 0; k < t2; k++) rl_put(out, out_max, &w, tail[k]);
    } else {
        rl->dirty = 0;
    }
    if (out && w < out_max) out[w] = '\0';
    return w;
}

/* ---- history ring ---- */

static char rl_hist[RL_HIST_MAX][RL_BUF_MAX];
static int  rl_hist_n;            /* entries used */
static int  rl_hist_walk;         /* navigation cursor, -1 = at the edit line */

void rl_hist_add(const char *line) {
    if (!line || !line[0]) return;
    /* Duplicate of the newest entry: skip (bash behaviour). */
    if (rl_hist_n > 0 && strcmp(rl_hist[rl_hist_n - 1], line) == 0) {
        rl_hist_walk = -1;
        return;
    }
    if (rl_hist_n == RL_HIST_MAX) {
        for (int i = 0; i < RL_HIST_MAX - 1; i++)
            memcpy(rl_hist[i], rl_hist[i + 1], RL_BUF_MAX);
        rl_hist_n--;
    }
    strncpy(rl_hist[rl_hist_n], line, RL_BUF_MAX - 1);
    rl_hist[rl_hist_n][RL_BUF_MAX - 1] = '\0';
    rl_hist_n++;
    rl_hist_walk = -1;
}

int rl_hist_step(int dir, char *out, int out_max) {
    if (rl_hist_n == 0) return 0;
    if (dir < 0) {                                    /* older (up) */
        if (rl_hist_walk < 0) rl_hist_walk = rl_hist_n - 1;
        else if (rl_hist_walk > 0) rl_hist_walk--;
        else return 0;                                /* already oldest */
    } else {                                          /* newer (down) */
        if (rl_hist_walk < 0) return 0;               /* already at edit line */
        rl_hist_walk++;
        if (rl_hist_walk >= rl_hist_n) {
            rl_hist_walk = -1;                        /* back to the edit line */
            return 1;
        }
    }
    if (out && out_max > 0) {
        strncpy(out, rl_hist[rl_hist_walk], out_max - 1);
        out[out_max - 1] = '\0';
    }
    return 1;
}

int rl_hist_pos(void)   { return rl_hist_walk; }
int rl_hist_count(void) { return rl_hist_n; }

/* ---- the loop layer ---- */

static void write_all(const char *p, int n) {
    if (p && n > 0) (void)write(1, p, (unsigned)n);
}

/* Replace the on-screen line: \r, prompt, new text, blank the leftovers,
 * park the cursor at the end.  The caller owns the prompt bytes. */
static void repaint_history_line(const char *prompt, rl_line_t *rl) {
    char out[RL_BUF_MAX + 64];
    int w = 0;
    rl_put(out, (int)sizeof(out) - 1, &w, '\r');
    rl_put(out, (int)sizeof(out) - 1, &w, ' ');   /* visual kick, erased below */
    rl_put(out, (int)sizeof(out) - 1, &w, '\r');
    if (prompt) {
        for (const char *p = prompt; *p; p++)
            rl_put(out, (int)sizeof(out) - 1, &w, *p);
    }
    for (int i = 0; i < rl->len; i++)
        rl_put(out, (int)sizeof(out) - 1, &w, rl->buf[i]);
    /* Blank what the previous entry left behind. */
    static int last_painted;                       /* one session at a time */
    for (int i = rl->len; i < last_painted; i++)
        rl_put(out, (int)sizeof(out) - 1, &w, ' ');
    for (int i = rl->len; i < last_painted; i++)
        rl_put(out, (int)sizeof(out) - 1, &w, '\b');
    last_painted = rl->len;
    write_all(out, w);
    rl->pos = rl->len;
}

char *readline(const char *prompt) {
    rl_line_t rl;
    memset(&rl, 0, sizeof(rl));
    rl_esc = 0;

    /* Echo policy (header comment): echo ourselves only when the tty under
     * fd 0 has ECHO off — a tty with echo on is already doing it (the
     * discipline, or the legacy console path).  Non-tty stdin (a pipe in a
     * host test): echo nothing, editing still works for the byte stream. */
    struct termios tio;
    int have_tty = (tcgetattr(0, &tio) == 0);
    int echo_here = have_tty && !(tio.c_lflag & ECHO);

    const char *p = (prompt && *prompt) ? prompt : 0;
    if (p) {
        for (const char *q = p; *q; q++) putchar(*q);
        fflush(stdout);
    }

    char out[RL_BUF_MAX + 16];
    char hist[RL_BUF_MAX];
    for (;;) {
        unsigned char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) return rl.len ? strdup(rl.buf) : NULL;   /* EOF: partial or NULL */

        if (c == '\n' || c == '\r') {
            if (echo_here) {
                putchar('\n');
                fflush(stdout);
            }
            rl_hist_add(rl.buf);
            return strdup(rl.buf);
        }

        int n = rl_feed(&rl, c, out, (int)sizeof(out) - 1);
        if (n == RL_KEY_HIST_PREV || n == RL_KEY_HIST_NEXT) {
            if (rl_hist_step(n == RL_KEY_HIST_PREV ? -1 : 1, hist, sizeof(hist))) {
                if (rl_hist_pos() < 0) {
                    /* Down past the newest: back to an empty edit line. */
                    memset(&rl, 0, sizeof(rl));
                } else {
                    memset(&rl, 0, sizeof(rl));
                    memcpy(rl.buf, hist, RL_BUF_MAX);   /* hist is always NUL-filled */
                    rl.len = (int)strlen(rl.buf);
                    if (rl.len >= RL_BUF_MAX) rl.len = RL_BUF_MAX - 1;
                    rl.buf[rl.len] = '\0';
                    rl.pos = rl.len;
                }
                if (echo_here) repaint_history_line(p, &rl);
            }
            continue;
        }
        if (echo_here) write_all(out, n);
    }
}
