/* gsysmon — graphical system monitor with animated progress bars. */
#include "auragui.h"
#include "unistd.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[16];
static ag_view_t view;
static ag_widget_t *cpu_bar, *mem_bar, *net_bar, *disk_bar;
static ag_widget_t *cpu_lbl, *mem_lbl, *net_lbl, *disk_lbl;

static uint32_t rng_state = 0xC0FFEE;
static int      pseudo_rand(int max) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (int)((rng_state >> 16) % (uint32_t)max);
}

int main(void) {
    wid = ag_window_create(380, 70, 360, 260, "System Monitor", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 16, AG_PANEL);

    ag_add_label(&view, 12,  14, "AuraLite System Monitor", AG_DARK);
    cpu_lbl  = ag_add_label(&view, 12,  40, "CPU:   ", AG_BLACK);
    cpu_bar  = ag_add_progress(&view, 90, 36, 250, 100, 18);
    mem_lbl  = ag_add_label(&view, 12,  74, "Mem:   ", AG_BLACK);
    mem_bar  = ag_add_progress(&view, 90, 70, 250, 100, 42);
    net_lbl  = ag_add_label(&view, 12, 108, "Net:   ", AG_BLACK);
    net_bar  = ag_add_progress(&view, 90, 104, 250, 100, 5);
    disk_lbl = ag_add_label(&view, 12, 142, "Disk:  ", AG_BLACK);
    disk_bar = ag_add_progress(&view, 90, 138, 250, 100, 88);

    ag_add_label(&view, 12, 180, "Processes:", AG_DARK);
    ag_add_label(&view, 12, 200, " 1 init/shell", AG_BLACK);
    ag_add_label(&view, 12, 214, " 2 gui-compositor", AG_BLACK);
    ag_add_label(&view, 12, 228, " 3 idle", AG_BLACK);

    ag_view_render(&view);

    /* Animate forever (until window is closed via close button). */
    for (;;) {
        ag_event_t e;
        /* Pull all pending events. */
        while (ag_poll_event(wid, &e)) {
            if (ag_view_dispatch(&view, &e)) { ag_window_destroy(wid); return 0; }
        }
        cpu_bar->value  = pseudo_rand(100);
        mem_bar->value  = 30 + pseudo_rand(50);
        net_bar->value  = pseudo_rand(30);
        disk_bar->value = 60 + pseudo_rand(30);
        ag_view_render(&view);
        /* Cheap busy-sleep without timer_ms helper. */
        for (volatile int i = 0; i < 2000000; i++) {}
    }
    return 0;
}
