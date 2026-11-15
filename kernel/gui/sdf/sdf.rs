//! Window-independent MSDF text renderer for AuraLite GUI.
//!
//! The atlas is shared by every GUI surface. The rasterizer only knows the
//! destination kind and its geometry; it never receives a window ID or reads
//! Window Manager state.

use core::ffi::{c_char, CStr};
use core::sync::atomic::{AtomicU8, Ordering};

extern "C" {
    fn gfx_blend_pixel(x: i32, y: i32, color: u32, alpha: u8);
    fn gfx_blend_pixels4(x: i32, y: i32, color: u32, alpha: *const u8);
}

#[derive(Copy, Clone)]
enum TextTarget {
    Buffer {
        pixels: *mut u32,
        width: u32,
        height: u32,
        stride: u32,
    },
    Screen,
}

const HEADER_BYTES: usize = 24;
const GLYPH_RECORD_BYTES: usize = 40;
const PIXEL_FORMAT_RGB888: u32 = 0;

// These values must match the msdf-atlas-gen configuration used to create
// atlas.bin for the Inter font.
const ATLAS_PX_RANGE: f32 = 8.0;
const ATLAS_ASCENT_EM: f32 = 0.95;
const ATLAS_LINE_HEIGHT_EM: f32 = 1.20;
const INV_255: f32 = 1.0 / 255.0;

#[used]
static ATLAS_RAW: &[u8] = include_bytes!("atlas.bin");

struct Atlas {
    width: u32,
    height: u32,
    glyphs: &'static [u8],
    glyph_count: u32,
    pixels: &'static [u8],
}

static INIT_STATE: AtomicU8 = AtomicU8::new(0);
const INIT_UNINIT: u8 = 0;
const INIT_OK: u8 = 1;
const INIT_FAILED: u8 = 2;
const INIT_BUSY: u8 = 3;

static mut ATLAS: Option<Atlas> = None;

#[inline]
fn floor_f(x: f32) -> f32 {
    let t = x as i64 as f32;
    if t > x { t - 1.0 } else { t }
}

#[inline]
fn ceil_f(x: f32) -> f32 {
    let t = x as i64 as f32;
    if t < x { t + 1.0 } else { t }
}

