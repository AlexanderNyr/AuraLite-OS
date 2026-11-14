// MSDF text renderer, kernel-space, no_std, no alloc.

#![no_std]

use core::ffi::{c_char, CStr};
use core::panic::PanicInfo;
use core::sync::atomic::{AtomicU8, Ordering};

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

extern "C" {
    fn gui_blend_pixel(wid: i32, x: i32, y: i32, color: u32, a: u8) -> i32;
}

const HEADER_BYTES: usize = 24;
const GLYPH_RECORD_BYTES: usize = 40;
const PIXEL_FORMAT_RGB888: u32 = 0;

const ATLAS_PX_RANGE:       f32 = 8.0;
const ATLAS_ASCENT_EM:      f32 = 0.95; // приблизительный ascender для Inter
const ATLAS_LINE_HEIGHT_EM: f32 = 1.20;

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
const INIT_OK:     u8 = 1;
const INIT_FAILED: u8 = 2;
const INIT_BUSY:   u8 = 3;

static mut ATLAS: Option<Atlas> = None;

fn read_u32(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}
fn read_f32(buf: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

fn parse_atlas() -> Option<Atlas> {
    let raw = ATLAS_RAW;
    if raw.len() < HEADER_BYTES { return None; }
    if &raw[0..4] != b"MSDF" { return None; }
    if read_u32(raw, 4) != 1 { return None; }
    let width       = read_u32(raw,  8);
    let height      = read_u32(raw, 12);
    let glyph_count = read_u32(raw, 16);
    if read_u32(raw, 20) != PIXEL_FORMAT_RGB888 { return None; }
    if width == 0 || height == 0 { return None; }

    let glyphs_off = HEADER_BYTES;
    let glyphs_end = glyphs_off
        .checked_add((glyph_count as usize).checked_mul(GLYPH_RECORD_BYTES)?)?;
    let pixels_len = (width as usize)
        .checked_mul(height as usize)?
        .checked_mul(3)?;
    let pixels_end = glyphs_end.checked_add(pixels_len)?;
    if raw.len() < pixels_end { return None; }

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
        INIT_OK     => unsafe { (*core::ptr::addr_of!(ATLAS)).as_ref() },
        INIT_FAILED => None,
        INIT_BUSY   => {
            for _ in 0..1024 { core::hint::spin_loop(); }
            match INIT_STATE.load(Ordering::Acquire) {
                INIT_OK => unsafe { (*core::ptr::addr_of!(ATLAS)).as_ref() },
                _ => None,
            }
        }
        _ => match INIT_STATE.compare_exchange(
            INIT_UNINIT, INIT_BUSY, Ordering::AcqRel, Ordering::Acquire)
        {
            Ok(_) => {
                let parsed = parse_atlas();
                let ok = parsed.is_some();
                unsafe { *core::ptr::addr_of_mut!(ATLAS) = parsed; }
                INIT_STATE.store(if ok { INIT_OK } else { INIT_FAILED },
                                 Ordering::Release);
                unsafe { (*core::ptr::addr_of!(ATLAS)).as_ref() }
            }
            Err(_) => None,
        },
    }
}

struct Glyph {
    advance: f32,
    plane_l: f32, plane_b: f32, plane_r: f32, plane_t: f32,
    atlas_l: f32, atlas_b: f32, atlas_r: f32, atlas_t: f32,
}

// ИСПОЛЬЗУЕМ ЛИНЕЙНЫЙ ПОИСК: массив из msdf-atlas-gen не отсортирован!
fn glyph_for(a: &Atlas, cp: u32) -> Option<Glyph> {
    for i in 0..a.glyph_count {
        let off = (i as usize) * GLYPH_RECORD_BYTES;
        let rec = &a.glyphs[off..off + GLYPH_RECORD_BYTES];
        let c = read_u32(rec, 0);
        if c == cp {
            return Some(Glyph {
                advance: read_f32(rec,  4),
                plane_l: read_f32(rec,  8),
                plane_b: read_f32(rec, 12),
                plane_r: read_f32(rec, 16),
                plane_t: read_f32(rec, 20),
                atlas_l: read_f32(rec, 24),
                atlas_b: read_f32(rec, 28),
                atlas_r: read_f32(rec, 32),
                atlas_t: read_f32(rec, 36),
            });
        }
    }
    None
}

fn pixel_rgb(a: &Atlas, x: u32, y: u32) -> (f32, f32, f32) {
    let x = x.min(a.width.saturating_sub(1)) as usize;
    let y = y.min(a.height.saturating_sub(1)) as usize;
    let off = (y * a.width as usize + x) * 3;
    (
        a.pixels[off] as f32 / 255.0,
        a.pixels[off + 1] as f32 / 255.0,
        a.pixels[off + 2] as f32 / 255.0,
    )
}

