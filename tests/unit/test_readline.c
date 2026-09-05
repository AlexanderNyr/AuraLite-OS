/*
 * test_readline.c — host-side unit tests for the T4 line editor
 * (lib/libc/src/readline.c).
 *
 * The editing core is pure, so every rule gets driven byte by byte:
 * insert/erase, mid-line editing with tail repaint, ^U/^W kills, ^A/^E,
 * arrow-key cursor motion, the ESC[ n ~ home/end forms, buffer-full
 * behaviour, and the history ring's walk/boundary/duplicate rules.
 *
 * The repaint byte sequences are asserted where the contract is load
 * bearing (an erase in the middle of a line MUST redraw the tail; a kill
 * MUST leave the line visually empty) and ignored elsewhere, which is how
 * the editor itself treats them.
 */

#include "readline.h"
#include "string.h"
#include "stdio.h"

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define CHECK(c) do {                                                   \
    if (!(c)) { printf("    L%d: %s\n", __LINE__, #c); return 0; }      \
} while (0)

/* Feed a string of bytes; discard repaint output. */
static void feed_str(rl_line_t *rl, const char *s) {
    char out[RL_BUF_MAX + 16];
    for (const char *p = s; *p; p++) (void)rl_feed(rl, (unsigned char)*p, out, sizeof(out) - 1);
}

static int t_type_and_erase(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "abc");
    CHECK(rl.len == 3 && rl.pos == 3);
    CHECK(strcmp(rl.buf, "abc") == 0);
    char out[RL_BUF_MAX + 16];
    (void)rl_feed(&rl, 0x7F, out, sizeof(out) - 1);     /* DEL */
    CHECK(strcmp(rl.buf, "ab") == 0 && rl.pos == 2);
    (void)rl_feed(&rl, '\b', out, sizeof(out) - 1);     /* BS: same rule */
    CHECK(strcmp(rl.buf, "a") == 0 && rl.pos == 1);
    return 1;
}

static int t_midline_insert_redraws_tail(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "acd");
    /* Left twice: cursor from the end onto 'c'; insert 'b' -> "abcd". */
    char out[RL_BUF_MAX + 16];
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    int n = rl_feed(&rl, 'D',  out, sizeof(out) - 1);
    CHECK(n == 1);                       /* one \b emitted */
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    (void)rl_feed(&rl, 'D',  out, sizeof(out) - 1);
    CHECK(rl.pos == 1);
    (void)rl_feed(&rl, 'b', out, sizeof(out) - 1);
    CHECK(strcmp(rl.buf, "abcd") == 0);
    CHECK(rl.pos == 2);
    /* The insert repaint emitted the inserted char AND the tail "cd". */
    char tail_probe[RL_BUF_MAX + 16];
    rl_line_t rl2; memset(&rl2, 0, sizeof(rl2));
    feed_str(&rl2, "acd");
    (void)rl_feed(&rl2, 0x01, tail_probe, sizeof(tail_probe) - 1);  /* home */
    (void)rl_feed(&rl2, 0x1B, tail_probe, sizeof(tail_probe) - 1);
    (void)rl_feed(&rl2, '[',  tail_probe, sizeof(tail_probe) - 1);
    (void)rl_feed(&rl2, 'C',  tail_probe, sizeof(tail_probe) - 1);  /* right once */
    int n2 = rl_feed(&rl2, 'b', tail_probe, sizeof(tail_probe) - 1);
    /* 'b' + redraw tail "cd" + blank + park \b's, in order. */
    CHECK(n2 >= 5);
    int saw_b = -1, saw_c = -1, saw_d = -1;
    for (int i = 0; i < n2 && i < (int)sizeof(tail_probe); i++) {
        if (tail_probe[i] == 'b' && saw_b < 0) saw_b = i;
        if (tail_probe[i] == 'c' && saw_c < 0) saw_c = i;
        if (tail_probe[i] == 'd' && saw_d < 0) saw_d = i;
    }
    CHECK(saw_b >= 0 && saw_c > saw_b && saw_d > saw_c);
    return 1;
}

static int t_midline_erase_redraws_tail(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "abc");
    /* Left once (cursor on 'c'), DEL erases 'b' -> "ac". */
    char out[RL_BUF_MAX + 16];
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    (void)rl_feed(&rl, 'D',  out, sizeof(out) - 1);
    int n = rl_feed(&rl, 0x7F, out, sizeof(out) - 1);
    CHECK(strcmp(rl.buf, "ac") == 0);
    CHECK(rl.pos == 1);
    /* Repaint: \b-space-\b, then the tail "c", a blank and park \b's. */
    CHECK(n >= 5);
    int saw_c = -1;
    for (int i = 0; i < n; i++) if (out[i] == 'c') { saw_c = i; break; }
    CHECK(saw_c >= 3);                  /* after the erase trio */
    return 1;
}

