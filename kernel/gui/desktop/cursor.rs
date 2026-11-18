//! Software cursor state, damage tracking and rendering.
//!
//! The Window Manager still supplies the pointer position and decides when a
//! frame is presented. This module owns the cursor shape and the last cursor
//! position committed to the framebuffer.

use core::sync::atomic::{AtomicBool, AtomicI32, Ordering};

use super::mark_dirty;

// Keep these values in sync with gui_cursor_t in kernel/gui/gui.h.
const CURSOR_ARROW: i32 = 0;
const CURSOR_IBEAM: i32 = 1;
const CURSOR_HAND: i32 = 2;
const CURSOR_HRESIZE: i32 = 3;
const CURSOR_VRESIZE: i32 = 4;
const CURSOR_DRESIZE: i32 = 5;
const CURSOR_WAIT: i32 = 6;
const CURSOR_MOVE: i32 = 7;
const CURSOR_CROSSHAIR: i32 = 8;
const CURSOR_NOT_ALLOWED: i32 = 9;

const CURSOR_DIRTY_LEFT: i32 = 8;
const CURSOR_DIRTY_TOP: i32 = 8;
const CURSOR_DIRTY_WIDTH: u32 = 20;
const CURSOR_DIRTY_HEIGHT: u32 = 24;

static CURSOR_SHAPE: AtomicI32 = AtomicI32::new(CURSOR_ARROW);
static LAST_DRAWN_X: AtomicI32 = AtomicI32::new(-1);
static LAST_DRAWN_Y: AtomicI32 = AtomicI32::new(-1);
static LAST_DRAWN_VALID: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn mouse_get_position(x: *mut i32, y: *mut i32) -> i32;
    fn gui_poke();

    fn gfx_putpixel(x: u32, y: u32, color: u32);
    fn gfx_fill_rect(x: u32, y: u32, width: u32, height: u32, color: u32);
    fn gfx_draw_rect(x: u32, y: u32, width: u32, height: u32, color: u32);
    fn gfx_draw_line(x0: u32, y0: u32, x1: u32, y1: u32, color: u32);
    fn gfx_draw_circle(cx: u32, cy: u32, radius: u32, color: u32);
    fn gfx_fill_circle(cx: u32, cy: u32, radius: u32, color: u32);
}

#[inline]
fn mark_cursor_dirty(x: i32, y: i32) {
    mark_dirty(
        x.saturating_sub(CURSOR_DIRTY_LEFT),
        y.saturating_sub(CURSOR_DIRTY_TOP),
        CURSOR_DIRTY_WIDTH,
        CURSOR_DIRTY_HEIGHT,
    );
}

/// Reset cursor-owned state when the desktop shell is initialised.
pub(crate) fn init() {
    CURSOR_SHAPE.store(CURSOR_ARROW, Ordering::Relaxed);
    LAST_DRAWN_X.store(-1, Ordering::Relaxed);
    LAST_DRAWN_Y.store(-1, Ordering::Relaxed);
    LAST_DRAWN_VALID.store(false, Ordering::Release);
}

/// Reset cursor-owned state during GUI initialization.
#[no_mangle]
pub extern "C" fn gui_cursor_init() {
    init();
}

/// Update the cursor shape while invalidating its currently visible footprint.
fn set_shape(shape: i32) {
    if CURSOR_SHAPE.swap(shape, Ordering::AcqRel) == shape {
        return;
    }

    let mut x = -1;
    let mut y = -1;
    if unsafe { mouse_get_position(&mut x, &mut y) } == 1 {
        mark_cursor_dirty(x, y);
    } else if LAST_DRAWN_VALID.load(Ordering::Acquire) {
        mark_cursor_dirty(
            LAST_DRAWN_X.load(Ordering::Relaxed),
            LAST_DRAWN_Y.load(Ordering::Relaxed),
        );
    }

    unsafe { gui_poke() };
}

/// Select the cursor shape from Window Manager hit-test and drag state.
/// The C Window Manager supplies its state; the cursor policy lives here.
#[no_mangle]
pub extern "C" fn gui_cursor_update_from_input(
    hit_part: i32,
    drag_mode: i32,
    drag_edge: i32,
) {
    let shape = match drag_mode {
        1 => CURSOR_MOVE,
        2 if drag_edge & 0x3 != 0 => CURSOR_HRESIZE,
        2 if drag_edge & 0xC != 0 => CURSOR_VRESIZE,
        2 => CURSOR_DRESIZE,
        _ => match hit_part {
            5 | 7 | 8 => CURSOR_HRESIZE,
            9 | 10 => CURSOR_VRESIZE,
            _ => CURSOR_ARROW,
        },
    };

    set_shape(shape);
}

/// Public C ABI retained for existing kernel and desktop callers.
#[no_mangle]
pub extern "C" fn gui_set_cursor(shape: i32) {
    set_shape(shape);
}

