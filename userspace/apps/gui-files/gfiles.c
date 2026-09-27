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
static int drag_armed, drag_moved, drag_x, drag_y;
static char drag_path[AG_DROP_PATH_MAX];

/* Fixed names list backing the listbox.  We store entries as a flat buffer of
 * concatenated NUL-terminated names so the listbox's `items[]` (char*) array
 * can point into it. */
static char names_buf[8192];
static char *names_ptr[AG_MAX_LIST_ITEMS];

static void load_dir(const char *path) {
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

    /* A file preview must not destroy the list selection while dragging. */
    ag_listbox_clear(list);
    /* Directory — populate listbox via readdir. */
    int np = 0;
    int bp = 0;
    
    struct aura_dirent ents[128];
    int nents = aura_readdir(path, ents, 128);
    
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

/* Bounded absolute path of the currently selected entry, not the path box's
 * mutable text. A file preview leaves the directory/list selection intact. */
static int selected_path(char *out, size_t cap) {
    if (!list || list->selected < 0 || list->selected >= list->item_count)
        return -1;
    const char *base = path_box->text;
    const char *name = list->items[list->selected];
    if (!base || base[0] != '/' || !name || strchr(name, '/')) return -1;
    size_t n = strlen(base), m = strlen(name);
    size_t slash = (n > 0 && base[n - 1] == '/') ? 0 : 1;
    if (!m || n + slash + m >= cap) return -1;
    memcpy(out, base, n);
    if (slash) out[n++] = '/';
    memcpy(out + n, name, m + 1);
    return 0;
}

static void on_select(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    char path[AG_DROP_PATH_MAX];
    if (selected_path(path, sizeof path) != 0) return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (st.st_type != ST_TYPE_FILE) ag_textbox_set(path_box, path);
    load_dir(path);
}

/* USER32 consumers receive WM_DROPFILES only when they opted into
 * WS_EX_ACCEPTFILES. This native file manager supplies the real absolute path
 * on release over a DIFFERENT compositor window's client area. Capture keeps
 * the sender receiving the release, but drop hit-testing ignores capture. */
static int on_drag(ag_view_t *v, const ag_event_t *e, void *user) {
    (void)v; (void)user;
    if (e->type == AG_EVT_MOUSE_DOWN &&
        e->x >= list->x && e->x < list->x + (int32_t)list->w &&
        e->y >= list->y && e->y < list->y + (int32_t)list->h) {
        struct stat st;
        drag_armed = selected_path(drag_path, sizeof drag_path) == 0 &&
                     stat(drag_path, &st) == 0 && st.st_type == ST_TYPE_FILE;
        drag_moved = 0; drag_x = e->x; drag_y = e->y;
        if (drag_armed) ag_window_capture(wid);
    }
    if (drag_armed && e->type == AG_EVT_MOUSE_MOVE) {
        int dx = e->x - drag_x, dy = e->y - drag_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy >= 6) drag_moved = 1;
    }
    if (drag_armed && e->type == AG_EVT_MOUSE_UP) {
        if (drag_moved) {
            int sent = ag_send_file_drop(wid, drag_path);
            ag_textbox_set(status, sent == 0 ? "File delivered" :
                                            "No accepting window under pointer");
        }
        ag_window_capture(-1);
        drag_armed = drag_moved = 0;
    }
    if (drag_armed && e->type == AG_EVT_KEY_DOWN && e->key == 27) {
        ag_window_capture(-1);
        drag_armed = drag_moved = 0;
    }
    return 0;
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
    ag_view_run(&view, on_drag, 0);
    return 0;
}
