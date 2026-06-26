#ifndef AURALITE_KERNEL_GUI_SYSCALLS_H
#define AURALITE_KERNEL_GUI_SYSCALLS_H

#include <stdint.h>

/*
 * User-facing syscall numbers for the GUI v2.0.
 *
 * SYS_GUI_CALL  (200) : window lifecycle + drawing + icons + notifications.
 * SYS_GUI_EVENT (201) : event polling/wait.
 * SYS_GUI_THEME (202) : theme get/set (new in v2).
 */

#define SYS_GUI_CALL    200
#define SYS_GUI_EVENT   201
#define SYS_GUI_THEME   202

/* Sub-ops for SYS_GUI_CALL. */
enum {
    GUI_OP_CREATE = 1,      /* a2=x|y<<32, a3=w|h<<32, a4=title*, a5=flags */
    GUI_OP_DESTROY,         /* a2=wid */
    GUI_OP_SHOW,            /* a2=wid */
    GUI_OP_HIDE,            /* a2=wid */
    GUI_OP_MOVE,            /* a2=wid, a3=x|y<<32 */
    GUI_OP_RESIZE,          /* a2=wid, a3=w|h<<32 */
    GUI_OP_TITLE,           /* a2=wid, a3=title* */
    GUI_OP_FOCUS,           /* a2=wid */
    GUI_OP_MINIMIZE,        /* a2=wid */
    GUI_OP_MAXIMIZE,        /* a2=wid */
    GUI_OP_RESTORE,         /* a2=wid */
    GUI_OP_SNAP,            /* a2=wid, a3=snap_type (gui_snap_t) */
    GUI_OP_CLEAR,           /* a2=wid, a3=color */
    GUI_OP_FILL_RECT,       /* a2=wid, a3=x|y<<32, a4=w|h<<32, a5=color */
    GUI_OP_DRAW_RECT,       /* same */
    GUI_OP_DRAW_LINE,       /* a2=wid, a3=x0|y0<<32, a4=x1|y1<<32, a5=color */
    GUI_OP_DRAW_TEXT,       /* a2=wid, a3=x|y<<32, a4=text*, a5=color */
    GUI_OP_DRAW_PIXEL,      /* a2=wid, a3=x|y<<32, a4=color */
    GUI_OP_INVALIDATE,      /* a2=wid */
    GUI_OP_RENDER,          /* a2=0 (request immediate render) */
    GUI_OP_SET_CURSOR,      /* a2=cursor_id */
    GUI_OP_GET_SIZE,        /* a2=wid, a3=user u32*[2] (w, h) */
    GUI_OP_GET_POS,         /* a2=wid, a3=user int32*[2] (x, y) */
    GUI_OP_GET_RECT,        /* a2=wid, a3=user {i32,i32,u32,u32}* */
    GUI_OP_SET_CLIPBOARD,   /* a2=char* */
    GUI_OP_GET_CLIPBOARD,   /* a2=char*, a3=size */
    GUI_OP_BLIT,            /* a2=wid, a3=x|y<<32, a4={w,h,src*,stride} packed, a5=color_key (0=none) */
    GUI_OP_BLIT_ALPHA,      /* same layout, per-pixel alpha blending */
    /* Desktop icons (new in v2). */
    GUI_OP_ADD_ICON,        /* a2=x|y<<32, a3=label*, a4=icon_id */
    GUI_OP_REMOVE_ICON,     /* a2=icon_idx */
    /* Notifications (new in v2). */
    GUI_OP_NOTIFY,          /* a2=text*, a3=color, a4=duration_ms (0=default) */
    /* Window flags query. */
    GUI_OP_GET_FLAGS,       /* a2=wid → returns flags */
};

uint64_t syscall_gui_call (uint64_t op, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5);
uint64_t syscall_gui_event(uint64_t wid, uint64_t user_evt, uint64_t blocking);

/* Theme syscall: a1 = sub-op (0=get size, 1=get theme, 2=set theme). */
uint64_t syscall_gui_theme(uint64_t subop, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5);

#endif
