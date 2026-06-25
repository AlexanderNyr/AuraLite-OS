/* gtheme — GUI Theme Manager. */
#include "auragui.h"
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static int wid;
static ag_widget_t widgets[16];
static ag_view_t view;
static ag_widget_t *hex_box;
static ag_widget_t *status;

static void on_apply(ag_widget_t *w, void *user) {
    (void)w; (void)user;
    int fd = open("/disk/theme.txt");
    if (fd < 0) {
        ag_textbox_set(status, "Failed to open /disk/theme.txt");
        return;
    }
    const char *h = hex_box->text;
    write(fd, h, strlen(h));
    close(fd);
    ag_textbox_set(status, "Saved. Reboot/restart app to apply.");
}

int main(void) {
    wid = ag_window_create(100, 100, 320, 200, "Theme Manager", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    
    ag_view_init(&view, wid, widgets, 16, AG_PANEL);

    ag_add_label (&view, 20, 24, "Hex Color (0x00RRGGBB):", AG_BLACK);
    hex_box = ag_add_textbox(&view, 20, 44, 200, 24, "0x00ECEDF1");
    ag_add_button(&view, 230, 44, 70, 24, "Apply", on_apply, 0);

    status = ag_add_textbox(&view, 20, 140, 280, 24, "Ready");

    ag_view_run(&view, 0, 0);
    return 0;
}