#[inline]
fn read_u32(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

#[inline]
fn read_f32(buf: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

fn parse_atlas() -> Option<Atlas> {
    let raw = ATLAS_RAW;
    if raw.len() < HEADER_BYTES || &raw[0..4] != b"MSDF" {
        return None;
    }
    if read_u32(raw, 4) != 1 {
        return None;
    }

    let width = read_u32(raw, 8);
    let height = read_u32(raw, 12);
    let glyph_count = read_u32(raw, 16);
    if read_u32(raw, 20) != PIXEL_FORMAT_RGB888 || width == 0 || height == 0 {
        return None;
    }

    let glyphs_off = HEADER_BYTES;
    let glyphs_end = glyphs_off.checked_add(
        (glyph_count as usize).checked_mul(GLYPH_RECORD_BYTES)?,
    )?;
    let pixels_len = (width as usize)
        .checked_mul(height as usize)?
        .checked_mul(3)?;
    let pixels_end = glyphs_end.checked_add(pixels_len)?;
    if raw.len() < pixels_end {
        return None;
    }

    Some(Atlas {
        width,
        height,
        glyphs: &raw[glyphs_off..glyphs_end],
        glyph_count,
        pixels: &raw[glyphs_end..pixels_end],
    })
}

fn atlas() -> Option<&'static Atlas> {
    match INIT_STATE.load(Ordering::Acquire) {
        INIT_OK => unsafe { (*core::ptr::addr_of!(ATLAS)).as_ref() },
        INIT_FAILED => None,
        INIT_BUSY => {
            for _ in 0..1024 {
                core::hint::spin_loop();
            }
            match INIT_STATE.load(Ordering::Acquire) {
                INIT_OK => unsafe { (*core::ptr::addr_of!(ATLAS)).as_ref() },
                _ => None,
            }
        }
        _ => match INIT_STATE.compare_exchange(
            INIT_UNINIT,
            INIT_BUSY,
            Ordering::AcqRel,
            Ordering::Acquire,
        ) {
            Ok(_) => {
                let parsed = parse_atlas();
                let ok = parsed.is_some();
                unsafe {
                    *core::ptr::addr_of_mut!(ATLAS) = parsed;
                }
                INIT_STATE.store(
                    if ok { INIT_OK } else { INIT_FAILED },
                    Ordering::Release,
                );
                unsafe { (*core::ptr::addr_of!(ATLAS)).as_ref() }
            }
            Err(_) => None,
        },
    }
}

struct Glyph {
    advance: f32,
    plane_l: f32,
    plane_b: f32,
    plane_r: f32,
    plane_t: f32,
    atlas_l: f32,
    atlas_b: f32,
    atlas_r: f32,
    atlas_t: f32,
    drawable: bool,
}

#[inline]
fn blank_glyph(advance: f32) -> Glyph {
    Glyph {
        advance,
        plane_l: 0.0,
        plane_b: 0.0,
        plane_r: 0.0,
        plane_t: 0.0,
        atlas_l: 0.0,
        atlas_b: 0.0,
        atlas_r: 0.0,
        atlas_t: 0.0,
        drawable: false,
    }
}

// msdf-atlas-gen does not guarantee sorted codepoints.
fn glyph_for(atlas: &Atlas, codepoint: u32) -> Option<Glyph> {
    for i in 0..atlas.glyph_count {
        let offset = i as usize * GLYPH_RECORD_BYTES;
        let record = &atlas.glyphs[offset..offset + GLYPH_RECORD_BYTES];
        if read_u32(record, 0) == codepoint {
            let glyph = Glyph {
                advance: read_f32(record, 4),
                plane_l: read_f32(record, 8),
                plane_b: read_f32(record, 12),
                plane_r: read_f32(record, 16),
                plane_t: read_f32(record, 20),
                atlas_l: read_f32(record, 24),
                atlas_b: read_f32(record, 28),
                atlas_r: read_f32(record, 32),
                atlas_t: read_f32(record, 36),
                drawable: true,
            };

            if !glyph.advance.is_finite()
                || !glyph.plane_l.is_finite()
                || !glyph.plane_b.is_finite()
                || !glyph.plane_r.is_finite()
                || !glyph.plane_t.is_finite()
                || !glyph.atlas_l.is_finite()
                || !glyph.atlas_b.is_finite()
                || !glyph.atlas_r.is_finite()
                || !glyph.atlas_t.is_finite()
                || glyph.advance < 0.0
                || glyph.atlas_l < 0.0
                || glyph.atlas_b < 0.0
                || glyph.atlas_r > atlas.width as f32
                || glyph.atlas_t > atlas.height as f32
            {
                return None;
            }

            // U+0020 is layout only: keep its advance, but never rasterize
            // atlas data or replace it with the visible '?' glyph.
            if codepoint == 0x20 {
                return Some(blank_glyph(glyph.advance));
            }

            // Other empty glyph records are also valid when both their plane
            // and atlas rectangles are degenerate.
            let no_plane = glyph.plane_r == glyph.plane_l
                && glyph.plane_t == glyph.plane_b;
            let no_atlas = glyph.atlas_r == glyph.atlas_l
                && glyph.atlas_t == glyph.atlas_b;
            if no_plane && no_atlas {
                return Some(blank_glyph(glyph.advance));
            }

            if glyph.plane_r <= glyph.plane_l
                || glyph.plane_t <= glyph.plane_b
                || glyph.atlas_r <= glyph.atlas_l
                || glyph.atlas_t <= glyph.atlas_b
            {
                return None;
            }

            return Some(glyph);
        }
    }
    None
}

#[inline]
fn space_advance(atlas: &Atlas) -> f32 {
    glyph_for(atlas, 0x20)
        .map(|glyph| glyph.advance)
        .filter(|advance| advance.is_finite() && *advance > 0.0)
        .unwrap_or(0.278)
}

#[inline]
fn glyph_for_char(atlas: &Atlas, ch: char) -> Option<Glyph> {
    match ch {
        // A missing space record must still advance the pen and must never
        // fall through to the replacement-glyph path.
        ' ' => Some(glyph_for(atlas, 0x20).unwrap_or_else(|| blank_glyph(0.278))),
        // Tabs are four normal spaces. They are layout, not visible glyphs.
        '\t' => Some(blank_glyph(space_advance(atlas) * 4.0)),
        '\r' => Some(blank_glyph(0.0)),
        ch if ch.is_whitespace() => Some(
            glyph_for(atlas, ch as u32)
                .unwrap_or_else(|| blank_glyph(space_advance(atlas))),
        ),
        _ => glyph_for(atlas, ch as u32)
            .or_else(|| glyph_for(atlas, 0xFFFD))
            .or_else(|| glyph_for(atlas, b'?' as u32)),
    }
}

#[inline]
fn pixel_rgb(atlas: &Atlas, x: u32, y: u32) -> (f32, f32, f32) {
    let offset = (y as usize * atlas.width as usize + x as usize) * 3;
    (
        atlas.pixels[offset] as f32 * INV_255,
        atlas.pixels[offset + 1] as f32 * INV_255,
        atlas.pixels[offset + 2] as f32 * INV_255,
    )
}

/// Bilinear sample in atlas pixel space, clamped to the glyph's own bounds.
#[inline]
fn sample_bilinear(
    atlas: &Atlas,
    fx: f32,
    fy: f32,
    gx0: u32,
    gy0: u32,
    gx1: u32,
    gy1: u32,
) -> (f32, f32, f32) {
    let max_x = if gx1 > gx0 { gx1 - 1 } else { gx0 };
    let max_y = if gy1 > gy0 { gy1 - 1 } else { gy0 };

    let cx = fx.clamp(gx0 as f32, max_x as f32);
    let cy = fy.clamp(gy0 as f32, max_y as f32);

    let x0 = (floor_f(cx) as u32).min(max_x);
    let y0 = (floor_f(cy) as u32).min(max_y);
    let x1 = (x0 + 1).min(max_x);
    let y1 = (y0 + 1).min(max_y);
    let tx = cx - x0 as f32;
    let ty = cy - y0 as f32;

    let (r00, g00, b00) = pixel_rgb(atlas, x0, y0);
    let (r10, g10, b10) = pixel_rgb(atlas, x1, y0);
    let (r01, g01, b01) = pixel_rgb(atlas, x0, y1);
    let (r11, g11, b11) = pixel_rgb(atlas, x1, y1);

    #[inline(always)]
    fn lerp(a: f32, b: f32, t: f32) -> f32 {
        a + (b - a) * t
    }

    (
        lerp(lerp(r00, r10, tx), lerp(r01, r11, tx), ty),
        lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty),
        lerp(lerp(b00, b10, tx), lerp(b01, b11, tx), ty),
    )
}

