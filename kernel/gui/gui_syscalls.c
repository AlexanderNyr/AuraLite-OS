/* gui_syscalls.c — dispatch GUI v2.0 ops from user space.
 *
 * User pointers are copied/validated here rather than dereferenced directly.
 */

#include <stdint.h>
#include "kernel/gui/gui.h"
#include "kernel/gui/gui_syscalls.h"
#include "kernel/proc/usercopy.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/string.h"

#define GUI_USER_TEXT_MAX 512

static char gui_kernel_clipboard[GUI_USER_TEXT_MAX] = {0};

/* Helpers to pack/unpack two int32 in one uint64. */
static inline int32_t  lo32(uint64_t v) { return (int32_t)(v & 0xFFFFFFFFu); }
static inline int32_t  hi32(uint64_t v) { return (int32_t)(v >> 32); }

static int copy_gui_title(char *dst, uint64_t user_title) {
    if (!user_title) {
        dst[0] = 0;
        return 0;
    }
    return copy_string_from_user(dst, (const char *)(uintptr_t)user_title,
                                 GUI_TITLE_MAX);
}

static uint64_t current_pid(void) {
    tcb_t *cur = sched_current();
    return cur ? cur->id : 0;
}

static int require_owner(int wid) {
    uint64_t pid = current_pid();
    return gui_window_owned_by(wid, pid);
}

uint64_t syscall_gui_call(uint64_t op, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
    switch (op) {
    case GUI_OP_CREATE: {
        int32_t x = lo32(a2), y = hi32(a2);
        uint32_t w = (uint32_t)lo32(a3), h = (uint32_t)hi32(a3);
        char title[GUI_TITLE_MAX];
        if (copy_gui_title(title, a4) != 0) return (uint64_t)-1;
        uint32_t flags = (uint32_t)a5;
        return (uint64_t)gui_create_window(x, y, w, h, title, flags);
    }
    case GUI_OP_DESTROY:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_destroy_window((int)a2);
    case GUI_OP_SHOW:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_show_window((int)a2);
    case GUI_OP_HIDE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_hide_window((int)a2);
    case GUI_OP_MOVE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_move_window((int)a2, lo32(a3), hi32(a3));
    case GUI_OP_RESIZE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_resize_window((int)a2,
                                            (uint32_t)lo32(a3),
                                            (uint32_t)hi32(a3));
    case GUI_OP_TITLE: {
        if (!require_owner((int)a2)) return (uint64_t)-1;
        char title[GUI_TITLE_MAX];
        if (copy_gui_title(title, a3) != 0) return (uint64_t)-1;
        return (uint64_t)gui_set_title((int)a2, title);
    }
    case GUI_OP_FOCUS:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_focus_window((int)a2);
    case GUI_OP_MINIMIZE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_minimize_window((int)a2);
    case GUI_OP_MAXIMIZE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_maximize_window((int)a2);
    case GUI_OP_RESTORE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_restore_window((int)a2);
    case GUI_OP_SNAP:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_snap_window((int)a2, (gui_snap_t)a3);
    case GUI_OP_CLEAR:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_clear((int)a2, (uint32_t)a3);
    case GUI_OP_FILL_RECT:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_fill_rect((int)a2, lo32(a3), hi32(a3),
                                       (uint32_t)lo32(a4), (uint32_t)hi32(a4),
                                       (uint32_t)a5);
    case GUI_OP_DRAW_RECT:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_draw_rect((int)a2, lo32(a3), hi32(a3),
                                       (uint32_t)lo32(a4), (uint32_t)hi32(a4),
                                       (uint32_t)a5);
    case GUI_OP_DRAW_LINE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_draw_line((int)a2, lo32(a3), hi32(a3),
                                        lo32(a4), hi32(a4), (uint32_t)a5);
    case GUI_OP_DRAW_TEXT: {
        if (!require_owner((int)a2)) return (uint64_t)-1;
        char text[GUI_USER_TEXT_MAX];
        if (copy_string_from_user(text, (const char *)(uintptr_t)a4,
                                  sizeof(text)) != 0) {
            return (uint64_t)-1;
        }
        return (uint64_t)gui_draw_text((int)a2, lo32(a3), hi32(a3),
                                        text, (uint32_t)a5);
    }
    case GUI_OP_DRAW_PIXEL:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_draw_pixel((int)a2, lo32(a3), hi32(a3),
                                         (uint32_t)a4);
    case GUI_OP_INVALIDATE:
        if (!require_owner((int)a2)) return (uint64_t)-1;
        return (uint64_t)gui_invalidate_window((int)a2);
    case GUI_OP_RENDER:
        if (current_pid() > 2) return (uint64_t)-1;
        gui_render_now(); return 0;
    case GUI_OP_SET_CURSOR:
        if (current_pid() > 2) return (uint64_t)-1;
        gui_set_cursor((gui_cursor_t)a2); return 0;
    case GUI_OP_GET_SIZE: {
        if (!require_owner((int)a2)) return (uint64_t)-1;
        uint32_t wh[2];
        if (gui_get_window_size((int)a2, &wh[0], &wh[1]) != 0) return (uint64_t)-1;
        if (copy_to_user((void *)(uintptr_t)a3, wh, sizeof(wh)) != 0) {
            return (uint64_t)-1;
        }
        return 0;
    }
    case GUI_OP_GET_POS: {
        if (!require_owner((int)a2)) return (uint64_t)-1;
        int32_t xy[2];
        if (gui_get_window_pos((int)a2, &xy[0], &xy[1]) != 0) return (uint64_t)-1;
        if (copy_to_user((void *)(uintptr_t)a3, xy, sizeof(xy)) != 0) {
            return (uint64_t)-1;
        }
        return 0;
    }
    case GUI_OP_GET_RECT: {
        if (!require_owner((int)a2)) return (uint64_t)-1;
        int32_t x, y; uint32_t w, h;
        if (gui_get_window_rect((int)a2, &x, &y, &w, &h) != 0) return (uint64_t)-1;
        /* Pack: a3 = user pointer to struct { i32 x, y; u32 w, h; } */
        uint32_t rect[4] = { (uint32_t)x, (uint32_t)y, w, h };
        if (copy_to_user((void *)(uintptr_t)a3, rect, sizeof(rect)) != 0) return (uint64_t)-1;
        return 0;
    }
    case GUI_OP_SET_CLIPBOARD: {
        if (!a2) return (uint64_t)-1;
        if (copy_string_from_user(gui_kernel_clipboard, (const char *)(uintptr_t)a2, GUI_USER_TEXT_MAX) != 0) {
            gui_kernel_clipboard[0] = 0;
            return (uint64_t)-1;
        }
        return 0;
    }
    case GUI_OP_GET_CLIPBOARD: {
        if (!a2 || a3 == 0) return (uint64_t)-1;
        uint64_t len = strlen(gui_kernel_clipboard) + 1;
        if (len > a3) len = a3;
        if (copy_to_user((void *)(uintptr_t)a2, gui_kernel_clipboard, len) != 0) return (uint64_t)-1;
        char null_byte = 0;
        copy_to_user((void *)((uintptr_t)a2 + len - 1), &null_byte, 1);
        return 0;
    }
    case GUI_OP_ADD_ICON: {
        char label[32];
        if (copy_string_from_user(label, (const char *)(uintptr_t)a3, sizeof(label)) != 0) return (uint64_t)-1;
        return (uint64_t)gui_add_icon(lo32(a2), hi32(a2), label, (int)a4);
    }
    case GUI_OP_REMOVE_ICON:
        return (uint64_t)gui_remove_icon((int)a2);
    case GUI_OP_NOTIFY: {
        char text[128];
        if (copy_string_from_user(text, (const char *)(uintptr_t)a2, sizeof(text)) != 0) return (uint64_t)-1;
        return (uint64_t)gui_notify(text, (uint32_t)a3, (uint32_t)a4);
    }
    case GUI_OP_GET_FLAGS:
        return (uint64_t)gui_get_window_flags((int)a2);
    }
    return (uint64_t)-1;
}



