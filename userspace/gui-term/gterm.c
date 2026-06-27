/* gterm — minimal GUI terminal emulator.
 *
 * Shows a scrollback area + input textbox.  When user presses Enter, the
 * input string is "echoed" + run as built-in command (ls, cat, echo, pwd,
 * mkdir, rm — going through the same syscalls the shell uses).
 */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[8];
static ag_view_t view;
static ag_widget_t *input;

#define LINES 14
#define COLS  72
static char history[LINES][COLS + 1];
static int  hlen = 0;

static void hist_push(const char *s) {
    if (hlen >= LINES) {
        for (int i = 0; i < LINES - 1; i++) memcpy(history[i], history[i+1], COLS + 1);
        hlen = LINES - 1;
    }
    int n = 0;
    while (s[n] && n < COLS) { history[hlen][n] = s[n]; n++; }
    history[hlen][n] = 0;
    hlen++;
}

static void redraw_history(void) {
    ag_fill_rect(wid, 12, 12, 504, LINES * 14 + 4, AG_BLACK);
    for (int i = 0; i < hlen; i++) {
        uint32_t color = AG_GREEN;
        if (history[i][0] == '$') color = AG_YELLOW;
        ag_draw_text(wid, 16, 16 + i * 14, history[i], color);
    }
}

static void run_command(const char *line) {
    /* Tokenize first word. */
    char cmd[32]; int n = 0;
    while (line[n] == ' ') line++;
    while (line[n] && line[n] != ' ' && n < 31) { cmd[n] = line[n]; n++; }
    cmd[n] = 0;
    const char *arg = (line[n] == ' ') ? line + n + 1 : "";

    char buf[COLS + 1];
    /* Echo command. */
    buf[0] = '$'; buf[1] = ' ';
    int p = 2;
    while (line[p - 2] && p < COLS) { buf[p] = line[p - 2]; p++; }
    buf[p] = 0;
    hist_push(buf);

    if (strcmp(cmd, "help") == 0) {
        hist_push("commands: help, ls, cat, pwd, echo, uname, clear, exit");
    } else if (strcmp(cmd, "clear") == 0) {
        hlen = 0;
    } else if (strcmp(cmd, "exit") == 0) {
        hist_push("goodbye!");
    } else if (strcmp(cmd, "pwd") == 0) {
        hist_push("/");
    } else if (strcmp(cmd, "uname") == 0) {
        hist_push("AuraLite OS 1.0.0 x86_64");
    } else if (strcmp(cmd, "echo") == 0) {
        hist_push(arg);
    } else if (strcmp(cmd, "ls") == 0) {
        /* listdir prints to console; just show a hint here. */
        hist_push("(see console for ls output)");
        listdir(arg[0] ? arg : "/");
    } else if (strcmp(cmd, "cat") == 0) {
        if (!arg[0]) { hist_push("cat: missing file"); }
        else {
            int fd = open(arg, O_RDONLY);
            if (fd < 0) hist_push("cat: open failed");
            else {
                char b[COLS + 1];
                int64_t r = read(fd, b, COLS);
                close(fd);
                if (r > 0) {
                    b[r] = 0;
                    for (int i = 0; i < r; i++) if (b[i] < 0x20 && b[i] != ' ') b[i] = ' ';
                    hist_push(b);
                } else hist_push("(empty)");
            }
        }
    } else {
        hist_push("command not found");
    }
    redraw_history();
}

static void on_enter(ag_widget_t *w, void *u) {
    (void)u;
    run_command(w->text);
    ag_textbox_set(w, "");
    ag_view_render(&view);
}

static int loop_cb(ag_view_t *v, const ag_event_t *e, void *u) {
    (void)v; (void)u;
    if (e->type == AG_EVT_KEY_DOWN && e->key == '\n' && input->focused) {
        on_enter(input, 0);
    }
    return 0;
}

int main(void) {
    wid = ag_window_create(100, 70, 540, 260, "Terminal", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 8, AG_PANEL);
    /* Title-like banner. */
    hist_push("AuraLite Terminal 1.0  type 'help'");
    /* Scrollback is drawn during ag_view_render via fill_rect + history. */
    input = ag_add_textbox(&view, 12, LINES * 14 + 24, 504, 24, "");
    /* Add a 'Run' button so mouse users can also submit. */
    ag_add_button(&view, 12 + 510, LINES * 14 + 24, 16, 24, ">", 0, 0);
    /* Initial render then loop. */
    ag_view_render(&view);
    redraw_history();
    ag_render_now();
    ag_view_run(&view, loop_cb, 0);
    return 0;
}
