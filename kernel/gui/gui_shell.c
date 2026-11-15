/* kernel/gui/gui_shell.c — start menu, taskbar, desktop icons,
 * notifications.  The window manager, compositor and input router stay
 * in gui.c; this file is the desktop chrome that hangs off them. */

#include <stdint.h>
#include <stddef.h>
#include "kernel/gui/gui.h"
#include "kernel/gui/gui_shell.h"
#include "kernel/gui/taskbar_layout.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/process.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/timer/pit.h"
#include "drivers/mouse/mouse.h"

/* ===================================================================
 * Shell state
 * =================================================================== */

static gui_icon_t         icons[GUI_MAX_ICONS];
static int                icon_selected = -1;
static gui_notification_t notifications[GUI_MAX_NOTIFICATIONS];

static int      start_menu_open  = 0;
static int      taskbar_more_open = 0;
static unsigned taskbar_more_page = 0;

void gui_shell_init(void) {
    memset(icons, 0, sizeof(icons));
    memset(notifications, 0, sizeof(notifications));
    start_menu_open   = 0;
    taskbar_more_open = 0;
    taskbar_more_page = 0;
    icon_selected     = -1;
}

/* ===================================================================
 * Built-in application launcher
 * =================================================================== */

typedef enum {
    GUI_APP_GRP_APPS = 0,
    GUI_APP_GRP_OFFICE,
    GUI_APP_GRP_SYSTEM,
    GUI_APP_GRP_COUNT
} gui_app_group_t;

typedef struct gui_app_entry {
    const char *name;
    const char *path;
    int icon_id;
    gui_app_group_t group;
} gui_app_entry_t;

static const char *const gui_group_titles[GUI_APP_GRP_COUNT] = {
    "Applications",
    "Office",
    "System",
};

static const gui_app_entry_t gui_apps[] = {
    { "Calculator",      "/apps/gcalc",     4,  GUI_APP_GRP_APPS },
    { "Text Editor",     "/apps/gedit",     3,  GUI_APP_GRP_APPS },
    { "File Manager",    "/apps/gfiles",    2,  GUI_APP_GRP_APPS },
    { "Terminal",        "/apps/gterm",     1,  GUI_APP_GRP_APPS },
    { "Web Browser",     "/apps/gbrowser",  9,  GUI_APP_GRP_APPS },
    { "Music Player",    "/apps/gaudio",    8,  GUI_APP_GRP_APPS },
    { "Office",          "/apps/aoffice",  11,  GUI_APP_GRP_OFFICE },
    { "System Monitor",  "/apps/gsysmon",   5,  GUI_APP_GRP_SYSTEM },
    { "Task Manager",    "/apps/gtaskmgr",  7,  GUI_APP_GRP_SYSTEM },
    { "USB Manager",     "/apps/gusb",     10,  GUI_APP_GRP_SYSTEM },
    { "About",           "/apps/gabout",    6,  GUI_APP_GRP_SYSTEM },
};

#define GUI_START_MENU_W     180u
#define GUI_START_MENU_X       4u
#define GUI_START_BUTTON_W   GUI_TB_START_W
#define GUI_START_MENU_ITEM_Y 28u
#define GUI_START_MENU_ITEM_H 18u

static int gui_app_count(void) {
    return (int)(sizeof(gui_apps) / sizeof(gui_apps[0]));
}

static const gui_app_entry_t *gui_app_by_icon_id(int icon_id) {
    for (int i = 0; i < gui_app_count(); i++) {
        if (gui_apps[i].icon_id == icon_id) return &gui_apps[i];
    }
    return NULL;
}

static void gui_launch_app(const gui_app_entry_t *app) {
    if (!app || !app->path) return;
    int64_t pid = process_spawn(app->path);
    if (pid < 0) {
        kprintf("[gui] launch failed: %s (%s)\n", app->name, app->path);
        gui_notify("Launch failed", 0x00C04040, 2500);
    } else {
        kprintf("[gui] launched: %s (%s) pid=%lld\n",
                app->name, app->path, (long long)pid);
    }
}

#define GUI_MENU_MAX_ROWS 64

