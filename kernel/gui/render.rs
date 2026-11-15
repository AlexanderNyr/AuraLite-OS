//! Basic window renderer for the AuraLite GUI.
//!
//! This first version intentionally uses square geometry and flat surfaces.
//! Rendering is split into explicit stages so visual features can be added
//! without turning the window renderer into one large monolithic routine.

use core::ffi::c_char;

extern "C" {
    fn gfx_fill_rect_alpha(
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        color: u32,
        alpha: u8,
    );

    fn gfx_blit_rgb888(
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        src: *const u32,
        src_stride: u32,
        skip_mask: *const u8,
        mask_stride: u32,
    ) -> u64;

    fn gui_sdf_draw_screen(
        x: i32,
        y: i32,
        text: *const c_char,
        color: u32,
        px_size: u32,
    ) -> i32;
}

const HOVER_NONE: u32 = 0;
const HOVER_CLOSE: u32 = 1;
const HOVER_MAXIMIZE: u32 = 2;
const HOVER_MINIMIZE: u32 = 3;

#[repr(C)]
pub struct GuiRenderWindow {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,

    pub content_x: i32,
    pub content_y: i32,
    pub content_width: u32,
    pub content_height: u32,

    pub pixels: *const u32,
    pub buffer_width: u32,
    pub buffer_height: u32,
    pub stride: u32,

    pub hole_l: i32,
    pub hole_t: i32,
    pub hole_r: i32,
    pub hole_b: i32,

    pub title: *const c_char,

    pub decorated: u32,
    pub has_title: u32,
    pub has_close: u32,
    pub has_minmax: u32,
    pub resizable: u32,
    pub focused: u32,
    pub maximized: u32,
    pub hover_button: u32,
}

#[repr(C)]
pub struct GuiRenderTheme {
    pub window_bg: u32,
    pub window_content: u32,
    pub title_active: u32,
    pub title_inactive: u32,
    pub title_text: u32,
    pub border: u32,
    pub border_active: u32,
    pub close_hover: u32,
    pub max_hover: u32,
    pub min_hover: u32,

    pub titlebar_height: u32,
    pub border_width: u32,
    pub resize_grip: u32,
    pub text_size: u32,
}

#[inline]
unsafe fn fill_rect(x: i32, y: i32, width: u32, height: u32, color: u32) {
    if width != 0 && height != 0 {
        gfx_fill_rect_alpha(x, y, width, height, color, 255);
    }
}

#[inline]
unsafe fn fill_hline(x: i32, y: i32, width: u32, color: u32) {
    fill_rect(x, y, width, 1, color);
}

#[inline]
unsafe fn fill_vline(x: i32, y: i32, height: u32, color: u32) {
    fill_rect(x, y, 1, height, color);
}

/// Stage 1: draw the flat rectangular window surface and its title bar.
unsafe fn render_surface(window: &GuiRenderWindow, theme: &GuiRenderTheme) {
    if window.decorated == 0 {
        return;
    }

    fill_rect(
        window.x,
        window.y,
        window.width,
        window.height,
        theme.window_bg,
    );

    if window.has_title != 0 && theme.titlebar_height != 0 {
        let title_color = if window.focused != 0 {
            theme.title_active
        } else {
            theme.title_inactive
        };
        fill_rect(
            window.x,
            window.y,
            window.width,
            theme.titlebar_height.min(window.height),
            title_color,
        );
    }

    let border_width = theme.border_width.min(window.width / 2).min(window.height / 2);
    if border_width != 0 {
        let border_color = if window.focused != 0 {
            theme.border_active
        } else {
            theme.border
        };
        fill_rect(window.x, window.y, window.width, border_width, border_color);
        fill_rect(
            window.x,
            window.y + window.height as i32 - border_width as i32,
            window.width,
            border_width,
            border_color,
        );
        fill_rect(window.x, window.y, border_width, window.height, border_color);
        fill_rect(
            window.x + window.width as i32 - border_width as i32,
            window.y,
            border_width,
            window.height,
            border_color,
        );
    }
}

#[inline]
unsafe fn blit_region(
    window: &GuiRenderWindow,
    src_x: u32,
    src_y: u32,
    dst_x: i32,
    dst_y: i32,
    width: u32,
    height: u32,
) {
    if window.pixels.is_null() || width == 0 || height == 0 {
        return;
    }

    let offset = src_y as usize * window.stride as usize + src_x as usize;
    let src = window.pixels.add(offset);
    let _ = gfx_blit_rgb888(
        dst_x,
        dst_y,
        width,
        height,
        src,
        window.stride,
        core::ptr::null(),
        0,
    );
}