/// Mark both the previously presented cursor and its current position dirty.
/// The compositor calls this once after sampling the mouse for a tick.
#[no_mangle]
pub extern "C" fn gui_cursor_track_position(have_cursor: i32, x: i32, y: i32) {
    let have_cursor = have_cursor != 0;
    let was_valid = LAST_DRAWN_VALID.load(Ordering::Acquire);

    if have_cursor {
        let old_x = LAST_DRAWN_X.load(Ordering::Relaxed);
        let old_y = LAST_DRAWN_Y.load(Ordering::Relaxed);
        if !was_valid || old_x != x || old_y != y {
            if was_valid {
                mark_cursor_dirty(old_x, old_y);
            }
            mark_cursor_dirty(x, y);
        }
    } else if was_valid {
        mark_cursor_dirty(
            LAST_DRAWN_X.load(Ordering::Relaxed),
            LAST_DRAWN_Y.load(Ordering::Relaxed),
        );
    }
}

/// Record which cursor position was included in a successfully presented
/// frame. Presentation itself remains owned by the C compositor.
#[no_mangle]
pub extern "C" fn gui_cursor_commit_presented(have_cursor: i32, x: i32, y: i32) {
    let have_cursor = have_cursor != 0;
    LAST_DRAWN_X.store(if have_cursor { x } else { -1 }, Ordering::Relaxed);
    LAST_DRAWN_Y.store(if have_cursor { y } else { -1 }, Ordering::Relaxed);
    LAST_DRAWN_VALID.store(have_cursor, Ordering::Release);
}

/// Returns 1 when the cursor footprint intersects a screen-space rectangle.
#[no_mangle]
pub extern "C" fn gui_cursor_intersects_rect(
    mx: i32,
    my: i32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
) -> i32 {
    let cx0 = mx as i64 - CURSOR_DIRTY_LEFT as i64;
    let cy0 = my as i64 - CURSOR_DIRTY_TOP as i64;
    let cx1 = cx0 + CURSOR_DIRTY_WIDTH as i64;
    let cy1 = cy0 + CURSOR_DIRTY_HEIGHT as i64;
    let rx0 = x as i64;
    let ry0 = y as i64;
    let rx1 = rx0 + width as i64;
    let ry1 = ry0 + height as i64;

    if cx0 < rx1 && cy0 < ry1 && cx1 > rx0 && cy1 > ry0 {
        1
    } else {
        0
    }
}

