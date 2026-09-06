/* gtheme — GUI Theme Manager (RESIDUE2 T7 rewrite).
 *
 * The first version of this app was a bug with a window on top: it opened
 * the theme file O_RDONLY and then wrote to that read-only fd, and its
 * status line said "Reboot/restart app to apply" because nothing ever
 * applied a theme.  This version does the real thing:
 *
 *   - Apply  : re-tints the LIVE desktop (accent = title bars, window
 *              borders, start button) via ag_theme_set() -- the caller
 *              owns a window, so the T7 clipboard-era ACL lets it through
 *              -- and persists the whole theme to the dotfile.
 *   - Reload : reads the dotfile back and applies it.
 *
 * Headless CLI (windowless -- note these touch NO gated syscall):
 *   gtheme --save 0xRRGGBB   persist accent to the dotfile (fsync'd)
 *   gtheme --show            print the persisted accent (or DEFAULT)
 *   gtheme --selftest        save/load round-trip on /tmp, all fields
 */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define DOTFILE AG_THEME_DOTFILE

static int wid;
static ag_widget_t widgets[16];
static ag_view_t view;
static ag_widget_t *hex_box;
static ag_widget_t *status;

/* The accent touches every chrome element that "reads" as selection. */
static void tint_accent(ag_theme_t *t, uint32_t accent) {
    t->title_active  = accent;
    t->border_active = accent;
    t->start_btn_bg  = accent;
    t->taskbar_border = accent;
}

static int parse_hex(const char *s, uint32_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!s[0]) return 0;
    uint32_t v = 0;
    for (; *s; s++) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 1;
}

/* ---- GUI mode --------------------------------------------------------- */

static void on_apply(ag_widget_t *w, void *user) {
    (void)w; (void)user;
    uint32_t accent;
    if (!parse_hex(hex_box->text, &accent)) {
        ag_textbox_set(status, "bad hex (expect 0x00RRGGBB)");
        return;
    }
    ag_theme_t t;
    if (ag_theme_get(&t) != 0) {
        ag_textbox_set(status, "theme read failed");
        return;
    }
    tint_accent(&t, accent);
    /* Live: the caller owns this window, so the T7 ACL permits it. */
    if (ag_theme_set(&t) != 0) {
        ag_textbox_set(status, "apply failed (ACL?)");
        return;
    }
    if (ag_theme_save(DOTFILE) != 0) {
        ag_textbox_set(status, "applied, but dotfile save failed");
        return;
    }
    ag_textbox_set(status, "applied + saved to " DOTFILE);
}

static void on_reload(ag_widget_t *w, void *user) {
    (void)w; (void)user;
    ag_theme_t t;
    if (ag_theme_load(DOTFILE, &t) != 0) {
        ag_textbox_set(status, "no saved theme yet");
        return;
    }
    if (ag_theme_set(&t) != 0) {
        ag_textbox_set(status, "apply failed (ACL?)");
        return;
    }
    char msg[AG_MAX_WIDGET_TEXT];
    snprintf(msg, sizeof msg, "reloaded: accent 0x%06X", t.title_active & 0xFFFFFF);
    ag_textbox_set(status, msg);
}

/* ---- CLI modes (no window -> no gated syscalls) ------------------------ */

static int cli_save(const char *hex) {
    uint32_t accent;
    if (!parse_hex(hex, &accent)) {
        printf("GTHEME ERROR: bad hex '%s'\n", hex);
        return 1;
    }
    ag_theme_t t;
    if (ag_theme_get(&t) != 0) { printf("GTHEME ERROR: theme read\n"); return 1; }
    tint_accent(&t, accent);
    /* Windowless: the T7 ACL denies the live SET, and that is fine -- the
     * dotfile is written from the explicit struct, no gated syscall.  The
     * next desktop (glaunch) applies it at startup. */
    if (ag_theme_save2(DOTFILE, &t) != 0) { printf("GTHEME ERROR: save\n"); return 1; }
    printf("GTHEME SAVED 0x%06X (applies at next desktop start)\n", accent & 0xFFFFFF);
    return 0;
}

static int cli_show(void) {
    ag_theme_t t;
    if (ag_theme_load(DOTFILE, &t) != 0) {
        printf("GTHEME DEFAULT\n");
        return 0;
    }
    printf("GTHEME ACCENT 0x%06X\n", t.title_active & 0xFFFFFF);
    return 0;
}

static int cli_selftest(void) {
    /* Round-trip every theme field through a dotfile on /tmp. */
    const char *path = "/tmp/.aura-theme.rtt";
    unlink(path);
    ag_theme_t a, b;
    if (ag_theme_get(&a) != 0) { printf("GTHEME RTT FAIL: read\n"); return 1; }
    /* Mutate scattered fields so a field-mapping bug cannot hide. */
    a.title_active = 0x00AA3311;
    a.win_round = 7;
    a.icon_pad = 13;
    a.shadow_offset = -3;
    if (ag_theme_save2(path, &a) != 0) { printf("GTHEME RTT FAIL: save\n"); return 1; }
    if (ag_theme_load(path, &b) != 0) { printf("GTHEME RTT FAIL: load\n"); return 1; }
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        printf("GTHEME RTT FAIL: fields differ\n");
        return 1;
    }
    unlink(path);
    printf("GTHEME RTT PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--show") == 0)     return cli_show();
        if (strcmp(argv[1], "--selftest") == 0) return cli_selftest();
        if (strcmp(argv[1], "--save") == 0 && argc > 2) return cli_save(argv[2]);
        printf("usage: gtheme [--save 0xRRGGBB | --show | --selftest]\n");
        return 1;
    }

    wid = ag_window_create(100, 100, 340, 200, "Theme Manager", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);

    ag_view_init(&view, wid, widgets, 16, AG_PANEL);

    ag_add_label (&view, 20, 24, "Accent (0x00RRGGBB):", AG_BLACK);
    hex_box = ag_add_textbox(&view, 20, 44, 200, 24, "0x002F60C0");
    ag_add_button(&view, 230, 44, 84, 24, "Apply", on_apply, 0);

    ag_add_button(&view, 20, 80, 84, 24, "Reload", on_reload, 0);
    ag_add_label (&view, 116, 84, "apply = live + save dotfile", AG_GRAY);

    status = ag_add_textbox(&view, 20, 140, 296, 24, "Ready");

    ag_view_run(&view, 0, 0);
    return 0;
}