/// Stage 2: draw the client surface and copy its pixels. A rectangular hole
/// is split into four surrounding blits, leaving the parent/desktop visible.
unsafe fn render_content(window: &GuiRenderWindow, theme: &GuiRenderTheme) {
    if window.decorated != 0 {
        fill_rect(
            window.content_x,
            window.content_y,
            window.content_width,
            window.content_height,
            theme.window_content,
        );
    }

    if window.pixels.is_null()
        || window.stride == 0
        || window.buffer_width == 0
        || window.buffer_height == 0
        || window.stride < window.buffer_width
    {
        return;
    }

    let copy_width = window.content_width.min(window.buffer_width);
    let copy_height = window.content_height.min(window.buffer_height);
    if copy_width == 0 || copy_height == 0 {
        return;
    }

    let hx0 = window.hole_l.max(0) as u32;
    let hy0 = window.hole_t.max(0) as u32;
    let hx1 = window.hole_r.max(0) as u32;
    let hy1 = window.hole_b.max(0) as u32;

    let hole_l = hx0.min(copy_width);
    let hole_t = hy0.min(copy_height);
    let hole_r = hx1.min(copy_width);
    let hole_b = hy1.min(copy_height);

    if hole_r <= hole_l || hole_b <= hole_t {
        blit_region(
            window,
            0,
            0,
            window.content_x,
            window.content_y,
            copy_width,
            copy_height,
        );
        return;
    }

    // Region above the hole.
    if hole_t != 0 {
        blit_region(
            window,
            0,
            0,
            window.content_x,
            window.content_y,
            copy_width,
            hole_t,
        );
    }

    // Region left of the hole.
    let hole_height = hole_b - hole_t;
    if hole_l != 0 {
        blit_region(
            window,
            0,
            hole_t,
            window.content_x,
            window.content_y + hole_t as i32,
            hole_l,
            hole_height,
        );
    }

    // Region right of the hole.
    if hole_r < copy_width {
        blit_region(
            window,
            hole_r,
            hole_t,
            window.content_x + hole_r as i32,
            window.content_y + hole_t as i32,
            copy_width - hole_r,
            hole_height,
        );
    }

    // Region below the hole.
    if hole_b < copy_height {
        blit_region(
            window,
            0,
            hole_b,
            window.content_x,
            window.content_y + hole_b as i32,
            copy_width,
            copy_height - hole_b,
        );
    }
}

#[inline]
unsafe fn draw_close_glyph(x: i32, y: i32, color: u32) {
    for i in 0..9i32 {
        fill_rect(x + 4 + i, y + 3 + i, 1, 1, color);
        fill_rect(x + 12 - i, y + 3 + i, 1, 1, color);
    }
}

#[inline]
unsafe fn draw_maximize_glyph(x: i32, y: i32, color: u32) {
    fill_hline(x + 4, y + 3, 10, color);
    fill_hline(x + 4, y + 11, 10, color);
    fill_vline(x + 4, y + 3, 9, color);
    fill_vline(x + 13, y + 3, 9, color);
}

#[inline]
unsafe fn draw_minimize_glyph(x: i32, y: i32, color: u32) {
    fill_hline(x + 4, y + 10, 10, color);
}

/// Stage 3: MSDF title text and simple rectangular title-bar controls.
unsafe fn render_chrome(window: &GuiRenderWindow, theme: &GuiRenderTheme) {
    if window.decorated == 0 || window.has_title == 0 {
        return;
    }

    let titlebar_height = theme.titlebar_height.min(window.height);
    let text_size = theme.text_size.max(1);
    let text_height = (text_size.saturating_mul(120).saturating_add(99)) / 100;
    let text_y = window.y + ((titlebar_height.saturating_sub(text_height)) / 2) as i32;

    if !window.title.is_null() {
        let _ = gui_sdf_draw_screen(
            window.x + theme.border_width as i32 + 8,
            text_y,
            window.title,
            theme.title_text,
            text_size,
        );
    }

    let button_width = 18i32;
    let button_height = 16i32;
    let button_pad = 2i32;
    let border = theme.border_width as i32;
    let button_y = window.y + border + (titlebar_height as i32 - button_height) / 2;

    if window.has_close != 0 {
        let button_x = window.x + window.width as i32 - border - button_width - button_pad;
        let color = if window.hover_button == HOVER_CLOSE {
            theme.close_hover
        } else {
            theme.title_text
        };
        draw_close_glyph(button_x, button_y, color);
    }

    if window.has_minmax != 0 {
        let button_x = window.x + window.width as i32
            - border - 2 * (button_width + button_pad);
        let max_color = if window.hover_button == HOVER_MAXIMIZE {
            theme.max_hover
        } else {
            theme.title_text
        };
        draw_maximize_glyph(button_x, button_y, max_color);

        let min_x = button_x - button_width - button_pad;
        let min_color = if window.hover_button == HOVER_MINIMIZE {
            theme.min_hover
        } else {
            theme.title_text
        };
        draw_minimize_glyph(min_x, button_y, min_color);
    }

    if window.resizable != 0 && window.maximized == 0 && theme.resize_grip != 0 {
        let grip = theme.resize_grip.min(16) as i32;
        let gx = window.x + window.width as i32 - grip - 2;
        let gy = window.y + window.height as i32 - grip - 2;
        for offset in (3..grip).step_by(3) {
            let start_x = gx + offset;
            let start_y = gy + grip - 1;
            let end_x = gx + grip - 1;
            let end_y = gy + offset;
            let steps = (end_x - start_x).min(start_y - end_y).max(0);
            for i in 0..=steps {
                fill_rect(start_x + i, start_y - i, 1, 1, theme.border);
            }
        }
    }
}

/// Render one window in a fixed sequence of independently maintained stages.
/// The caller owns Window Manager locking and has already configured the
/// graphics driver's compositor clip rectangle.
#[no_mangle]
pub unsafe extern "C" fn gui_render_window(
    window: *const GuiRenderWindow,
    theme: *const GuiRenderTheme,
) -> i32 {
    if window.is_null() || theme.is_null() {
        return -1;
    }

    let window = &*window;
    let theme = &*theme;
    if window.width == 0 || window.height == 0 {
        return -1;
    }

    render_surface(window, theme);
    render_content(window, theme);
    render_chrome(window, theme);
    0
}
