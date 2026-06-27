/* gtaskmgr — GUI Task Manager reading live data from /proc. */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[32];
static ag_view_t view;
static ag_widget_t *mem_lbl;
static ag_widget_t *proc_lbls[16];
static char mem_text[128];
static char proc_texts[16][64];

static char *find_char(char *s, char c) {
    while (*s) {
        if (*s == c) return s;
        s++;
    }
    return 0;
}

static void update_proc_data(void) {
    /* Read memory info */
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        int64_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            /* Grab first line or two */
            char *nl = find_char(buf, '\n');
            if (nl) {
                char *nl2 = find_char(nl + 1, '\n');
                if (nl2) *nl2 = '\0';
            }
            strncpy(mem_text, buf, sizeof(mem_text) - 1);
            strcpy(mem_lbl->text, mem_text);
        }
        close(fd);
    }

    /* Read process list */
    int count = 0;
    for (int pid = 1; pid < 32 && count < 15; pid++) {
        char path[64];
        char name[64];
        /* Simple itoa */
        int p = pid, len = 0;
        char tmp[16];
        while (p > 0) { tmp[len++] = '0' + (p % 10); p /= 10; }
        strcpy(path, "/proc/");
        int off = 6;
        while (len > 0) path[off++] = tmp[--len];
        path[off] = '\0';
        strcat(path, "/cmdline");

        int fdp = open(path, O_RDONLY);
        if (fdp >= 0) {
            int64_t n = read(fdp, name, sizeof(name) - 1);
            if (n > 0) {
                name[n] = '\0';
                if (n > 0 && name[n-1] == '\n') name[n-1] = '\0';
                /* Format label */
                char *buf = proc_texts[count];
                strcpy(buf, "PID ");
                /* append pid */
                int p2 = pid, l2 = 0;
                char tmp2[16];
                while (p2 > 0) { tmp2[l2++] = '0' + (p2 % 10); p2 /= 10; }
                int ob = 4;
                while (l2 > 0) buf[ob++] = tmp2[--l2];
                buf[ob] = '\0';
                strcat(buf, " :  ");
                strcat(buf, name);
                strcpy(proc_lbls[count]->text, buf);
                count++;
            }
            close(fdp);
        }
    }
    while (count < 15) {
        strcpy(proc_lbls[count]->text, "");
        count++;
    }
}

static void on_refresh(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    update_proc_data();
    ag_view_render(&view);
}

int main(void) {
    wid = ag_window_create(100, 100, 360, 420, "Task Manager", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 32, AG_PANEL);

    ag_add_label(&view, 16, 16, "AuraLite GUI Task Manager", AG_ACCENT);
    strcpy(mem_text, "Loading memory info...");
    mem_lbl = ag_add_label(&view, 16, 40, mem_text, AG_DARK);

    ag_add_label(&view, 16, 75, "Active Processes (/proc):", AG_BLACK);

    for (int i = 0; i < 15; i++) {
        strcpy(proc_texts[i], "");
        proc_lbls[i] = ag_add_label(&view, 24, 100 + i * 18, proc_texts[i], AG_BLACK);
    }

    ag_add_button(&view, 130, 380, 100, 28, "Refresh", on_refresh, 0);

    update_proc_data();
    ag_view_render(&view);

    ag_view_run(&view, 0, 0);
    return 0;
}