/// Draw the current software cursor into the compositor back buffer.
/// The caller has already established the appropriate graphics clip.
#[no_mangle]
pub extern "C" fn gui_cursor_draw(mx: i32, my: i32) {
    let shape = CURSOR_SHAPE.load(Ordering::Acquire);

    unsafe {
        match shape {
            CURSOR_IBEAM => {
                for offset in -8..=8 {
                    gfx_putpixel(mx as u32, my.wrapping_add(offset) as u32, 0x00000000);
                }
                gfx_draw_line(
                    mx.wrapping_sub(3) as u32,
                    my.wrapping_sub(8) as u32,
                    mx.wrapping_add(3) as u32,
                    my.wrapping_sub(8) as u32,
                    0x00000000,
                );
                gfx_draw_line(
                    mx.wrapping_sub(3) as u32,
                    my.wrapping_add(8) as u32,
                    mx.wrapping_add(3) as u32,
                    my.wrapping_add(8) as u32,
                    0x00000000,
                );
            }
            CURSOR_HAND => {
                gfx_fill_rect(
                    mx.wrapping_sub(4) as u32,
                    my.wrapping_sub(1) as u32,
                    8,
                    12,
                    0x00FFFFFF,
                );
                gfx_draw_rect(
                    mx.wrapping_sub(4) as u32,
                    my.wrapping_sub(1) as u32,
                    8,
                    12,
                    0x00000000,
                );
            }
            CURSOR_HRESIZE => {
                gfx_draw_line(
                    mx.wrapping_sub(8) as u32,
                    my as u32,
                    mx.wrapping_add(8) as u32,
                    my as u32,
                    0x00FFFFFF,
                );
                gfx_draw_line(
                    mx.wrapping_sub(8) as u32,
                    my.wrapping_sub(1) as u32,
                    mx.wrapping_add(8) as u32,
                    my.wrapping_sub(1) as u32,
                    0x00000000,
                );
                gfx_draw_line(
                    mx.wrapping_sub(8) as u32,
                    my.wrapping_add(1) as u32,
                    mx.wrapping_add(8) as u32,
                    my.wrapping_add(1) as u32,
                    0x00000000,
                );
                gfx_draw_line(
                    mx.wrapping_sub(8) as u32,
                    my as u32,
                    mx.wrapping_sub(5) as u32,
                    my.wrapping_sub(3) as u32,
                    0x00FFFFFF,
                );
                gfx_draw_line(
                    mx.wrapping_sub(8) as u32,
                    my as u32,
                    mx.wrapping_sub(5) as u32,
                    my.wrapping_add(3) as u32,
                    0x00FFFFFF,
                );
                gfx_draw_line(
                    mx.wrapping_add(8) as u32,
                    my as u32,
                    mx.wrapping_add(5) as u32,
                    my.wrapping_sub(3) as u32,
                    0x00FFFFFF,
                );
                gfx_draw_line(
                    mx.wrapping_add(8) as u32,
                    my as u32,
                    mx.wrapping_add(5) as u32,
                    my.wrapping_add(3) as u32,
                    0x00FFFFFF,
                );
            }
            CURSOR_VRESIZE => {
                gfx_draw_line(
                    mx as u32,
                    my.wrapping_sub(8) as u32,
                    mx as u32,
                    my.wrapping_add(8) as u32,
                    0x00FFFFFF,
                );
                gfx_draw_line(
                    mx.wrapping_sub(1) as u32,
                    my.wrapping_sub(8) as u32,
                    mx.wrapping_sub(1) as u32,
                    my.wrapping_add(8) as u32,
                    0x00000000,
                );
                gfx_draw_line(
                    mx.wrapping_add(1) as u32,
                    my.wrapping_sub(8) as u32,
                    mx.wrapping_add(1) as u32,
                    my.wrapping_add(8) as u32,
                    0x00000000,
                );
            }
            CURSOR_DRESIZE => {
                for offset in -7..=7 {
                    gfx_putpixel(
                        mx.wrapping_add(offset) as u32,
                        my.wrapping_add(offset) as u32,
                        0x00FFFFFF,
                    );
                }
                gfx_draw_line(
                    mx.wrapping_sub(7) as u32,
                    my.wrapping_sub(7) as u32,
                    mx.wrapping_add(7) as u32,
                    my.wrapping_add(7) as u32,
                    0x00000000,
                );
            }
            CURSOR_WAIT => {
                gfx_fill_circle(mx as u32, my as u32, 6, 0x00FFFFFF);
                gfx_draw_circle(mx as u32, my as u32, 7, 0x00000000);
            }
            CURSOR_MOVE => {
                gfx_draw_line(
                    mx.wrapping_sub(8) as u32,
                    my as u32,
                    mx.wrapping_add(8) as u32,
                    my as u32,
                    0x00FFFFFF,
                );
                gfx_draw_line(
                    mx as u32,
                    my.wrapping_sub(8) as u32,
                    mx as u32,
                    my.wrapping_add(8) as u32,
                    0x00FFFFFF,
                );
            }
            CURSOR_CROSSHAIR => {
                gfx_draw_line(
                    mx.wrapping_sub(8) as u32,
                    my as u32,
                    mx.wrapping_sub(2) as u32,
                    my as u32,
                    0x00000000,
                );
                gfx_draw_line(
                    mx.wrapping_add(2) as u32,
                    my as u32,
                    mx.wrapping_add(8) as u32,
                    my as u32,
                    0x00000000,
                );
                gfx_draw_line(
                    mx as u32,
                    my.wrapping_sub(8) as u32,
                    mx as u32,
                    my.wrapping_sub(2) as u32,
                    0x00000000,
                );
                gfx_draw_line(
                    mx as u32,
                    my.wrapping_add(2) as u32,
                    mx as u32,
                    my.wrapping_add(8) as u32,
                    0x00000000,
                );
            }
            CURSOR_NOT_ALLOWED => {
                gfx_draw_circle(mx as u32, my as u32, 7, 0x00C04040);
                gfx_draw_line(
                    mx.wrapping_sub(5) as u32,
                    my.wrapping_add(5) as u32,
                    mx.wrapping_add(5) as u32,
                    my.wrapping_sub(5) as u32,
                    0x00C04040,
                );
            }
            _ => {
                // Classic 11x16 arrow. The outer edges are black and the
                // interior is white, matching the previous C implementation.
                const ARROW_WIDTH: [u8; 16] = [
                    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 6, 5, 4, 3, 2,
                ];
                for (row, width) in ARROW_WIDTH.iter().copied().enumerate() {
                    let row_y = my.wrapping_add(row as i32) as u32;
                    let width = width as u32;
                    if width > 2 {
                        gfx_fill_rect(
                            mx.wrapping_add(1) as u32,
                            row_y,
                            width - 2,
                            1,
                            0x00FFFFFF,
                        );
                    }
                    gfx_fill_rect(mx as u32, row_y, 1, 1, 0x00000000);
                    if width > 1 {
                        gfx_fill_rect(
                            mx.wrapping_add(width as i32 - 1) as u32,
                            row_y,
                            1,
                            1,
                            0x00000000,
                        );
                    }
                }
            }
        }
    }
}
