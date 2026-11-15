//! Taskbar rendering, taskbar hit testing and overflow dropdown.

use core::ffi::c_char;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use super::{
    center_text_y, draw_c_text_fit, draw_text, fill_rect, presentation_active, request_redraw,
    screen_height, screen_width, theme, TEXT_SIZE, SMALL_TEXT_SIZE,
};
use super::taskbar_layout as layout;

static MORE_OPEN: AtomicBool = AtomicBool::new(false);
static MORE_PAGE: AtomicU32 = AtomicU32::new(0);

extern "C" {
    fn gui_taskbar_win_count() -> i32;
    fn gui_taskbar_win_at(slot: i32) -> i32;
    fn gui_get_window_title(wid: i32, buffer: *mut c_char, size: u32);
    fn gui_window_is_minimized(wid: i32) -> i32;
    fn gui_focused_window() -> i32;
    fn gui_focus_window(wid: i32) -> i32;
    fn gui_minimize_window(wid: i32) -> i32;
    fn gui_restore_window(wid: i32) -> i32;
    fn gui_set_cursor(cursor: i32);
    fn timer_get_ticks() -> u64;
    fn timer_get_frequency() -> u32;
}

pub(crate) fn init() {
    MORE_OPEN.store(false, Ordering::Relaxed);
    MORE_PAGE.store(0, Ordering::Relaxed);
}

#[inline]
fn more_is_open() -> bool {
    MORE_OPEN.load(Ordering::Relaxed)
}

#[inline]
fn window_count() -> u32 {
    unsafe { gui_taskbar_win_count().max(0) as u32 }
}

fn taskbar_hit(mx: i32, my: i32) -> i32 {
    if presentation_active() {
        return -1;
    }

    let fw = screen_width();
    let fh = screen_height();
    let taskbar_height = theme().taskbar_h;
    let bar_y = fh.saturating_sub(taskbar_height) as i32;
    if my < bar_y {
        return -1;
    }

    if mx >= layout::TB_START_X as i32
        && mx < (layout::TB_START_X + layout::TB_START_W) as i32
        && my >= bar_y + 4
    {
        return -2;
    }

    let count = window_count();
    let current = layout::layout(fw, count);
    for slot in 0..current.visible {
        if let Some((button_x, button_width)) = layout::button_rect(fw, count, slot) {
            if mx >= button_x as i32 && mx < (button_x + button_width) as i32 {
                return unsafe { gui_taskbar_win_at(slot as i32) };
            }
        }
    }

    if current.more_w != 0
        && mx >= current.more_x as i32
        && mx < (current.more_x + current.more_w) as i32
    {
        return -3;
    }
    -1
}

pub(crate) fn is_start_button_at(mx: i32, my: i32) -> bool {
    taskbar_hit(mx, my) == -2
}

fn overflow_hit(mx: i32, my: i32) -> i32 {
    if !more_is_open() {
        return -1;
    }
    let fw = screen_width();
    let fh = screen_height();
    let taskbar_height = theme().taskbar_h;
    let count = window_count();
    let geometry = layout::overflow_geometry(fw, fh, taskbar_height, count);
    if geometry.rows == 0
        || mx < geometry.x
        || mx >= geometry.x + geometry.w as i32
        || my < geometry.y
        || my >= geometry.y + geometry.h as i32
    {
        return -1;
    }

    let relative_y = my - geometry.y - layout::TB_OV_PAD as i32;
    if relative_y < 0 {
        return -1;
    }
    let row = relative_y as u32 / geometry.row_h;
    let entry = layout::overflow_row_entry(&geometry, MORE_PAGE.load(Ordering::Relaxed), row);
    if entry < 0 {
        return -3;
    }
    let current = layout::layout(fw, count);
    unsafe { gui_taskbar_win_at((current.visible + entry as u32) as i32) }
}

fn draw_overflow(fw: u32, fh: u32, taskbar_height: u32) {
    if !more_is_open() {
        return;
    }
    let count = window_count();
    let geometry = layout::overflow_geometry(fw, fh, taskbar_height, count);
    if geometry.rows == 0 {
        return;
    }

    fill_rect(
        geometry.x.max(0) as u32,
        geometry.y.max(0) as u32,
        geometry.w,
        geometry.h,
        0x0028_2838,
    );

    let current = layout::layout(fw, count);
    let page = MORE_PAGE.load(Ordering::Relaxed);
    for row in 0..geometry.rows {
        let entry = layout::overflow_row_entry(&geometry, page, row);
        let y = geometry.y + layout::TB_OV_PAD as i32 + (row * geometry.row_h) as i32;
        if entry < 0 {
            draw_text(
                geometry.x + 8,
                y,
                "> more",
                0x0070_b0d8,
                SMALL_TEXT_SIZE,
            );
            continue;
        }

        let wid = unsafe { gui_taskbar_win_at((current.visible + entry as u32) as i32) };
        if wid < 0 {
            continue;
        }
        let mut title = [0u8; 64];
        unsafe { gui_get_window_title(wid, title.as_mut_ptr().cast::<c_char>(), title.len() as u32); }
        draw_c_text_fit(
            geometry.x + 8,
            y,
            title.as_ptr().cast::<c_char>(),
            geometry.w.saturating_sub(20),
            theme().taskbar_text,
            SMALL_TEXT_SIZE,
        );
    }
}