typedef struct gui_menu_row {
    int app;            /* index into gui_apps[], or -1 for a group header */
    const char *title;
} gui_menu_row_t;

static int gui_menu_rows(gui_menu_row_t *rows, int max) {
    int n = 0;
    for (int g = 0; g < GUI_APP_GRP_COUNT; g++) {
        int first = 1;
        for (int i = 0; i < gui_app_count(); i++) {
            if (gui_apps[i].group != (gui_app_group_t)g) continue;
            if (first) {
                if (n < max) {
                    rows[n].app   = -1;
                    rows[n].title = gui_group_titles[g];
                }
                n++;
                first = 0;
            }
            if (n < max) { rows[n].app = i; rows[n].title = gui_apps[i].name; }
            n++;
        }
    }
    return (n > max) ? max : n;
}

#define GUI_START_MENU_MIN_H  220u
#define GUI_START_MENU_PAD_B    8u

static uint32_t start_menu_height(void) {
    gui_menu_row_t rows[GUI_MENU_MAX_ROWS];
    int n = gui_menu_rows(rows, GUI_MENU_MAX_ROWS);
    uint32_t h = GUI_START_MENU_ITEM_Y +
                 (uint32_t)n * GUI_START_MENU_ITEM_H +
                 GUI_START_MENU_PAD_B;
    return (h < GUI_START_MENU_MIN_H) ? GUI_START_MENU_MIN_H : h;
}

static int start_menu_rect(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    uint32_t fh   = gfx_get_height();
    uint32_t tb_h = gui_get_theme()->taskbar_h;
    uint32_t menu_h = start_menu_height();
    if (fh <= tb_h + menu_h) return 0;
    if (x) *x = (int32_t)GUI_START_MENU_X;
    if (y) *y = (int32_t)(fh - tb_h - menu_h);
    if (w) *w = GUI_START_MENU_W;
    if (h) *h = menu_h;
    return 1;
}

static int start_menu_hit(int32_t mx, int32_t my) {
    if (!start_menu_open) return -1;
    int32_t  menu_x = 0, menu_y = 0;
    uint32_t menu_w = 0, menu_h = 0;
    if (!start_menu_rect(&menu_x, &menu_y, &menu_w, &menu_h)) return -1;
    if (mx < menu_x || mx >= menu_x + (int32_t)menu_w ||
        my < menu_y || my >= menu_y + (int32_t)menu_h) return -1;

    int32_t rel_y = my - menu_y;
    if (rel_y < (int32_t)GUI_START_MENU_ITEM_Y) return -2;
    int row = (rel_y - (int32_t)GUI_START_MENU_ITEM_Y) /
              (int32_t)GUI_START_MENU_ITEM_H;
    gui_menu_row_t rows[GUI_MENU_MAX_ROWS];
    int n = gui_menu_rows(rows, GUI_MENU_MAX_ROWS);
    if (row < 0 || row >= n) return -2;
    return rows[row].app;
}

static int start_menu_contains(int32_t mx, int32_t my) {
    return start_menu_hit(mx, my) != -1;
}

/* ===================================================================
 * Taskbar windows strip
 * =================================================================== */

static int taskbar_hit(int32_t mx, int32_t my) {
    if (gui_presentation_active()) return -1;
    uint32_t fh   = gfx_get_height();
    uint32_t tb_h = gui_get_theme()->taskbar_h;
    if ((uint32_t)my < fh - tb_h) return -1;

    /* Start button. */
    if ((uint32_t)mx >= 4 && (uint32_t)mx < 4 + GUI_START_BUTTON_W &&
        (uint32_t)my >= fh - tb_h + 4) return -2;

    uint32_t fw = gfx_get_width();
    int nwin = gui_taskbar_win_count();
    gui_tb_layout_t tl;
    gui_tb_layout(fw, (unsigned)nwin, &tl);
    for (unsigned slot = 0; slot < tl.visible; slot++) {
        uint32_t bx = 0, btn_w = 0;
        if (!gui_tb_button_rect(fw, (unsigned)nwin, slot, &bx, &btn_w)) break;
        if ((uint32_t)mx >= bx && (uint32_t)mx < bx + btn_w)
            return gui_taskbar_win_at((int)slot);
    }
    if (tl.more_w != 0u &&
        (uint32_t)mx >= tl.more_x &&
        (uint32_t)mx < tl.more_x + tl.more_w) return -3;
    return -1;
}

