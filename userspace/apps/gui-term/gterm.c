/* gterm — GUI terminal for AuraLite OS.
 *
 * Real shell experience inside a window:
 *  - builtins: help, clear, exit, pwd, cd, uname, echo, ls, cat, mkdir, rm
 *    (mkdir/rm accept a path; rm == unlink for files, rmdir for dirs)
 *  - ANY installed program (weather, sysinfo, snake, ...) runs with its
 *    stdout+stderr captured through a pipe and streamed into the scrollback:
 *      pipe -> dup2 onto 1/2 -> spawnv -> drain -> waitpid.
 *    The child inherits the terminal's cwd, so `cd` affects later commands.
 *
 * History note: the very first version of this file was a widget demo whose
 * drawing bug made it look dead — the scrollback was painted and THEN
 * ag_view_render() hit the whole window with ag_clear(bg) (it also does that
 * unconditionally at the end of EVERY ag_view_run() iteration), so a command
 * executed but nothing ever appeared, and `ls` dumped its listing to the
 * kernel console instead of the window.  The fixes below: a custom event
 * loop that always repaints in the one correct order (clear + widgets first,
 * scrollback text last, then ag_render_now), and real captured output.
 */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "dirent.h"
#include "sys/wait.h"

static int wid;
static ag_widget_t widgets[8];
static ag_view_t view;
static ag_widget_t *input;

#define COLS      84           /* max characters per displayed line */
#define VISIBLE   17           /* lines visible in the scrollback area */
#define HIST_SIZE 96           /* ring buffer capacity (scrollback memory) */
#define LINE_H    16
#define AREA_X    12
#define AREA_Y    12

static char hist[HIST_SIZE][COLS + 1];
static int  hist_n = 0;        /* total lines ever pushed (mod ring) */

/* ---- scrollback ------------------------------------------------------- */

static void hist_push_raw(const char *s) {
    int dst = hist_n % HIST_SIZE;
    int i = 0;
    while (s[i] && i < COLS) { hist[dst][i] = s[i]; i++; }
    hist[dst][i] = 0;
    hist_n++;
}

/* Feed arbitrary text: control chars are sanitized, '\n' splits lines and
 * long lines wrap at COLS, so piped program output always lands cleanly. */
static void term_text(const char *s) {
    char clean[512];
    int ci = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\r' || c == '\t') c = ' ';
        if (c == '\n') {
            clean[ci] = 0;
            hist_push_raw(clean);
            ci = 0;
            continue;
        }
        if (c < 0x20 || c == 0x7F) c = ' ';
        clean[ci++] = (char)c;
        if (ci == COLS) {                 /* hard wrap */
            clean[ci] = 0;
            hist_push_raw(clean);
            ci = 0;
        }
    }
    if (ci > 0) {
        clean[ci] = 0;
        hist_push_raw(clean);
    }
}

static void term_push(const char *s) { term_text(s); }

/* ---- painting --------------------------------------------------------- */

static void draw_history(void) {
    int first = hist_n - VISIBLE;
    if (first < 0) first = 0;
    /* black terminal area */
    ag_fill_rect(wid, AREA_X, AREA_Y, 616, VISIBLE * LINE_H + 4, AG_BLACK);
    /* subtle frame */
    ag_draw_rect(wid, AREA_X, AREA_Y, 616, VISIBLE * LINE_H + 4, AG_GRAY);
    int row = 0;
    for (int i = first; i < hist_n; i++, row++) {
        const char *line = hist[i % HIST_SIZE];
        uint32_t color = AG_GREEN;
        if (line[0] == '$') color = AG_YELLOW;
        else if (line[0] == '[') color = AG_GRAY;
        ag_draw_text(wid, AREA_X + 5, AREA_Y + 4 + row * LINE_H, line, color);
    }
}

/* The ONE place that decides what hits the screen, in the only correct
 * order: clear + widgets, then scrollback text, then present. */
static void repaint(void) {
    ag_view_render(&view);      /* ag_clear(bg) + widgets (+ internal flush) */
    draw_history();
    ag_render_now();
}

/* ---- external commands ------------------------------------------------ */

