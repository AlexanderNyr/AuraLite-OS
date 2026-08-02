/* gaudio — Graphical music player with animated visualizer. */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[32];
static ag_view_t view;
static ag_widget_t *status_lbl;
static ag_widget_t *vis_bars[8];

static uint32_t rng_state = 0x8BADF00D;
static int pseudo_rand(int max) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (int)((rng_state >> 16) % (uint32_t)max);
}

static void on_play_starwars(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    strcpy(status_lbl->text, "Playing: Star Wars Theme");
    ag_view_render(&view);
    int fd = open("/dev/audio", O_WRONLY);
    if (fd >= 0) {
        write(fd, "BEEP 440 500", 12);
        close(fd);
    }
}

static void on_play_ode(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    strcpy(status_lbl->text, "Playing: Ode to Joy");
    ag_view_render(&view);
    int fd = open("/dev/audio", O_WRONLY);
    if (fd >= 0) {
        write(fd, "BEEP 330 300", 12);
        close(fd);
    }
}

static void on_stop(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    strcpy(status_lbl->text, "Status: Stopped");
    ag_view_render(&view);
    int fd = open("/dev/audio", O_WRONLY);
    if (fd >= 0) {
        write(fd, "BEEP 0 10", 9);
        close(fd);
    }
}

int main(void) {
    wid = ag_window_create(150, 120, 340, 320, "AuraLite Music Player", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 32, AG_PANEL);

    ag_add_label(&view, 16, 16, "AuraLite Audio Player", AG_ACCENT);
    status_lbl = ag_add_label(&view, 16, 40, "Status: Ready", AG_DARK);

    ag_add_button(&view, 16, 70,  140, 28, "Star Wars", on_play_starwars, 0);
    ag_add_button(&view, 170, 70, 140, 28, "Ode to Joy", on_play_ode, 0);
    ag_add_button(&view, 16, 105, 140, 28, "Stop", on_stop, 0);

    ag_add_label(&view, 16, 150, "Visualizer / Equalizer:", AG_BLACK);

    for (int i = 0; i < 8; i++) {
        vis_bars[i] = ag_add_progress(&view, 16 + i * 38, 180, 32, 100, 10 + i * 10);
    }

    ag_view_render(&view);

    /* Animate visualizer loop */
    for (;;) {
        ag_event_t e;
        while (ag_poll_event(wid, &e)) {
            if (ag_view_dispatch(&view, &e)) {
                ag_window_destroy(wid);
                return 0;
            }
        }
        if (strcmp(status_lbl->text, "Status: Stopped") != 0 && strcmp(status_lbl->text, "Status: Ready") != 0) {
            for (int i = 0; i < 8; i++) {
                vis_bars[i]->value = 10 + pseudo_rand(80);
            }
            ag_view_render(&view);
        }
        for (volatile int s = 0; s < 2000000; s++) {}
    }
    return 0;
}