static void draw_taskbar_overflow(uint32_t fw, uint32_t fh, uint32_t tb_h) {
    if (!taskbar_more_open) return;
    int n = gui_taskbar_win_count();
    gui_tb_overflow_t ov;
    gui_tb_overflow_geometry(fw, fh, tb_h, (unsigned)n, &ov);
    if (ov.rows == 0) return;

    gfx_fill_rect((uint32_t)ov.x, (uint32_t)ov.y, ov.w, ov.h, 0x00282838);
    gfx_draw_rect((uint32_t)ov.x, (uint32_t)ov.y, ov.w, ov.h, 0x0060A0E0);

    gui_tb_layout_t l;
    gui_tb_layout(fw, (unsigned)n, &l);
    uint32_t cols = (ov.w > 20u) ? ((ov.w - 20u) / 8u) : 0u;
    if (cols > 19u) cols = 19u;

    for (unsigned r = 0; r < ov.rows; r++) {
        int entry = gui_tb_overflow_row_entry(&ov, taskbar_more_page, r);
        uint32_t ry = (uint32_t)ov.y + GUI_TB_OV_PAD + r * ov.row_h;
        if (entry < 0) {
            gfx_draw_text((uint32_t)ov.x + 8u, ry, "\xAF more", 0x0070B0D8);
            continue;
        }
        int wid = gui_taskbar_win_at((int)l.visible + entry);
        if (wid < 0) continue;
        char full[GUI_TITLE_MAX];
        gui_get_window_title(wid, full, sizeof(full));
        char title[20];
        uint32_t k = 0;
        while (k < cols && k < sizeof(title) - 1 && full[k]) {
            title[k] = full[k];
            k++;
        }
        title[k] = 0;
        gfx_draw_text((uint32_t)ov.x + 8u, ry, title,
                      gui_get_theme()->taskbar_text);
    }
}

static int taskbar_overflow_hit(int32_t mx, int32_t my) {
    if (!taskbar_more_open) return -1;
    uint32_t fw = gfx_get_width(), fh = gfx_get_height();
    uint32_t tb_h = gui_get_theme()->taskbar_h;
    int n = gui_taskbar_win_count();
    gui_tb_overflow_t ov;
    gui_tb_overflow_geometry(fw, fh, tb_h, (unsigned)n, &ov);
    if (ov.rows == 0) return -1;
    if (mx < ov.x || mx >= ov.x + (int32_t)ov.w) return -1;
    if (my < ov.y || my >= ov.y + (int32_t)ov.h) return -1;
    int32_t rel = my - ov.y - (int32_t)GUI_TB_OV_PAD;
    if (rel < 0) return -1;
    unsigned r = (unsigned)(rel / (int32_t)ov.row_h);
    int entry = gui_tb_overflow_row_entry(&ov, taskbar_more_page, r);
    if (entry < 0) return -3;
    gui_tb_layout_t l;
    gui_tb_layout(fw, (unsigned)n, &l);
    return gui_taskbar_win_at((int)l.visible + entry);
}

