/* gfiles — GUI file manager. */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[16];
static ag_view_t view;
static ag_widget_t *path_box, *list, *content_view, *status;

/* Fixed names list backing the listbox.  We store entries as a flat buffer of
 * concatenated NUL-terminated names so the listbox's `items[]` (char*) array
 * can point into it. */
static char names_buf[8192];
static char *names_ptr[AG_MAX_LIST_ITEMS];

static void load_dir(const char *path) {
    ag_listbox_clear(list);
    /* AuraLite's VFS exposes readdir via SYS_STAT? Actually via separate
     * syscall.  For simplicity we re-use the legacy `listdir` (prints to
     * console) — and additionally try opening the path as a file to show
     * contents in content_view if it's not a directory. */
    /* Try stat to determine type. */
    struct stat st;
    if (stat(path, &st) != 0) {
        ag_textbox_set(status, "stat failed");
        return;
    }
    if (st.st_type == ST_TYPE_FILE) {
        /* It's a file — read its head into content_view. */
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[AG_MAX_WIDGET_TEXT];
            int64_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = 0;
                /* truncate control chars to spaces */
                for (int i = 0; i < n; i++) if (buf[i] < 0x20 && buf[i] != '\n') buf[i] = ' ';
                ag_textbox_set(content_view, buf);
            } else {
                ag_textbox_set(content_view, "(empty)");
            }
            char st_str[64];
            int p = 0;
            const char *pfx = "file ";
            while (*pfx) st_str[p++] = *pfx++;
            uint64_t sz = st.st_size;
            char num[24]; int np = 0;
            if (sz == 0) num[np++] = '0';
            while (sz) { num[np++] = '0' + (sz % 10); sz /= 10; }
            while (np-- > 0) st_str[p++] = num[np];
            const char *suf = " bytes";
            while (*suf) st_str[p++] = *suf++;
            st_str[p] = 0;
            ag_textbox_set(status, st_str);
        }
        return;
    }

    /* Directory — populate listbox via readdir. */
    int np = 0;
    int bp = 0;
    
    struct dirent ents[128];
    int nents = readdir(path, ents, 128);
    
    if (nents > 0) {
        for (int i = 0; i < nents && np < AG_MAX_LIST_ITEMS; i++) {
            int l = (int)strlen(ents[i].name);
            if (bp + l + 1 >= (int)sizeof(names_buf)) break;
            names_ptr[np] = &names_buf[bp];
            memcpy(&names_buf[bp], ents[i].name, (size_t)l);
            names_buf[bp + l] = 0;
            ag_listbox_add(list, names_ptr[np]);
            bp += l + 1;
            np++;
        }
    } else {
        ag_textbox_set(status, "directory empty or read failed");
    }
    
    char st_str[40];
    int p = 0;
    int n = list->item_count;
    char num[12]; int nn = 0;
    if (n == 0) num[nn++] = '0';
    while (n) { num[nn++] = '0' + (n % 10); n /= 10; }
    while (nn-- > 0) st_str[p++] = num[nn];
    const char *suf = " entries";
    while (*suf) st_str[p++] = *suf++;
    st_str[p] = 0;
    ag_textbox_set(status, st_str);
    ag_textbox_set(content_view, "(directory)");
}

static void on_open(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    load_dir(path_box->text);
}

static void on_select(ag_widget_t *w, void *u) {
    (void)u;
    if (w->selected < 0 || w->selected >= w->item_count) return;
    /* Build path = base "/" name. */
    char p[256];
    int n = 0;
    const char *base = path_box->text;
    while (base[n]) { p[n] = base[n]; n++; }
    if (n > 0 && p[n-1] != '/') p[n++] = '/';
    const char *nm = w->items[w->selected];
    while (*nm) { p[n++] = *nm++; }
    p[n] = 0;
    ag_textbox_set(path_box, p);
    load_dir(p);
}

int main(void) {
    wid = ag_window_create(80, 80, 540, 320, "File Manager", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 16, AG_PANEL);

    ag_add_label (&view, 12, 14, "Path:", AG_BLACK);
    path_box = ag_add_textbox(&view, 60, 8,  370, 24, "/");
    ag_add_button(&view, 436, 8, 90, 24, "Open", on_open, 0);

    list = ag_add_listbox(&view, 12, 44, 200, 200);
    list->on_select = on_select;

    ag_add_label (&view, 222, 44, "Preview:", AG_BLACK);
    content_view = ag_add_textbox(&view, 222, 64, 304, 180, "(select a file)");

    ag_add_label (&view, 12, 254, "Status:", AG_BLACK);
    status = ag_add_textbox(&view, 60, 250, 466, 24, "ready");

    load_dir("/");
    ag_view_run(&view, 0, 0);
    return 0;
}