#[no_mangle]
pub extern "C" fn shell_draw_taskbar() {
    if presentation_active() {
        return;
    }

    let active = theme();
    let fw = screen_width();
    let fh = screen_height();
    let taskbar_height = active.taskbar_h;
    let taskbar_y = fh.saturating_sub(taskbar_height);
    let button_height = taskbar_height.saturating_sub(8);

    fill_rect(0, taskbar_y, fw, taskbar_height, active.taskbar_bg);

    // Start button.
    fill_rect(
        4,
        taskbar_y + 4,
        layout::TB_START_W,
        button_height,
        active.start_btn_bg,
    );
    draw_text(
        12,
        center_text_y(taskbar_y + 4, button_height, TEXT_SIZE),
        "AuraLite",
        active.start_btn_text,
        TEXT_SIZE,
    );

    // The menu is drawn above the taskbar and below the window strip.
    super::start_menu::draw(taskbar_y);

    // Normal window buttons.
    let count = window_count();
    let current = layout::layout(fw, count);
    for slot in 0..current.visible {
        let wid = unsafe { gui_taskbar_win_at(slot as i32) };
        if wid < 0 {
            break;
        }
        let Some((button_x, button_width)) = layout::button_rect(fw, count, slot) else {
            break;
        };
        let color = unsafe {
            if wid == gui_focused_window() {
                0x002f_60c0
            } else if gui_window_is_minimized(wid) != 0 {
                0x0020_2830
            } else {
                0x0030_3840
            }
        };
        fill_rect(button_x, taskbar_y + 4, button_width, button_height, color);

        let mut title = [0u8; 64];
        unsafe { gui_get_window_title(wid, title.as_mut_ptr().cast::<c_char>(), title.len() as u32); }
        draw_c_text_fit(
            button_x as i32 + 6,
            center_text_y(taskbar_y + 4, button_height, TEXT_SIZE),
            title.as_ptr().cast::<c_char>(),
            button_width.saturating_sub(12),
            active.taskbar_text,
            TEXT_SIZE,
        );
    }

    // Overflow button.
    if current.more_w != 0 {
        let color = if more_is_open() { 0x0040_5060 } else { 0x0030_3840 };
        fill_rect(current.more_x, taskbar_y + 4, current.more_w, button_height, color);
        draw_text(
            current.more_x as i32 + (current.more_w.saturating_sub(8) / 2) as i32,
            center_text_y(taskbar_y + 4, button_height, TEXT_SIZE),
            ">",
            active.taskbar_text,
            TEXT_SIZE,
        );
    }

    // Uptime clock, matching the previous shell implementation.
    let mut frequency = unsafe { timer_get_frequency() };
    if frequency == 0 {
        frequency = 100;
    }
    let seconds = unsafe { timer_get_ticks() } / frequency as u64;
    let hours = ((seconds / 3600) % 24) as u32;
    let minutes = ((seconds / 60) % 60) as u32;
    let secs = (seconds % 60) as u32;
    let clock = [
        b'0' + ((hours / 10) % 10) as u8,
        b'0' + (hours % 10) as u8,
        b':',
        b'0' + ((minutes / 10) % 10) as u8,
        b'0' + (minutes % 10) as u8,
        b':',
        b'0' + ((secs / 10) % 10) as u8,
        b'0' + (secs % 10) as u8,
    ];
    let clock_text = unsafe { core::str::from_utf8_unchecked(&clock) };
    draw_text(
        fw.saturating_sub(90) as i32,
        center_text_y(taskbar_y, taskbar_height, TEXT_SIZE),
        clock_text,
        active.taskbar_text,
        TEXT_SIZE,
    );

    // Draw the popup last because it overlaps the window buttons.
    draw_overflow(fw, fh, taskbar_height);
}

#[no_mangle]
pub extern "C" fn shell_handle_overflow_click(mx: i32, my: i32, left_pressed: i32) -> i32 {
    if !more_is_open() {
        return 0;
    }

    let hit = overflow_hit(mx, my);
    if left_pressed != 0 {
        if hit >= 0 {
            unsafe {
                if gui_window_is_minimized(hit) != 0 {
                    gui_restore_window(hit);
                } else {
                    gui_focus_window(hit);
                }
            }
            MORE_OPEN.store(false, Ordering::Relaxed);
            request_redraw();
            return 1;
        }

        if hit == -3 {
            let next = layout::overflow_next_page(
                &layout::overflow_geometry(
                    screen_width(),
                    screen_height(),
                    theme().taskbar_h,
                    window_count(),
                ),
                MORE_PAGE.load(Ordering::Relaxed),
            );
            MORE_PAGE.store(next, Ordering::Relaxed);
            request_redraw();
            return 1;
        }

        MORE_OPEN.store(false, Ordering::Relaxed);
        request_redraw();
        if taskbar_hit(mx, my) == -3 {
            return 1;
        }
        return 0;
    }

    if hit != -1 {
        unsafe { gui_set_cursor(0); }
        return 1;
    }
    0
}

#[no_mangle]
pub extern "C" fn shell_handle_taskbar_click(mx: i32, my: i32, left_pressed: i32) -> i32 {
    if left_pressed == 0 {
        return 0;
    }

    match taskbar_hit(mx, my) {
        -2 => {
            super::start_menu::toggle();
            MORE_OPEN.store(false, Ordering::Relaxed);
            request_redraw();
            unsafe { gui_set_cursor(0); }
            1
        }
        -3 => {
            MORE_OPEN.store(!more_is_open(), Ordering::Relaxed);
            MORE_PAGE.store(0, Ordering::Relaxed);
            super::start_menu::close();
            request_redraw();
            1
        }
        wid if wid >= 0 => {
            unsafe {
                if gui_window_is_minimized(wid) != 0 {
                    gui_restore_window(wid);
                } else if gui_focused_window() == wid {
                    gui_minimize_window(wid);
                } else {
                    gui_focus_window(wid);
                }
            }
            request_redraw();
            0
        }
        _ => 0,
    }
}