void shell_draw_taskbar(void) {
    if (gui_presentation_active()) return;
    const gui_theme_t *t = gui_get_theme();
    uint32_t fw   = gfx_get_width();
    uint32_t fh   = gfx_get_height();
    uint32_t tb_h = t->taskbar_h;
    uint32_t tb_y = fh - tb_h;

    /* Background. */
    gfx_fill_rect(0, tb_y, fw, tb_h, t->taskbar_bg);
    gfx_draw_line(0, tb_y, fw, tb_y, t->taskbar_border);

    /* Start button. */
    gfx_fill_rect(4, tb_y + 4, GUI_START_BUTTON_W, tb_h - 8, t->start_btn_bg);
    gfx_draw_rect(4, tb_y + 4, GUI_START_BUTTON_W, tb_h - 8,
                  start_menu_open ? 0x00FFFFFF : 0x00306090);
    {
        uint32_t fh_inner = gui_font_h();
        uint32_t ty = tb_y + (tb_h > fh_inner ? (tb_h - fh_inner) / 2 : 0);
        gfx_draw_text(12, ty, "AuraLite", t->start_btn_text);
    }

    /* Start menu dropdown. */
    if (start_menu_open) {
        int32_t  menu_x = 0, menu_y = 0;
        uint32_t menu_w = 0, menu_h = 0;
        if (start_menu_rect(&menu_x, &menu_y, &menu_w, &menu_h)) {
            gfx_fill_rect((uint32_t)menu_x, (uint32_t)menu_y, menu_w, menu_h,
                          0x00282838);
            gfx_draw_rect((uint32_t)menu_x, (uint32_t)menu_y, menu_w, menu_h,
                          0x0060A0E0);
            gfx_draw_text((uint32_t)menu_x + 12, (uint32_t)menu_y + 10,
                          "AuraLite", t->taskbar_text);
            gfx_draw_line((uint32_t)menu_x + 8, (uint32_t)menu_y + 22,
                          (uint32_t)menu_x + menu_w - 8, (uint32_t)menu_y + 22,
                          0x00405060);

            gui_menu_row_t rows[GUI_MENU_MAX_ROWS];
            int nrows = gui_menu_rows(rows, GUI_MENU_MAX_ROWS);
            for (int r = 0; r < nrows; r++) {
                uint32_t ay = (uint32_t)menu_y + GUI_START_MENU_ITEM_Y +
                              (uint32_t)r * GUI_START_MENU_ITEM_H;
                if (ay + 16 > tb_y) break;
                if (rows[r].app < 0) {
                    gfx_draw_text((uint32_t)menu_x + 8, ay,
                                  rows[r].title, 0x0070B0D8);
                    gfx_draw_line((uint32_t)menu_x + 8, ay + 15,
                                  (uint32_t)menu_x + menu_w - 8, ay + 15,
                                  0x00384858);
                } else {
                    gfx_draw_text((uint32_t)menu_x + 16, ay,
                                  rows[r].title, t->taskbar_text);
                }
            }
        }
    }

    /* Window buttons. */
    int nwin = gui_taskbar_win_count();
    gui_tb_layout_t tl;
    gui_tb_layout(fw, (unsigned)nwin, &tl);
    uint32_t btn_h = tb_h - 8;
    uint32_t title_cols = (tl.btn_w > 12u) ? ((tl.btn_w - 12u) / 8u) : 0u;
    if (title_cols > 19u) title_cols = 19u;
    for (unsigned slot = 0; slot < tl.visible; slot++) {
        int i = gui_taskbar_win_at((int)slot);
        if (i < 0) break;
        uint32_t bx = 0, btn_w = 0;
        if (!gui_tb_button_rect(fw, (unsigned)nwin, slot, &bx, &btn_w)) break;
        uint32_t col;
        if (i == gui_focused_window())
            col = 0x002F60C0;     /* == t->title_active when focused */
        else if (gui_window_is_minimized(i))
            col = 0x00202830;
        else
            col = 0x00303840;
        gfx_fill_rect(bx, tb_y + 4, btn_w, btn_h, col);
        gfx_draw_rect(bx, tb_y + 4, btn_w, btn_h, 0x00606878);

        char full[GUI_TITLE_MAX];
        gui_get_window_title(i, full, sizeof(full));
        char title_short[20];
        uint32_t n = 0;
        while (n < title_cols && n < sizeof(title_short) - 1 && full[n]) {
            title_short[n] = full[n];
            n++;
        }
        title_short[n] = 0;
        gfx_draw_text(bx + 6,
                      tb_y + (tb_h > gui_font_h() ? (tb_h - gui_font_h()) / 2 : 0),
                      title_short, t->taskbar_text);
    }

    /* "»" button. */
    if (tl.more_w != 0u) {
        gfx_fill_rect(tl.more_x, tb_y + 4, tl.more_w, btn_h,
                      taskbar_more_open ? 0x00405060 : 0x00303840);
        gfx_draw_rect(tl.more_x, tb_y + 4, tl.more_w, btn_h, 0x00606878);
        uint32_t ty = tb_y +
                      (tb_h > gui_font_h() ? (tb_h - gui_font_h()) / 2 : 0);
        gfx_draw_text(tl.more_x +
                      (tl.more_w > 12u ? (tl.more_w - 8u) / 2u : 2u),
                      ty, "\xAF", t->taskbar_text);
    }

    /* Clock. */
    uint32_t tray_x = fw - 90;
    uint64_t ticks = timer_get_ticks();
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    uint64_t s  = ticks / hz;                       /* uptime seconds */
    uint32_t mm = (uint32_t)((s / 60) % 60);
    uint32_t hh = (uint32_t)((s / 3600) % 24);
    uint32_t ss = (uint32_t)(s % 60);
    char clk[16];
    int p = 0;
    clk[p++] = '0' + (hh / 10) % 10; clk[p++] = '0' + hh % 10;
    clk[p++] = ':';                  clk[p++] = '0' + (mm / 10) % 10;
    clk[p++] = '0' + mm % 10;        clk[p++] = ':';
    clk[p++] = '0' + (ss / 10) % 10; clk[p++] = '0' + ss % 10;
    clk[p]   = 0;
    gfx_draw_text(tray_x,
                  tb_y + (tb_h > gui_font_h() ? (tb_h - gui_font_h()) / 2 : 0),
                  clk, t->taskbar_text);

    /* Overflow list last (it overlaps windows above the taskbar). */
    draw_taskbar_overflow(fw, fh, tb_h);
}