#[inline]
fn median3(a: f32, b: f32, c: f32) -> f32 {
    if a > b {
        if b > c { b } else if a > c { c } else { a }
    } else if a > c {
        a
    } else if b > c {
        c
    } else {
        b
    }
}

#[inline]
unsafe fn blend_four(
    target: TextTarget,
    x: i32,
    y: i32,
    color: u32,
    alpha: &[u8; 4],
) {
    match target {
        TextTarget::Buffer { pixels, width, height, stride } => {
            let _ = crate::drawing::gui_rust_blend_pixels4(
                pixels,
                width,
                height,
                stride,
                x,
                y,
                color,
                alpha.as_ptr(),
            );
        }
        TextTarget::Screen => {
            gfx_blend_pixels4(x, y, color, alpha.as_ptr());
        }
    }
}

#[inline]
unsafe fn blend_one(
    target: TextTarget,
    x: i32,
    y: i32,
    color: u32,
    alpha: u8,
) {
    match target {
        TextTarget::Buffer { pixels, width, height, stride } => {
            let _ = crate::drawing::gui_rust_blend_pixel(
                pixels, width, height, stride, x, y, color, alpha,
            );
        }
        TextTarget::Screen => {
            gfx_blend_pixel(x, y, color, alpha);
        }
    }
}