static int t_kill_line_and_word(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "one two");
    char out[RL_BUF_MAX + 16];
    (void)rl_feed(&rl, 0x17, out, sizeof(out) - 1);     /* ^W: kill "two" */
    CHECK(strcmp(rl.buf, "one ") == 0);
    (void)rl_feed(&rl, 0x15, out, sizeof(out) - 1);     /* ^U: kill all */
    CHECK(rl.len == 0 && rl.pos == 0 && rl.buf[0] == '\0');
    return 1;
}

static int t_ctrl_a_e(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "xyz");
    char out[RL_BUF_MAX + 16];
    (void)rl_feed(&rl, 0x01, out, sizeof(out) - 1);     /* ^A */
    CHECK(rl.pos == 0);
    (void)rl_feed(&rl, 0x05, out, sizeof(out) - 1);     /* ^E */
    CHECK(rl.pos == 3);
    return 1;
}

static int t_arrow_right_moves_and_emits_char(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "ab");
    char out[RL_BUF_MAX + 16];
    (void)rl_feed(&rl, 0x01, out, sizeof(out) - 1);     /* home */
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    int n = rl_feed(&rl, 'C', out, sizeof(out) - 1);    /* right: onto 'b' */
    CHECK(n == 1 && out[0] == 'a');   /* re-emits the char it stepped over */
    CHECK(rl.pos == 1);
    return 1;
}

static int t_home_end_tilde_forms(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "mid");
    char out[RL_BUF_MAX + 16];
    /* ESC [ 1 ~ = home */
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    (void)rl_feed(&rl, '1',  out, sizeof(out) - 1);
    (void)rl_feed(&rl, '~',  out, sizeof(out) - 1);
    CHECK(rl.pos == 0);
    /* ESC [ 4 ~ = end */
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    (void)rl_feed(&rl, '4',  out, sizeof(out) - 1);
    (void)rl_feed(&rl, '~',  out, sizeof(out) - 1);
    CHECK(rl.pos == 3);
    return 1;
}

static int t_hist_up_codes(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    char out[8];
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    CHECK(rl_feed(&rl, 'A', out, sizeof(out) - 1) == -1);   /* HIST_PREV */
    (void)rl_feed(&rl, 0x1B, out, sizeof(out) - 1);
    (void)rl_feed(&rl, '[',  out, sizeof(out) - 1);
    CHECK(rl_feed(&rl, 'B', out, sizeof(out) - 1) == -2);   /* HIST_NEXT */
    /* The escape state resets after each sequence: a plain 'B' afterwards
     * is an insert, not a sequence tail. */
    (void)rl_feed(&rl, 'B', out, sizeof(out) - 1);
    CHECK(rl.buf[0] == 'B');
    return 1;
}

static int t_buffer_full(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    char out[8];
    for (int i = 0; i < RL_BUF_MAX + 20; i++) {
        (void)rl_feed(&rl, (unsigned char)('a' + (i % 26)), out, sizeof(out) - 1);
    }
    CHECK(rl.len == RL_BUF_MAX - 1);          /* room for the NUL kept */
    CHECK(rl.pos == rl.len);
    return 1;
}

static int t_history_ring(void) {
    rl_hist_add("first");
    rl_hist_add("second");
    rl_hist_add("second");                    /* duplicate: dropped */
    CHECK(rl_hist_count() == 2);
    char buf[RL_BUF_MAX];
    CHECK(rl_hist_step(-1, buf, sizeof(buf)) == 1);   /* up -> newest */
    CHECK(strcmp(buf, "second") == 0);
    CHECK(rl_hist_step(-1, buf, sizeof(buf)) == 1);   /* up -> oldest */
    CHECK(strcmp(buf, "first") == 0);
    CHECK(rl_hist_step(-1, buf, sizeof(buf)) == 0);   /* pinned at oldest */
    CHECK(rl_hist_step(+1, buf, sizeof(buf)) == 1);
    CHECK(strcmp(buf, "second") == 0);
    CHECK(rl_hist_step(+1, buf, sizeof(buf)) == 1);   /* past newest: edit line */
    CHECK(rl_hist_pos() == -1);
    CHECK(rl_hist_step(+1, buf, sizeof(buf)) == 0);   /* pinned below */
    return 1;
}

static int t_ignored_control_bytes(void) {
    rl_line_t rl; memset(&rl, 0, sizeof(rl));
    feed_str(&rl, "a");
    feed_str(&rl, "\x02\x03\x06\x07");        /* unhandled controls */
    CHECK(strcmp(rl.buf, "a") == 0 && rl.len == 1);
    return 1;
}

int main(void) {
    printf("test_readline: the T4 line editor core\n");

    RUN(t_type_and_erase);
    RUN(t_midline_insert_redraws_tail);
    RUN(t_midline_erase_redraws_tail);
    RUN(t_kill_line_and_word);
    RUN(t_ctrl_a_e);
    RUN(t_arrow_right_moves_and_emits_char);
    RUN(t_home_end_tilde_forms);
    RUN(t_hist_up_codes);
    RUN(t_buffer_full);
    RUN(t_history_ring);
    RUN(t_ignored_control_bytes);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
