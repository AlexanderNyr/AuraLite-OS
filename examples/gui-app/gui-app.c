/*
 * gui-app.c — a worked AuraGUI application for AuraLite OS.
 *
 * The console example (../hello-app) shows the build; this one shows the
 * shape of a GUI program, which is different enough to be worth its own
 * example: a window, a view over a widget array, and an event loop that the
 * toolkit runs for you.
 *
 * Built by `make sdk-check` against the staged SDK, like every example here.
 */

#include "auragui.h"
#include "stdio.h"

/* The widget array is supplied by the application, not allocated by the
 * toolkit. It must outlive the view — a local in main() is fine, a local in
 * a helper function is a dangling pointer the moment that function returns. */
static ag_widget_t widgets[8];
static ag_view_t   view;
static ag_widget_t *readout;

static int clicks;

static void on_click(ag_widget_t *w, void *user) {
    (void)w; (void)user;
    clicks++;

    char text[64];
    snprintf(text, sizeof(text), "clicked %d time%s",
             clicks, clicks == 1 ? "" : "s");
    ag_textbox_set(readout, text);

    /* Ask the window manager to repaint. An ordinary application must NOT
     * call the render op directly: the kernel restricts that to PID <= 2, so
     * a third-party program that tries it simply gets nothing. */
    ag_window_invalidate(view.wid);

    printf("GUIAPP: click %d\n", clicks);
    fflush(stdout);
}

int main(void) {
    int wid = ag_window_create(200, 150, 320, 180, "SDK Example",
                               AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE |
                               AG_WIN_MOVABLE);
    if (wid < 0) {
        printf("GUIAPP: no window (is the GUI running?)\n");
        fflush(stdout);
        return 1;
    }
    ag_window_show(wid);

    ag_view_init(&view, wid, widgets, 8, AG_PANEL);
    ag_add_label(&view, 20, 16, "Built with the AuraLite SDK", AG_ACCENT);
    readout = ag_add_textbox(&view, 20, 44, 280, 24, "no clicks yet");
    ag_add_button(&view, 20, 84, 120, 30, "Click me", on_click, 0);

    printf("GUIAPP: window %d created\n", wid);
    fflush(stdout);

    /* Runs until the window is closed. */
    ag_view_run(&view, 0, 0);

    printf("GUIAPP: exiting after %d click(s)\n", clicks);
    fflush(stdout);
    return 0;
}