fn draw_glyph(
    target: TextTarget,
    atlas: &Atlas,
    glyph: &Glyph,
    pen_x: f32,
    baseline_y: f32,
    color: u32,
    px_size: f32,
) {
    if !glyph.drawable {
        return;
    }

    let scale = px_size;

    let x0 = pen_x + glyph.plane_l * scale;
    let y0 = baseline_y - glyph.plane_t * scale;
    let x1 = pen_x + glyph.plane_r * scale;
    let y1 = baseline_y - glyph.plane_b * scale;

    let quad_w = floor_f((x1 - x0) + 0.5) as i32;
    let quad_h = floor_f((y1 - y0) + 0.5) as i32;
    if quad_w <= 0 || quad_h <= 0 || quad_w > 4096 || quad_h > 4096 {
        return;
    }

    let x0i = floor_f(x0 + 0.5) as i32;
    let y0i = floor_f(y0 + 0.5) as i32;

    let atlas_h = atlas.height as f32;
    let gx0 = floor_f(glyph.atlas_l.max(0.0)) as u32;
    let gx1 = ((ceil_f(glyph.atlas_r) as u32).max(gx0.saturating_add(1)))
        .min(atlas.width);

    // Atlas Y grows down: top = atlas_h - atlas_t, bottom = atlas_h - atlas_b.
    let gy_top = floor_f((atlas_h - glyph.atlas_t).max(0.0)) as u32;
    let gy_bottom = ceil_f(atlas_h - glyph.atlas_b) as u32;
    let gy0 = gy_top.min(atlas.height);
    let gy1 = gy_bottom.min(atlas.height).max(gy0.saturating_add(1));

    if gx1 <= gx0 || gy1 <= gy0 {
        return;
    }

    let atlas_glyph_w = (gx1 - gx0) as f32;
    let atlas_glyph_h = (gy1 - gy0) as f32;
    let screen_px_range = (ATLAS_PX_RANGE
        * (quad_w as f32 / atlas_glyph_w).max(quad_h as f32 / atlas_glyph_h))
        .max(1.0);

    let dfx = (glyph.atlas_r - glyph.atlas_l) / quad_w as f32;
    let fx_start = glyph.atlas_l + dfx * 0.5;
    let dfy = (gy1 - gy0) as f32 / quad_h as f32;
    let fy_start = gy0 as f32 + dfy * 0.5;

    let mut alpha_buf = [0u8; 4];

    for py in 0..quad_h {
        let fy = fy_start + dfy * py as f32;
        let dy = y0i + py;
        let mut px = 0i32;

        while px + 4 <= quad_w {
            let mut any = false;
            let mut k = 0usize;
            while k < 4 {
                let fx = fx_start + dfx * (px + k as i32) as f32;
                let (r, g, b) = sample_bilinear(atlas, fx, fy, gx0, gy0, gx1, gy1);
                let distance = median3(r, g, b);
                let coverage = ((distance - 0.5) * screen_px_range + 0.5)
                    .clamp(0.0, 1.0);
                let value = (coverage * 255.0 + 0.5) as u8;
                alpha_buf[k] = value;
                any |= value != 0;
                k += 1;
            }

            if any {
                unsafe {
                    blend_four(target, x0i + px, dy, color, &alpha_buf);
                }
            }
            px += 4;
        }

        while px < quad_w {
            let fx = fx_start + dfx * px as f32;
            let (r, g, b) = sample_bilinear(atlas, fx, fy, gx0, gy0, gx1, gy1);
            let distance = median3(r, g, b);
            let coverage = ((distance - 0.5) * screen_px_range + 0.5)
                .clamp(0.0, 1.0);
            let alpha = (coverage * 255.0 + 0.5) as u8;
            if alpha != 0 {
                unsafe {
                    blend_one(target, x0i + px, dy, color, alpha);
                }
            }
            px += 1;
        }
    }
}

