/* gedit — minimal GUI text editor.
 *
 * Single-line text storage in textbox.  File save/load through VFS to /tmp.
 */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[12];
static ag_view_t view;
static ag_widget_t *path_box, *content_box, *status;

static void on_save(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    int fd = open(path_box->text, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { ag_textbox_set(status, "save: open failed"); return; }
    int len = (int)strlen(content_box->text);
    if (write(fd, content_box->text, (size_t)len) != len) {
        ag_textbox_set(status, "save: short write");
    } else {
        ag_textbox_set(status, "saved");
    }
    close(fd);
}

static void on_load(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    int fd = open(path_box->text, O_RDONLY);
    if (fd < 0) { ag_textbox_set(status, "load: open failed"); return; }
    char buf[AG_MAX_WIDGET_TEXT];
    int64_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) { ag_textbox_set(status, "load: read failed"); return; }
    buf[n] = 0;
    /* Strip trailing newline. */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    ag_textbox_set(content_box, buf);
    ag_textbox_set(status, "loaded");
}

int main(void) {
    wid = ag_window_create(120, 90, 480, 280, "Text Editor", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 12, AG_PANEL);

    ag_add_label  (&view, 12, 14, "File:", AG_BLACK);
    path_box    = ag_add_textbox(&view, 60, 8, 280, 24, "/tmp/notes.txt");
    ag_add_button (&view, 350, 8, 56, 24, "Load", on_load, 0);
    ag_add_button (&view, 412, 8, 56, 24, "Save", on_save, 0);

    ag_add_label  (&view, 12, 48, "Content:", AG_BLACK);
    content_box = ag_add_textbox(&view, 12, 64, 456, 160, "");

    ag_add_label  (&view, 12, 234, "Status:", AG_BLACK);
    status      = ag_add_textbox(&view, 60, 230, 408, 24, "ready");

    ag_view_run(&view, 0, 0);
    return 0;
}
