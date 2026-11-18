//! Desktop wallpaper rendering.
//!
//! This module currently preserves the existing gradient-and-stripe desktop
//! background. Image loading and wallpaper placement modes can be added here
//! later without moving desktop rendering back into the C window manager.

use super::{screen_height, screen_width, theme};

extern "C" {
    fn gfx_gradient_v(
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        top: u32,
        bottom: u32,
    );
    fn gfx_draw_line(x0: u32, y0: u32, x1: u32, y1: u32, color: u32);
}

/// Draw the wallpaper into the active compositor clip.
///
/// The C compositor remains responsible for selecting full-screen or dirty
/// clips and for presenting the completed frame.
#[no_mangle]
pub extern "C" fn desktop_wallpaper_render() {
    let active = theme();
    let width = screen_width();
    let height = screen_height();

    unsafe {
        gfx_gradient_v(
            0,
            0,
            width,
            height,
            active.desktop_top,
            active.desktop_bot,
        );
    }

    let mut y = 0u32;
    while y < height {
        unsafe {
            gfx_draw_line(0, y, width, y, 0x00000018);
        }

        let Some(next_y) = y.checked_add(48) else {
            break;
        };
        y = next_y;
    }
}