fn draw_text(
    target: TextTarget,
    atlas: &Atlas,
    x: i32,
    y: i32,
    text: &str,
    color: u32,
    px_size: u32,
) {
    let px_size_f = px_size as f32;
    let line_height = px_size_f * ATLAS_LINE_HEIGHT_EM;
    let ascender = px_size_f * ATLAS_ASCENT_EM;
    let mut pen_x = x as f32;
    let mut baseline_y = y as f32 + ascender;

    for ch in text.chars() {
        if ch == '\n' {
            pen_x = x as f32;
            baseline_y += line_height;
            continue;
        }

        if let Some(glyph) = glyph_for_char(atlas, ch) {
            draw_glyph(target, atlas, &glyph, pen_x, baseline_y, color, px_size_f);
            pen_x += glyph.advance * px_size_f;
        }
    }
}

fn measure_text(atlas: &Atlas, text: &str, px_size: u32) -> (u32, u32) {
    let scale = px_size as f32;
    let mut line_width = 0.0f32;
    let mut max_width = 0.0f32;
    let mut lines = 1u32;

    for ch in text.chars() {
        if ch == '\n' {
            if line_width > max_width {
                max_width = line_width;
            }
            line_width = 0.0;
            lines = lines.saturating_add(1);
        } else if let Some(glyph) = glyph_for_char(atlas, ch) {
            line_width += glyph.advance * scale;
        }
    }

    if line_width > max_width {
        max_width = line_width;
    }

    let width = ceil_f(max_width).max(0.0) as u32;
    let height = ceil_f(scale * ATLAS_LINE_HEIGHT_EM * lines as f32).max(0.0) as u32;
    (width, height)
}

/// Draw UTF-8 MSDF text into an arbitrary caller-owned 32-bit pixel buffer.
/// Coordinates are relative to that buffer. The caller remains responsible
/// for window ownership and marking its content dirty.
#[no_mangle]
pub unsafe extern "C" fn gui_sdf_draw_buffer(
    pixels: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    text_ptr: *const c_char,
    color: u32,
    px_size: u32,
) -> i32 {
    if pixels.is_null() || width == 0 || height == 0 || stride < width
        || text_ptr.is_null() || px_size == 0 || px_size > 4096
    {
        return -1;
    }

    let Some(atlas) = atlas() else { return -1; };
    let Ok(text) = CStr::from_ptr(text_ptr).to_str() else { return -1; };

    draw_text(
        TextTarget::Buffer { pixels, width, height, stride },
        atlas,
        x,
        y,
        text,
        color,
        px_size,
    );
    0
}

/// Draw UTF-8 MSDF text into the framebuffer driver's back buffer.
/// Screen clipping and native framebuffer color conversion are handled by
/// graphics.c. Coordinates are screen-space and y is the top of the line box.
#[no_mangle]
pub unsafe extern "C" fn gui_sdf_draw_screen(
    x: i32,
    y: i32,
    text_ptr: *const c_char,
    color: u32,
    px_size: u32,
) -> i32 {
    if text_ptr.is_null() || px_size == 0 || px_size > 4096 {
        return -1;
    }

    let Some(atlas) = atlas() else { return -1; };
    let Ok(text) = CStr::from_ptr(text_ptr).to_str() else { return -1; };

    draw_text(TextTarget::Screen, atlas, x, y, text, color, px_size);
    0
}

/// Measure UTF-8 text using the same Inter atlas and pixel size as drawing.
/// Either output pointer may be null.
#[no_mangle]
pub unsafe extern "C" fn gui_sdf_measure_text(
    text_ptr: *const c_char,
    px_size: u32,
    out_width: *mut u32,
    out_height: *mut u32,
) -> i32 {
    if text_ptr.is_null() || px_size == 0 || px_size > 4096
        || (out_width.is_null() && out_height.is_null()) {
        return -1;
    }

    let Some(atlas) = atlas() else { return -1; };
    let Ok(text) = CStr::from_ptr(text_ptr).to_str() else { return -1; };
    let (width, height) = measure_text(atlas, text, px_size);

    if !out_width.is_null() {
        out_width.write(width);
    }
    if !out_height.is_null() {
        out_height.write(height);
    }
    0
}
