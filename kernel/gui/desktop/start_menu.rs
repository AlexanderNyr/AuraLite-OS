//! Start-menu state, geometry, rendering and click handling.

use core::sync::atomic::{AtomicBool, Ordering};

use super::{
    apps::{self, MenuRow},
    draw_text, fill_rect, presentation_active, request_redraw, screen_height,
    theme, SMALL_TEXT_SIZE,
};

const MENU_WIDTH: u32 = 180;
const MENU_X: i32 = 4;
const MENU_ITEM_Y: u32 = 28;
const MENU_ITEM_HEIGHT: u32 = 18;
const MENU_MIN_HEIGHT: u32 = 220;
const MENU_BOTTOM_PAD: u32 = 8;

static MENU_OPEN: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn gui_set_cursor(cursor: i32);
}

#[derive(Copy, Clone)]
struct MenuRect {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
}

pub(crate) fn init() {
    MENU_OPEN.store(false, Ordering::Relaxed);
}

#[inline]
pub(crate) fn is_open() -> bool {
    MENU_OPEN.load(Ordering::Relaxed)
}

pub(crate) fn toggle() {
    MENU_OPEN.store(!is_open(), Ordering::Relaxed);
}

pub(crate) fn close() {
    MENU_OPEN.store(false, Ordering::Relaxed);
}

fn menu_height() -> u32 {
    let calculated = MENU_ITEM_Y
        .saturating_add((apps::menu_row_count() as u32).saturating_mul(MENU_ITEM_HEIGHT))
        .saturating_add(MENU_BOTTOM_PAD);
    calculated.max(MENU_MIN_HEIGHT)
}

fn menu_rect() -> Option<MenuRect> {
    let height = screen_height();
    let taskbar_height = theme().taskbar_h;
    let menu_height = menu_height();
    if height <= taskbar_height.saturating_add(menu_height) {
        return None;
    }
    Some(MenuRect {
        x: MENU_X,
        y: (height - taskbar_height - menu_height) as i32,
        width: MENU_WIDTH,
        height: menu_height,
    })
}

/// Returns an app index, -2 for header/empty space, or -1 outside the menu.
fn hit_test(mx: i32, my: i32) -> i32 {
    if !is_open() {
        return -1;
    }
    let Some(rect) = menu_rect() else { return -1; };
    if mx < rect.x
        || mx >= rect.x + rect.width as i32
        || my < rect.y
        || my >= rect.y + rect.height as i32
    {
        return -1;
    }

    let relative_y = my - rect.y;
    if relative_y < MENU_ITEM_Y as i32 {
        return -2;
    }
    let row = ((relative_y as u32 - MENU_ITEM_Y) / MENU_ITEM_HEIGHT) as usize;
    match apps::menu_row(row) {
        Some(MenuRow::App(index)) => index as i32,
        Some(MenuRow::Header(_)) | None => -2,
    }
}

fn contains(mx: i32, my: i32) -> bool {
    hit_test(mx, my) != -1
}

pub(crate) fn draw(taskbar_y: u32) {
    if !is_open() || presentation_active() {
        return;
    }
    let Some(rect) = menu_rect() else { return; };
    let active = theme();

    fill_rect(rect.x as u32, rect.y as u32, rect.width, rect.height, 0x0028_2838);
    draw_text(
        rect.x + 12,
        rect.y + 7,
        "AuraLite",
        active.taskbar_text,
        SMALL_TEXT_SIZE,
    );

    for row in 0..apps::menu_row_count() {
        let y = rect.y + MENU_ITEM_Y as i32 + row as i32 * MENU_ITEM_HEIGHT as i32;
        if y as u32 + MENU_ITEM_HEIGHT > taskbar_y {
            break;
        }
        match apps::menu_row(row) {
            Some(MenuRow::Header(title)) => {
                draw_text(rect.x + 8, y, title, 0x0070_b0d8, SMALL_TEXT_SIZE);
            }
            Some(MenuRow::App(index)) => {
                if let Some(title) = apps::app_name(index) {
                    draw_text(rect.x + 16, y, title, active.taskbar_text, SMALL_TEXT_SIZE);
                }
            }
            None => break,
        }
    }

}

#[no_mangle]
pub extern "C" fn shell_handle_start_menu_click(
    mx: i32,
    my: i32,
    wid: i32,
    left_pressed: i32,
) -> i32 {
    if !is_open() {
        return 0;
    }

    let menu_hit = hit_test(mx, my);
    if left_pressed != 0 {
        if menu_hit >= 0 {
            apps::launch_index(menu_hit as usize);
            close();
            request_redraw();
            return 1;
        }
        if menu_hit == -2 {
            return 1;
        }
        if !(wid < 0 && super::taskbar::is_start_button_at(mx, my)) {
            close();
            request_redraw();
            return 1;
        }
        return 0;
    }

    if contains(mx, my) {
        unsafe { gui_set_cursor(0); }
        return 1;
    }
    0
}