/* ===================================================================
 * Desktop icons
 * =================================================================== */

int gui_add_icon(int32_t x, int32_t y, const char *label, int icon_id) {
    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (!icons[i].in_use) {
            icons[i].in_use = 1;
            icons[i].x = x;
            icons[i].y = y;
            strncpy(icons[i].label, label, sizeof(icons[i].label) - 1);
            icons[i].label[sizeof(icons[i].label) - 1] = 0;
            icons[i].icon_id = icon_id;
            tcb_t *owner = sched_current();
            icons[i].owner_pid = owner ? (int)owner->id : 0;
            const gui_theme_t *t = gui_get_theme();
            gui_mark_dirty(x, y,
                           t->icon_size + t->icon_pad * 2,
                           t->icon_size + 20);
            return i;
        }
    }
    return -1;
}

int gui_remove_icon(int icon_idx) {
    if (icon_idx < 0 || icon_idx >= GUI_MAX_ICONS ||
        !icons[icon_idx].in_use) return -1;
    const gui_theme_t *t = gui_get_theme();
    gui_mark_dirty(icons[icon_idx].x, icons[icon_idx].y,
                   t->icon_size + t->icon_pad * 2,
                   t->icon_size + 20);
    memset(&icons[icon_idx], 0, sizeof(icons[icon_idx]));
    if (icon_selected == icon_idx) icon_selected = -1;
    return 0;
}

int gui_icon_owned_by(int icon_idx, uint64_t owner_pid) {
    if (icon_idx < 0 || icon_idx >= GUI_MAX_ICONS ||
        !icons[icon_idx].in_use) return 0;
    return (uint64_t)(uint32_t)icons[icon_idx].owner_pid == owner_pid;
}

int gui_icon_count(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_ICONS; i++) if (icons[i].in_use) n++;
    return n;
}

void shell_cleanup_icons(uint64_t owner_pid) {
    if (owner_pid == 0) return;
    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (icons[i].in_use &&
            (uint64_t)(uint32_t)icons[i].owner_pid == owner_pid) {
            memset(&icons[i], 0, sizeof(icons[i]));
            if (icon_selected == i) icon_selected = -1;
        }
    }
}

void shell_draw_icons(void) {
    const gui_theme_t *t = gui_get_theme();
    uint32_t isz = t->icon_size;

    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (!icons[i].in_use) continue;
        int32_t ix = icons[i].x;
        int32_t iy = icons[i].y;
        int sel = (i == icon_selected);

        if (sel) {
            gfx_fill_rect((uint32_t)ix, (uint32_t)iy, isz, isz,
                          t->icon_selected);
        } else {
            gfx_fill_rect((uint32_t)ix, (uint32_t)iy, isz, isz, 0x00406080);
        }
        gfx_draw_rect((uint32_t)ix, (uint32_t)iy, isz, isz, t->icon_text);

        uint32_t cx = (uint32_t)ix + isz / 2;
        uint32_t cy = (uint32_t)iy + isz / 2;
        gfx_fill_circle(cx, cy, 8, sel ? 0x00FFFFFF : 0x0080B0E0);
        gfx_fill_rect(cx - 3, cy - 3, 7, 7,
                      sel ? t->icon_selected : 0x00406080);

        gfx_draw_text((uint32_t)ix, (uint32_t)iy + isz + 2,
                      icons[i].label, t->icon_text);
    }
}