static void run_external(const char *path, char *const argv[]) {
    int fds[2];
    if (pipe(fds) < 0) { term_push("[!] pipe() failed"); return; }

    /* Redirect our stdout/stderr into the pipe for the duration of the
     * spawn: process_spawn() makes the child INHERIT this fd table (see
     * vfs_fork_inherit in kernel/proc/process.c), so the child's writes to
     * 1/2 land in the pipe. */
    fflush(stdout);
    fflush(stderr);
    int save1 = dup(1), save2 = dup(2);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[1]);              /* child gets only its own 1/2 as writers */
    pid_t pid = spawnv(path, argv);
    if (save1 >= 0) { dup2(save1, 1); close(save1); }
    if (save2 >= 0) { dup2(save2, 2); close(save2); }

    if (pid < 0) {
        term_push("[!] spawn failed");
        close(fds[0]);
        return;
    }

    char head[96];
    snprintf(head, sizeof head, "[running %s (pid %d)]", path, (int)pid);
    term_push(head);
    repaint();

    /* Stream output as it arrives; EOF when the child (and its progeny)
     * closes the last write end. */
    for (;;) {
        char chunk[256];
        int64_t r = read(fds[0], chunk, sizeof(chunk) - 1);
        if (r <= 0) break;
        chunk[r] = 0;
        term_text(chunk);
        repaint();
    }
    close(fds[0]);

    int status = 0;
    pid_t got;
    do { got = waitpid(pid, &status, 0); } while (got != pid && got >= 0);
    if (status != 0) {
        char tail[64];
        snprintf(tail, sizeof tail, "[exit status %d]", status);
        term_push(tail);
    }
}

/* ---- builtins --------------------------------------------------------- */

static void bi_ls(const char *arg) {
    static struct aura_dirent ents[96];
    /* vfs_readdir does not resolve "." against the cwd: build an absolute
     * path ourselves (bare `ls` after `cd` would otherwise fail). */
    char path[160];
    const char *use = arg;
    if (!arg[0]) {
        if (!getcwd(path, sizeof path)) { term_push("ls: getcwd failed"); return; }
        use = path;
    } else if (arg[0] != '/') {
        char b[96];
        if (!getcwd(b, sizeof b)) { term_push("ls: getcwd failed"); return; }
        snprintf(path, sizeof path, "%s/%s",
                 (b[0] == '/' && b[1] == 0) ? "" : b, arg);
        use = path;
    }
    int n = aura_readdir(use, ents, 96);
    if (n < 0) { term_push("ls: cannot read directory"); return; }
    char ln[COLS + 1];
    int used = 0;
    ln[0] = 0;
    for (int i = 0; i < n; i++) {
        char name[280];
        int l = snprintf(name, sizeof name, "%s%s",
                         ents[i].name, ents[i].type == DT_DIR ? "/" : "");
        if (used + l + 2 > COLS) {        /* wrap into columns */
            term_push(ln);
            used = 0; ln[0] = 0;
        }
        memcpy(ln + used, name, (size_t)l);
        used += l;
        ln[used++] = ' '; ln[used++] = ' ';
        ln[used] = 0;
    }
    if (used > 0) term_push(ln);
    if (n == 0)   term_push("(empty directory)");
}

static void bi_cat(const char *arg) {
    if (!arg[0]) { term_push("cat: missing file"); return; }
    int fd = open(arg, O_RDONLY);
    if (fd < 0) { term_push("cat: cannot open file"); return; }
    int64_t total = 0;
    for (;;) {
        char b[256];
        int64_t r = read(fd, b, sizeof(b) - 1);
        if (r <= 0) break;
        total += r;
        b[r] = 0;
        term_text(b);
    }
    close(fd);
    if (total == 0) term_push("(empty file)");
}

/* ---- command dispatch ------------------------------------------------- */

