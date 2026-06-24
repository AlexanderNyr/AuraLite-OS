/* gui_syscalls.c — dispatch GUI ops from user space.
 *
 * For now user pointers are dereferenced directly (the project ships without
 * pointer validation — see docs/syscall_abi.md "TODO").  When a copy_from_user
 * helper lands these handlers should switch to it.
 */

#include <stdint.h>
#include "kernel/gui/gui.h"
#include "kernel/gui/gui_syscalls.h"
#include "kernel/lib/string.h"

/* Helpers to pack/unpack two int32 in one uint64. */
static inline int32_t  lo32(uint64_t v) { return (int32_t)(v & 0xFFFFFFFFu); }
static inline int32_t  hi32(uint64_t v) { return (int32_t)(v >> 32); }

uint64_t syscall_gui_call(uint64_t op, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
    switch (op) {
    case GUI_OP_CREATE: {
        int32_t x = lo32(a2), y = hi32(a2);
        uint32_t w = (uint32_t)lo32(a3), h = (uint32_t)hi32(a3);
        const char *title = (const char *)a4;
        uint32_t flags = (uint32_t)a5;
        return (uint64_t)gui_create_window(x, y, w, h, title, flags);
    }
    case GUI_OP_DESTROY:  return (uint64_t)gui_destroy_window((int)a2);
    case GUI_OP_SHOW:     return (uint64_t)gui_show_window((int)a2);
    case GUI_OP_HIDE:     return (uint64_t)gui_hide_window((int)a2);
    case GUI_OP_MOVE:     return (uint64_t)gui_move_window((int)a2, lo32(a3), hi32(a3));
    case GUI_OP_RESIZE:   return (uint64_t)gui_resize_window((int)a2,
                                                            (uint32_t)lo32(a3),
                                                            (uint32_t)hi32(a3));
    case GUI_OP_TITLE:    return (uint64_t)gui_set_title((int)a2, (const char *)a3);
    case GUI_OP_FOCUS:    return (uint64_t)gui_focus_window((int)a2);
    case GUI_OP_MINIMIZE: return (uint64_t)gui_minimize_window((int)a2);
    case GUI_OP_MAXIMIZE: return (uint64_t)gui_maximize_window((int)a2);
    case GUI_OP_RESTORE:  return (uint64_t)gui_restore_window((int)a2);
    case GUI_OP_CLEAR:    return (uint64_t)gui_clear((int)a2, (uint32_t)a3);
    case GUI_OP_FILL_RECT:
        return (uint64_t)gui_fill_rect((int)a2, lo32(a3), hi32(a3),
                                       (uint32_t)lo32(a4), (uint32_t)hi32(a4),
                                       (uint32_t)a5);
    case GUI_OP_DRAW_RECT:
        return (uint64_t)gui_draw_rect((int)a2, lo32(a3), hi32(a3),
                                       (uint32_t)lo32(a4), (uint32_t)hi32(a4),
                                       (uint32_t)a5);
    case GUI_OP_DRAW_LINE:
        return (uint64_t)gui_draw_line((int)a2, lo32(a3), hi32(a3),
                                        lo32(a4), hi32(a4), (uint32_t)a5);
    case GUI_OP_DRAW_TEXT:
        return (uint64_t)gui_draw_text((int)a2, lo32(a3), hi32(a3),
                                        (const char *)a4, (uint32_t)a5);
    case GUI_OP_DRAW_PIXEL:
        return (uint64_t)gui_draw_pixel((int)a2, lo32(a3), hi32(a3),
                                         (uint32_t)a4);
    case GUI_OP_INVALIDATE: return (uint64_t)gui_invalidate_window((int)a2);
    case GUI_OP_RENDER:     gui_render_now(); return 0;
    case GUI_OP_SET_CURSOR: gui_set_cursor((gui_cursor_t)a2); return 0;
    case GUI_OP_GET_SIZE: {
        uint32_t w, h;
        if (gui_get_window_size((int)a2, &w, &h) != 0) return (uint64_t)-1;
        uint32_t *out = (uint32_t *)a3;
        if (out) { out[0] = w; out[1] = h; }
        return 0;
    }
    }
    return (uint64_t)-1;
}

uint64_t syscall_gui_event(uint64_t wid, uint64_t user_evt, uint64_t blocking) {
    gui_event_t *out = (gui_event_t *)user_evt;
    if (!out) return (uint64_t)-1;
    if (blocking) {
        return (uint64_t)gui_wait_event((int)wid, out);
    }
    return (uint64_t)gui_poll_event((int)wid, out);
}