static int hit_icon(int32_t mx, int32_t my) {
    uint32_t isz = gui_get_theme()->icon_size;
    for (int i = 0; i < GUI_MAX_ICONS; i++) {
        if (!icons[i].in_use) continue;
        if (mx >= icons[i].x && mx < icons[i].x + (int32_t)isz &&
            my >= icons[i].y && my < icons[i].y + (int32_t)isz + 16) {
            return i;
        }
    }
    return -1;
}

/* ===================================================================
 * Notifications
 * =================================================================== */

int gui_notify(const char *text, uint32_t color, uint32_t duration_ms) {
    int slot = -1;
    uint32_t oldest_tick = (uint32_t)-1;
    int oldest_slot = 0;
    for (int i = 0; i < GUI_MAX_NOTIFICATIONS; i++) {
        if (!notifications[i].in_use) { slot = i; break; }
        if (notifications[i].start_tick < oldest_tick) {
            oldest_tick = notifications[i].start_tick;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;

    notifications[slot].in_use = 1;
    strncpy(notifications[slot].text, text,
            sizeof(notifications[slot].text) - 1);
    notifications[slot].text[sizeof(notifications[slot].text) - 1] = 0;
    notifications[slot].color       = color;
    notifications[slot].start_tick  = (uint32_t)timer_get_ticks();
    notifications[slot].duration_ms = duration_ms ? duration_ms : 3000;

    uint32_t fw = gfx_get_width();
    uint32_t fh = gfx_get_height();
    gui_mark_dirty((int32_t)(fw - 320),
                   (int32_t)(fh - gui_get_theme()->taskbar_h - 60),
                   310, 50);
    return slot;
}

void shell_expire_notifications(void) {
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    int expired = 0;
    for (int i = 0; i < GUI_MAX_NOTIFICATIONS; i++) {
        if (!notifications[i].in_use) continue;
        uint32_t elapsed = ((uint32_t)timer_get_ticks() -
                            notifications[i].start_tick) * 1000 / hz;
        if (elapsed >= notifications[i].duration_ms) {
            notifications[i].in_use = 0;
            expired = 1;
        }
    }
    if (expired) gui_request_redraw();
}

void shell_draw_notifications(void) {
    const gui_theme_t *t = gui_get_theme();
    uint32_t fw   = gfx_get_width();
    uint32_t fh   = gfx_get_height();
    uint32_t tb_h = t->taskbar_h;
    uint32_t hz   = timer_get_frequency();
    if (hz == 0) hz = 100;

    int idx = 0;
    for (int i = 0; i < GUI_MAX_NOTIFICATIONS; i++) {
        if (!notifications[i].in_use) continue;
        uint32_t elapsed = ((uint32_t)timer_get_ticks() -
                            notifications[i].start_tick) * 1000 / hz;
        if (elapsed >= notifications[i].duration_ms) {
            notifications[i].in_use = 0;
            continue;
        }
        uint32_t nw = 300, nh = 40;
        uint32_t nx = fw - nw - 8;
        uint32_t ny = fh - tb_h - nh - 8 - (uint32_t)idx * (nh + 6);

        gfx_fill_rect(nx, ny, nw, nh, t->notif_bg);
        gfx_draw_rect(nx, ny, nw, nh, t->notif_border);
        gfx_fill_rect(nx, ny, 4, nh, notifications[i].color);
        gfx_draw_text(nx + 10,
                      ny + (nh > gui_font_h() ? (nh - gui_font_h()) / 2 : 0),
                      notifications[i].text, t->notif_text);
        idx++;
    }
}

/* ===================================================================
 * Interaction handlers
 * =================================================================== */

int shell_handle_start_menu_click(int32_t mx, int32_t my, int wid,
                                  int left_pressed) {
    if (!start_menu_open) return 0;
    int sm_hit = start_menu_hit(mx, my);

    if (left_pressed) {
        if (sm_hit >= 0) {
            gui_launch_app(&gui_apps[sm_hit]);
            start_menu_open = 0;
            gui_request_redraw();
            return 1;
        }
        if (sm_hit == -2) {
            /* Header / empty: consume, keep menu open. */
            return 1;
        }
        if (!(wid < 0 && taskbar_hit(mx, my) == -2)) {
            start_menu_open = 0;
            gui_request_redraw();
            return 1;
        }
        /* Click landed on the Start button: fall through so the taskbar
         * handler toggles the menu closed. */
        return 0;
    } else if (start_menu_contains(mx, my)) {
        gui_set_cursor(GUI_CURSOR_ARROW);
        return 1;
    }
    return 0;
}

int shell_handle_overflow_click(int32_t mx, int32_t my, int left_pressed) {
    if (!taskbar_more_open) return 0;
    int om = taskbar_overflow_hit(mx, my);

    if (left_pressed) {
        if (om >= 0) {
            if (gui_window_is_minimized(om)) gui_restore_window(om);
            else                             gui_focus_window(om);
            taskbar_more_open = 0;
            gui_request_redraw();
            return 1;
        }
        if (om == -3) {
            unsigned n = gui_taskbar_win_count();
            gui_tb_overflow_t ov;
            gui_tb_overflow_geometry(gfx_get_width(), gfx_get_height(),
                                     gui_get_theme()->taskbar_h, n, &ov);
            taskbar_more_page = gui_tb_overflow_next_page(&ov,
                                                         taskbar_more_page);
            gui_request_redraw();
            return 1;
        }
        /* Click outside: close, but let the click reach whatever it
         * landed on (except "»", where closing IS the effect). */
        taskbar_more_open = 0;
        gui_request_redraw();
        if (taskbar_hit(mx, my) == -3) return 1;
        return 0;
    } else if (om != -1) {
        gui_set_cursor(GUI_CURSOR_ARROW);
        return 1;
    }
    return 0;
}

int shell_handle_taskbar_click(int32_t mx, int32_t my, int left_pressed) {
    if (!left_pressed) return 0;
    int tb = taskbar_hit(mx, my);
    if (tb == -2) {
        start_menu_open   = !start_menu_open;
        taskbar_more_open = 0;
        gui_request_redraw();
        gui_set_cursor(GUI_CURSOR_ARROW);
        return 1;
    }
    if (tb == -3) {
        taskbar_more_open = !taskbar_more_open;
        taskbar_more_page = 0;
        start_menu_open   = 0;
        gui_request_redraw();
        return 1;
    }
    if (tb >= 0) {
        if (gui_window_is_minimized(tb))         gui_restore_window(tb);
        else if (gui_focused_window() == tb)     gui_minimize_window(tb);
        else                                     gui_focus_window(tb);
        gui_request_redraw();
        /* Fall through so the icon deselect still runs (matches the
         * pre-split behaviour of route_mouse_event). */
    }
    return 0;
}

int shell_handle_icon_click(int32_t mx, int32_t my, int left_pressed) {
    if (!left_pressed) return 0;
    int ic = hit_icon(mx, my);
    if (ic < 0) {
        icon_selected = -1;
        return 0;
    }
    icon_selected = ic;
    gui_request_redraw();

    uint32_t now = (uint32_t)timer_get_ticks();
    int icon_part = 1000 + ic;
    if (gui_is_double_click(now, mx, my, -1, icon_part, MOUSE_BTN_LEFT)) {
        kprintf("[gui] icon double-clicked: %s (id=%d)\n",
                icons[ic].label, icons[ic].icon_id);
        gui_launch_app(gui_app_by_icon_id(icons[ic].icon_id));
        gui_record_click(0, -999, -999, -999, -1, 0);
    } else {
        gui_record_click(now, mx, my, -1, icon_part, MOUSE_BTN_LEFT);
    }
    return 1;
}