/* gclip — GUI clipboard manager (RESIDUE2 T7).
 *
 * The kernel clipboard finally has both halves of a contract worth
 * managing: reads and writes are limited to window-owning GUI
 * participants (the T7 ACL, see /tests guiacl), and textboxes already
 * speak Ctrl+C / Ctrl+X / Ctrl+V against it.  This app is the visible
 * half: set the clipboard from a textbox, paste it back, clear it --
 * every action also lands on the serial log so a headless gate can pin
 * the behaviour (`gclip --selftest` runs the same actions without the
 * window loop and exits).
 */
#include "auragui.h"
#include "unistd.h"
#include "stdio.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[16];
static ag_view_t view;
static ag_widget_t *box, *status;

static int clip_fails = 0, clip_total = 0;
static void clip_check(const char *name, int cond) {
    clip_total++;
    if (cond) printf("GCLIP PASS: %s\n", name);
    else { printf("GCLIP FAIL: %s\n", name); clip_fails++; }
}

/* One set->get cycle, used by both the button and --selftest. */
static int clip_roundtrip(const char *text, char *back, uint32_t back_sz) {
    if (ag_set_clipboard(text) != 0) return -1;
    back[0] = 0;
    if (ag_get_clipboard(back, back_sz) != 0) return -2;
    return strcmp(back, text) == 0 ? 0 : -3;
}

static void refresh_status(const char *what) {
    char cur[128];
    if (ag_get_clipboard(cur, sizeof cur) == 0) {
        char s[AG_MAX_WIDGET_TEXT];
        snprintf(s, sizeof s, "%s | clipboard: %.32s", what, cur);
        ag_textbox_set(status, s);
    } else {
        ag_textbox_set(status, what);
    }
}

static void on_set(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    if (ag_set_clipboard(box->text) != 0) {
        ag_textbox_set(status, "set failed (ACL: no window?)");
        printf("GCLIP: set FAILED\n");
        return;
    }
    printf("GCLIP: set ok\n");
    refresh_status("set");
}

static void on_paste(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    char buf[AG_MAX_WIDGET_TEXT];
    if (ag_get_clipboard(buf, sizeof buf) != 0) {
        ag_textbox_set(status, "get failed");
        printf("GCLIP: get FAILED\n");
        return;
    }
    ag_textbox_set(box, buf);
    printf("GCLIP: pasted '%.32s'\n", buf);
    refresh_status("pasted");
}

static void on_clear(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    ag_set_clipboard("");
    printf("GCLIP: cleared\n");
    refresh_status("cleared");
}

static int selftest(void) {
    /* The app's own contract, exercised the way a user would. */
    char back[128];

    int r = clip_roundtrip("GCLIP-TOKEN-42", back, sizeof back);
    clip_check("set+get round-trip", r == 0);
    if (r == 0) clip_check("payload intact", strcmp(back, "GCLIP-TOKEN-42") == 0);

    clip_check("clear to empty", ag_set_clipboard("") == 0);
    back[0] = 'X';
    clip_check("get after clear", ag_get_clipboard(back, sizeof back) == 0 &&
                                 back[0] == 0);

    /* Long-string clamp: set 200 'A's, get into 64 bytes -> must return
     * a truncated but NUL-terminated string, not overflow. */
    char big[201];
    memset(big, 'A', 200); big[200] = 0;
    clip_check("long set accepted", ag_set_clipboard(big) == 0);
    char small[64];
    clip_check("short buffer get ok", ag_get_clipboard(small, sizeof small) == 0);
    clip_check("short buffer NUL-terminated",
               memchr(small, 0, sizeof small) != 0);

    if (clip_fails == 0) printf("GCLIP DONE: %d/%d\n", clip_total, clip_total);
    else printf("GCLIP DONE: %d/%d (%d FAILED)\n",
                clip_total - clip_fails, clip_total, clip_fails);
    return clip_fails ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        /* --selftest needs the participant ACL: open a window first. */
        wid = ag_window_create(0, 0, 1, 1, "", AG_WIN_BORDERLESS);
        if (wid < 0) { printf("GCLIP FAIL: no window\n"); return 1; }
        int rc = selftest();
        ag_window_destroy(wid);
        return rc;
    }

    wid = ag_window_create(160, 120, 360, 190, "Clipboard", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 16, AG_PANEL);

    ag_add_label(&view, 16, 14, "Text:", AG_BLACK);
    box = ag_add_textbox(&view, 16, 34, 240, 24, "");
    ag_add_button(&view, 268, 34, 70, 24, "Set", on_set, 0);

    ag_add_button(&view, 16, 72, 70, 24, "Paste", on_paste, 0);
    ag_add_button(&view, 96, 72, 70, 24, "Clear", on_clear, 0);
    ag_add_label(&view, 180, 76, "Ctrl+C / Ctrl+V work in textboxes", AG_GRAY);

    status = ag_add_textbox(&view, 16, 130, 328, 24, "ready");

    refresh_status("ready");
    ag_view_run(&view, 0, 0);
    return 0;
}