fn sample_bilinear(a: &Atlas, u: f32, v: f32) -> (f32, f32, f32) {
    let fx = u.clamp(0.0, 1.0) * (a.width  as f32 - 1.0);
    let fy = v.clamp(0.0, 1.0) * (a.height as f32 - 1.0);

    let x0i = fx as u32;
    let y0i = fy as u32;
    let x0 = x0i.min(a.width  - 1);
    let y0 = y0i.min(a.height - 1);
    let x1 = (x0 + 1).min(a.width  - 1);
    let y1 = (y0 + 1).min(a.height - 1);
    let tx = fx - x0i as f32;
    let ty = fy - y0i as f32;

    let (r00, g00, b00) = pixel_rgb(a, x0, y0);
    let (r10, g10, b10) = pixel_rgb(a, x1, y0);
    let (r01, g01, b01) = pixel_rgb(a, x0, y1);
    let (r11, g11, b11) = pixel_rgb(a, x1, y1);

    let lerp = |a: f32, b: f32, t: f32| a + (b - a) * t;
    let r = lerp(lerp(r00, r10, tx), lerp(r01, r11, tx), ty);
    let g = lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty);
    let b = lerp(lerp(b00, b10, tx), lerp(b01, b11, tx), ty);
    (r, g, b)
}

fn median3(a: f32, b: f32, c: f32) -> f32 {
    if a > b {
        if b > c { b } else if a > c { c } else { a }
    } else {
        if a > c { a } else if b > c { c } else { b }
    }
}

fn draw_glyph(
    wid: i32,
    atlas: &Atlas,
    g: &Glyph,
    pen_x: f32,
    baseline_y: f32,
    color: u32,
    px_size: f32,
) {
    let scale = px_size;

    let x0 = pen_x + g.plane_l * scale;
    let y0 = baseline_y - g.plane_t * scale;
    let x1 = pen_x + g.plane_r * scale;
    let y1 = baseline_y - g.plane_b * scale;

    let qw = ((x1 - x0) + 0.5) as i32;
    let qh = ((y1 - y0) + 0.5) as i32;
    if qw <= 0 || qh <= 0 || qw > 4096 || qh > 4096 { return; }

    let aw = atlas.width  as f32;
    let ah = atlas.height as f32;
    let u0 = g.atlas_l / aw;
    let u1 = g.atlas_r / aw;
    let v0 = 1.0 - g.atlas_t / ah;
    let v1 = 1.0 - g.atlas_b / ah;

    let atlas_glyph_w = (g.atlas_r - g.atlas_l).abs().max(1.0);
    let atlas_glyph_h = (g.atlas_t - g.atlas_b).abs().max(1.0);

    let screen_px_range = (
        0.5 * ATLAS_PX_RANGE *
        (qw as f32 / atlas_glyph_w)
            .max(qh as f32 / atlas_glyph_h)
    ).max(1.0);

    for py in 0..qh {
        let ty = (py as f32 + 0.5) / qh as f32;
        let v = v0 + (v1 - v0) * ty;
        for px in 0..qw {
            let tx = (px as f32 + 0.5) / qw as f32;
            let u = u0 + (u1 - u0) * tx;
            let (r, g_ch, b) = sample_bilinear(atlas, u, v);
            let sd = median3(r, g_ch, b);
            let alpha_f = ((sd - 0.5) * screen_px_range + 0.5).clamp(0.0, 1.0);
            let alpha = (alpha_f * 255.0) as u8;
            if alpha == 0 { continue; }
            
            let dx = (x0 + 0.5) as i32 + px;
            let dy = (y0 + 0.5) as i32 + py;
            unsafe { gui_blend_pixel(wid, dx, dy, color, alpha); }
        }
    }
}

#[no_mangle]
pub extern "C" fn sdf_draw_text(
    wid: i32,
    x: i32,
    y: i32,
    s: *const c_char,
    color: u32,
    px_size: u32,
) -> i32 {
    if s.is_null() || px_size == 0 { return -1; }
    let Some(atlas) = atlas() else { return -1; };
    let Ok(text) = unsafe { CStr::from_ptr(s) }.to_str() else { return -1; };

    let px_size_f = px_size as f32;
    let scale     = px_size_f;
    let line_h    = px_size_f * ATLAS_LINE_HEIGHT_EM;
    let ascender  = px_size_f * ATLAS_ASCENT_EM;

    let mut pen_x      = x as f32;
    let mut baseline_y = y as f32 + ascender;

    for ch in text.chars() {
        if ch == '\n' {
            pen_x = x as f32;
            baseline_y += line_h;
            continue;
        }
        if let Some(g) = glyph_for(atlas, ch as u32) {
            draw_glyph(wid, atlas, &g, pen_x, baseline_y, color, px_size_f);
            pen_x += g.advance * scale;
        }
    }
    0
}