static void run_command(const char *line, int *want_exit) {
    while (*line == ' ') line++;
    if (!*line) return;

    /* Echo prompt. */
    char echo[COLS + 1];
    snprintf(echo, sizeof echo, "$ %s", line);
    hist_push_raw(echo);            /* prompt is never wrapped */

    /* Tokenize: argv[0] = command, up to 8 args, space-separated. */
    static char copy[160];
    static char *argv[10];
    int argc = 0;
    int ci = 0;
    for (int i = 0; line[i] && ci < (int)sizeof(copy) - 1; i++)
        copy[ci++] = line[i];
    copy[ci] = 0;
    char *p = copy;
    while (*p && argc < 9) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    const char *cmd = argv[0];
    const char *arg = (argc >= 2) ? argv[1] : "";

    if      (strcmp(cmd, "help")  == 0) {
        term_push("builtins: help clear exit pwd cd ls cat mkdir rm echo uname");
        term_push("any installed program works too: weather, sysinfo, snake, ...");
    }
    else if (strcmp(cmd, "clear") == 0) { hist_n = 0; }
    else if (strcmp(cmd, "exit")  == 0) { *want_exit = 1; return; }
    else if (strcmp(cmd, "uname") == 0) { term_push("AuraLite OS x86_64"); }
    else if (strcmp(cmd, "echo")  == 0) { term_text(line + 4 + (line[4] ? 1 : 0)); }
    else if (strcmp(cmd, "pwd")   == 0) {
        char b[COLS + 1];
        term_push(getcwd(b, sizeof b) ? b : "?");
    }
    else if (strcmp(cmd, "cd")    == 0) {
        if (chdir(arg[0] ? arg : "/") < 0) term_push("cd: no such directory");
    }
    else if (strcmp(cmd, "ls")    == 0) { bi_ls(arg); }
    else if (strcmp(cmd, "cat")   == 0) { bi_cat(arg); }
    else if (strcmp(cmd, "mkdir") == 0) {
        if (!arg[0] || mkdir(arg, 0755) < 0) term_push("mkdir: failed");
    }
    else if (strcmp(cmd, "rm")    == 0) {
        if (!arg[0]) { term_push("rm: missing operand"); }
        else if (unlink(arg) < 0 && rmdir(arg) < 0) term_push("rm: failed");
    }
    else {
        /* Not a builtin -> try an installed program via the same search
         * path the text shell uses. */
        char resolved[128];
        if (!prog_resolve(cmd, resolved, (int)sizeof(resolved))) {
            char msg[COLS + 1];
            snprintf(msg, sizeof msg, "%s: command not found (try help)", cmd);
            term_push(msg);
        } else {
            argv[0] = resolved;         /* argv[0] convention: no .'/' prefix */
            run_external(resolved, argv);
        }
    }
}

/* ---- widgets / events ------------------------------------------------- */

static int want_exit = 0;

static void on_enter(ag_widget_t *w, void *u) {
    (void)u;
    ag_widget_t *box = (w == input) ? w : input;
    run_command(box->text, &want_exit);
    ag_textbox_set(box, "");
    repaint();                          /* show 'exit' echo before leaving */
    /* when run_command set want_exit, the main loop breaks and main()
     * returns -- the kernel destroys the window with its owner. */
}

static void on_run_button(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    on_enter(input, 0);
}

/* Custom loop instead of ag_view_run(): the latter calls ag_view_render()
 * after every event, and that render ag_clear()s the whole window, erasing
 * the scrollback.  Here each event still goes through ag_view_dispatch()
 * (textbox caret/editing, button clicks) but the FINAL paint of every
 * iteration is repaint() — with history drawn on top of the cleared view. */
int main(void) {
    wid = ag_window_create(140, 90, 648, 340, "Terminal", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 8, AG_PANEL);

    term_push("AuraLite GUI Terminal");
    term_push("commands behave like the text shell now - type 'help'");

    input = ag_add_textbox(&view, AREA_X, AREA_Y + VISIBLE * LINE_H + 12,
                           556, 24, "");
    /* A terminal should take keys immediately, but dispatch routes key
     * events only through view.focused_widget (see ag_view_dispatch /
     * set_focus): mirror exactly what set_focus() does on a click. */
    input->focused = 1;
    view.focused_widget = input->id;
    ag_add_button(&view, AREA_X + 564, AREA_Y + VISIBLE * LINE_H + 12,
                  50, 24, "Run", on_run_button, 0);

    repaint();
    for (;;) {
        ag_event_t e;
        if (!ag_poll_event(wid, &e)) {
            for (volatile int i = 0; i < 200000; i++) {}
            continue;
        }
        int quit = ag_view_dispatch(&view, &e);   /* Run-button fires here */
        if (want_exit || quit) break;
        if (e.type == AG_EVT_KEY_DOWN && e.key == '\n' && input->focused)
            on_enter(input, 0);                   /* echoes '$ exit' first */
        if (want_exit) break;
        repaint();
    }
    return 0;
}
