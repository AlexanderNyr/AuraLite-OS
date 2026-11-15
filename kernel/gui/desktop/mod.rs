//! AuraLite desktop shell.
//! kernel/gui/desktop/mod.rs
//! The desktop owns taskbar, start-menu, desktop-icon and notification state.
//! Window management, input dispatch and compositing remain in gui.c for now.

pub mod apps;
pub mod icons;
pub mod notifications;
pub mod start_menu;
pub mod taskbar;
pub mod taskbar_layout;

use core::ffi::c_char;

pub(crate) const TEXT_SIZE: u32 = 16;
pub(crate) const SMALL_TEXT_SIZE: u32 = 14;

#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct GuiTheme {
    pub desktop_top: u32,
    pub desktop_bot: u32,
    pub win_bg: u32,
    pub win_content: u32,
    pub title_active: u32,
    pub title_inactive: u32,
    pub title_text: u32,
    pub border: u32,
    pub border_active: u32,
    pub taskbar_bg: u32,
    pub taskbar_border: u32,
    pub taskbar_text: u32,
    pub start_btn_bg: u32,
    pub start_btn_text: u32,
    pub close_bg: u32,
    pub close_bg_hover: u32,
    pub max_bg: u32,
    pub min_bg: u32,
    pub icon_text: u32,
    pub icon_selected: u32,
    pub notif_bg: u32,
    pub notif_border: u32,
    pub notif_text: u32,
    pub shadow_color: u32,
    pub shadow_offset: i32,
    pub taskbar_h: u32,
    pub titlebar_h: u32,
    pub border_w: u32,
    pub resize_grip: u32,
    pub icon_size: u32,
    pub icon_pad: u32,
    pub win_round: u32,
    pub dpi: u32,
}

extern "C" {
    fn gui_get_theme() -> *const GuiTheme;
    fn gui_mark_dirty(x: i32, y: i32, w: u32, h: u32);
    fn gui_request_redraw();
    fn gui_presentation_active() -> i32;
    fn gui_draw_screen_text(
        x: i32,
        y: i32,
        text: *const c_char,
        color: u32,
        px_size: u32,
    ) -> i32;
    fn gui_screen_text_width(text: *const c_char, px_size: u32, out_width: *mut u32) -> i32;
    fn gfx_get_width() -> u32;
    fn gfx_get_height() -> u32;
    fn gfx_fill_rect(x: u32, y: u32, w: u32, h: u32, color: u32);
}

#[inline]
pub(crate) fn theme() -> GuiTheme {
    unsafe {
        let ptr = gui_get_theme();
        if ptr.is_null() {
            GuiTheme {
                desktop_top: 0,
                desktop_bot: 0,
                win_bg: 0,
                win_content: 0,
                title_active: 0,
                title_inactive: 0,
                title_text: 0x00ff_ffff,
                border: 0,
                border_active: 0,
                taskbar_bg: 0x0020_2838,
                taskbar_border: 0,
                taskbar_text: 0x00ff_ffff,
                start_btn_bg: 0x0030_4868,
                start_btn_text: 0x00ff_ffff,
                close_bg: 0,
                close_bg_hover: 0,
                max_bg: 0,
                min_bg: 0,
                icon_text: 0x00ff_ffff,
                icon_selected: 0x0040_6080,
                notif_bg: 0x0028_2838,
                notif_border: 0,
                notif_text: 0x00ff_ffff,
                shadow_color: 0,
                shadow_offset: 0,
                taskbar_h: 28,
                titlebar_h: 22,
                border_w: 0,
                resize_grip: 0,
                icon_size: 32,
                icon_pad: 4,
                win_round: 0,
                dpi: 96,
            }
        } else {
            ptr.read()
        }
    }
}

#[inline]
pub(crate) fn screen_width() -> u32 {
    unsafe { gfx_get_width() }
}

#[inline]
pub(crate) fn screen_height() -> u32 {
    unsafe { gfx_get_height() }
}

#[inline]
pub(crate) fn presentation_active() -> bool {
    unsafe { gui_presentation_active() != 0 }
}

#[inline]
pub(crate) fn fill_rect(x: u32, y: u32, w: u32, h: u32, color: u32) {
    if w != 0 && h != 0 {
        unsafe { gfx_fill_rect(x, y, w, h, color) }
    }
}

#[inline]
pub(crate) fn mark_dirty(x: i32, y: i32, w: u32, h: u32) {
    if w != 0 && h != 0 {
        unsafe { gui_mark_dirty(x, y, w, h) }
    }
}

#[inline]
pub(crate) fn request_redraw() {
    unsafe { gui_request_redraw() }
}

#[inline]
pub(crate) fn text_line_height(px_size: u32) -> u32 {
    (px_size.saturating_mul(6).saturating_add(4)) / 5
}

/// Draw a UTF-8 string using the shared Inter MSDF atlas.
pub(crate) fn draw_text(x: i32, y: i32, text: &str, color: u32, px_size: u32) {
    let mut buffer = [0u8; 256];
    let mut end = text.len().min(buffer.len() - 1);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    buffer[..end].copy_from_slice(&text.as_bytes()[..end]);
    buffer[end] = 0;
    unsafe {
        let _ = gui_draw_screen_text(
            x,
            y,
            buffer.as_ptr().cast::<c_char>(),
            color,
            px_size,
        );
    }
}

/// Draw a UTF-8 string, trimming at a character boundary to fit `max_width`.
pub(crate) fn draw_text_fit(
    x: i32,
    y: i32,
    text: &str,
    max_width: u32,
    color: u32,
    px_size: u32,
) {
    let mut buffer = [0u8; 256];
    let mut end = text.len().min(buffer.len() - 1);
    while !text.is_char_boundary(end) {
        end -= 1;
    }

    loop {
        buffer[..end].copy_from_slice(&text.as_bytes()[..end]);
        buffer[end] = 0;
        let mut width = 0u32;
        let measured = unsafe {
            gui_screen_text_width(buffer.as_ptr().cast::<c_char>(), px_size, &mut width) == 0
        };
        if !measured || width <= max_width || end == 0 {
            break;
        }

        end = text[..end]
            .char_indices()
            .last()
            .map(|(index, _)| index)
            .unwrap_or(0);
    }

    unsafe {
        let _ = gui_draw_screen_text(
            x,
            y,
            buffer.as_ptr().cast::<c_char>(),
            color,
            px_size,
        );
    }
}

/// Draw a NUL-terminated title supplied by the C window manager.
pub(crate) fn draw_c_text_fit(
    x: i32,
    y: i32,
    text: *const c_char,
    max_width: u32,
    color: u32,
    px_size: u32,
) {
    if text.is_null() {
        return;
    }
    let bytes = unsafe { core::ffi::CStr::from_ptr(text) }.to_bytes();
    if let Ok(value) = core::str::from_utf8(bytes) {
        draw_text_fit(x, y, value, max_width, color, px_size);
    }
}

#[inline]
pub(crate) fn center_text_y(top: u32, height: u32, px_size: u32) -> i32 {
    let line_height = text_line_height(px_size);
    (top + height.saturating_sub(line_height) / 2) as i32
}

/// Initialise all desktop-owned state. The C entry point is retained.
#[no_mangle]
pub extern "C" fn gui_shell_init() {
    icons::init();
    notifications::init();
    taskbar::init();
    start_menu::init();
}