uint64_t syscall_gui_event(uint64_t wid, uint64_t user_evt, uint64_t blocking) {
    gui_event_t evt;
    int r;
    if (!user_evt) return (uint64_t)-1;
    if (!require_owner((int)wid)) return (uint64_t)-1;
    if (!validate_user_range((void *)(uintptr_t)user_evt, sizeof(evt), 1)) {
        return (uint64_t)-1;
    }
    if (blocking) r = gui_wait_event((int)wid, &evt);
    else          r = gui_poll_event((int)wid, &evt);
    if (r <= 0) return (uint64_t)r;
    if (copy_to_user((void *)(uintptr_t)user_evt, &evt, sizeof(evt)) != 0) {
        return (uint64_t)-1;
    }
    return (uint64_t)r;
}

uint64_t syscall_gui_theme(uint64_t subop, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    switch (subop) {
    case 0: /* Get theme struct size. */
        return (uint64_t)sizeof(gui_theme_t);
    case 1: { /* Get current theme. */
        const gui_theme_t *t = gui_get_theme();
        if (!a2) return (uint64_t)-1;
        if (!validate_user_range((void *)(uintptr_t)a2, sizeof(gui_theme_t), 1))
            return (uint64_t)-1;
        if (copy_to_user((void *)(uintptr_t)a2, t, sizeof(gui_theme_t)) != 0)
            return (uint64_t)-1;
        return 0;
    }
    case 2: { /* Set theme. */
        gui_theme_t t;
        if (!a2) return (uint64_t)-1;
        if (copy_from_user(&t, (const void *)(uintptr_t)a2, sizeof(t)) != 0)
            return (uint64_t)-1;
        gui_set_theme(&t);
        return 0;
    }
    }
    return (uint64_t)-1;
}
