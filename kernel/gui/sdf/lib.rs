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
    fn gui_blend_pixels4(wid: i32, x: i32, y: i32, color: u32, alpha: *const u8) -> i32;
    
}

#[no_mangle]
pub extern "C" fn gui_dirty_is_empty() -> i32 {
    dirty_lock();
    let empty = unsafe { DIRTY_COUNT == 0 } as i32;
    dirty_unlock();
    empty
}

// ---------------------------------------------------------------------------
// Math shims (variant 1: truncate-toward-zero + one correction).
//
// `x as i64 as f32` rounds toward zero.  Floor is that value minus one when
// it overshoots `x` (only happens for negative fractional `x`); ceil is plus
// one when it undershoots.  Both lower to fptosi + sitofp + a compare, with
// no libc, no libm, no compiler_builtins math.
//
// Assumes finite `x` in i64 range.  Every call site in this file feeds
// atlas coordinates and screen coords (0..8192), so the assumption holds.
// ---------------------------------------------------------------------------

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

const HEADER_BYTES: usize = 24;
const GLYPH_RECORD_BYTES: usize = 40;
const PIXEL_FORMAT_RGB888: u32 = 0;

// MUST match the atlas generator's `-pxrange`.  msdf-atlas-gen's default is
// 4; if atlas.bin was regenerated with a different value, change this.
const ATLAS_PX_RANGE: f32 = 8.0;
const ATLAS_ASCENT_EM: f32 = 0.95;      // Inter's OS/2 ascender ≈ 0.969
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
const INIT_OK:     u8 = 1;
const INIT_FAILED: u8 = 2;
const INIT_BUSY:   u8 = 3;

static mut ATLAS: Option<Atlas> = None;

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

// Linear scan: msdf-atlas-gen doesn't guarantee sorted codepoints.
fn glyph_for(a: &Atlas, cp: u32) -> Option<Glyph> {
    for i in 0..a.glyph_count {
        let off = (i as usize) * GLYPH_RECORD_BYTES;
        let rec = &a.glyphs[off..off + GLYPH_RECORD_BYTES];
        if read_u32(rec, 0) == cp {
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

#[inline]
fn pixel_rgb(a: &Atlas, x: u32, y: u32) -> (f32, f32, f32) {
    let off = ((y * a.width + x) * 3) as usize;
    (
        a.pixels[off]     as f32 * INV_255,
        a.pixels[off + 1] as f32 * INV_255,
        a.pixels[off + 2] as f32 * INV_255,
    )
}

/// Bilinear sample in ATLAS PIXEL SPACE, clamped to [gx0, gx1-1] × [gy0, gy1-1].
/// This is the fix for the "blurry" edges: the atlas is packed without
/// padding, so an unclamped fetch leaks the neighbouring glyph's MSDF values
/// into the edge pixels of the current glyph and corrupts `median3`.
#[inline]
fn sample_bilinear(
    a: &Atlas,
    fx: f32,
    fy: f32,
    gx0: u32,
    gy0: u32,
    gx1: u32, // exclusive
    gy1: u32, // exclusive
) -> (f32, f32, f32) {
    // Highest valid pixel index inside the glyph.
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

    let (r00, g00, b00) = pixel_rgb(a, x0, y0);
    let (r10, g10, b10) = pixel_rgb(a, x1, y0);
    let (r01, g01, b01) = pixel_rgb(a, x0, y1);
    let (r11, g11, b11) = pixel_rgb(a, x1, y1);

    #[inline(always)]
    fn lerp(a: f32, b: f32, t: f32) -> f32 { a + (b - a) * t }

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

    // Plane quad in continuous screen space.
    let x0 = pen_x + g.plane_l * scale;
    let y0 = baseline_y - g.plane_t * scale;
    let x1 = pen_x + g.plane_r * scale;
    let y1 = baseline_y - g.plane_b * scale;

    let qw = floor_f((x1 - x0) + 0.5) as i32;
    let qh = floor_f((y1 - y0) + 0.5) as i32;
    if qw <= 0 || qh <= 0 || qw > 4096 || qh > 4096 { return; }

    // Snap quad origin to integers.  `as i32` truncates toward zero, which
    // shifts partially-offscreen glyphs by one pixel; floor matches the
    // convention used everywhere else.
    let x0i = floor_f(x0 + 0.5) as i32;
    let y0i = floor_f(y0 + 0.5) as i32;

    // Glyph bbox in ATLAS PIXEL SPACE (Y grows downward in the atlas array).
    let aw = atlas.width  as f32;
    let ah = atlas.height as f32;
    let _ = aw;

    let gx0 = floor_f(g.atlas_l.max(0.0)) as u32;
    let gx1 = ((ceil_f(g.atlas_r) as u32).max(gx0 + 1)).min(atlas.width);

    // atlas_b/t are stored in text coords where atlas_t > atlas_b.
    // In the array (Y down), the top row of the glyph is `ah - atlas_t`
    // and the bottom row is `ah - atlas_b`.
    let gy_top = floor_f((ah - g.atlas_t).max(0.0)) as u32;
    let gy_bot = ceil_f(ah - g.atlas_b) as u32;
    let gy0 = gy_top;
    let gy1 = gy_bot.min(atlas.height).max(gy0 + 1);

    if gx1 <= gx0 || gy1 <= gy0 { return; }

    let atlas_glyph_w = (gx1 - gx0) as f32;
    let atlas_glyph_h = (gy1 - gy0) as f32;

    // Same formula as msdf-atlas-gen's shader, without the spurious 0.5.
    let screen_px_range = (ATLAS_PX_RANGE
        * (qw as f32 / atlas_glyph_w).max(qh as f32 / atlas_glyph_h))
        .max(1.0);

    // Per-glyph linear ramps (no division in the inner loop).
    // fx(px) = atlas_l + (atlas_r - atlas_l) * ((px as f32) + 0.5) / qw
    // fy(py) = gy0     + (gy1     - gy0    ) * ((py as f32) + 0.5) / qh
    let dfx = (g.atlas_r - g.atlas_l) / qw as f32;
    let fx_start = g.atlas_l + dfx * 0.5;

    let dfy = (gy1 - gy0) as f32 / qh as f32;
    let fy_start = gy0 as f32 + dfy * 0.5;

    let mut alpha_buf = [0u8; 4];

    for py in 0..qh {
        let fy = fy_start + dfy * py as f32;
        let dy = y0i + py;

        // ---- 4-pixel SIMD batch ----
        let mut px = 0i32;
        while px + 4 <= qw {
            let mut any = false;
            let mut k: usize = 0;
            while k < 4 {
                let fx = fx_start + dfx * (px + k as i32) as f32;
                let (r, g_ch, b) = sample_bilinear(atlas, fx, fy, gx0, gy0, gx1, gy1);
                let sd = median3(r, g_ch, b);
                let a = ((sd - 0.5) * screen_px_range + 0.5).clamp(0.0, 1.0);
                let av = (a * 255.0 + 0.5) as u8;
                alpha_buf[k] = av;
                if av != 0 { any = true; }
                k += 1;
            }
            if any {
                let dx = x0i + px;
                unsafe {
                    gui_blend_pixels4(wid, dx, dy, color, alpha_buf.as_ptr());
                }
            }
            px += 4;
        }

        // ---- scalar tail (0..3 px) ----
        while px < qw {
            let fx = fx_start + dfx * px as f32;
            let (r, g_ch, b) = sample_bilinear(atlas, fx, fy, gx0, gy0, gx1, gy1);
            let sd = median3(r, g_ch, b);
            let a = ((sd - 0.5) * screen_px_range + 0.5).clamp(0.0, 1.0);
            let av = (a * 255.0 + 0.5) as u8;
            if av != 0 {
                let dx = x0i + px;
                unsafe { gui_blend_pixel(wid, dx, dy, color, av); }
            }
            px += 1;
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

// ---------------------------------------------------------------------------
// Dirty rectangle tracker (replaces the C dirty_rects[] / dirty_count).
//
// C used ONE bounding box for all dirty regions, so moving the cursor from
// one corner to the other damaged the whole diagonal.  Here the rectangles
// stay disjoint: overlapping/touching ones are merged, and the compositor
// renders each rectangle separately.
//
// The tracker is shared between the compositor thread (which snapshots and
// renders) and syscall threads (which add rects via gui_invalidate_rect).
// A small spinlock serialises them.
// ---------------------------------------------------------------------------

use core::sync::atomic::AtomicBool;

#[derive(Copy, Clone)]
#[repr(C)]
pub struct DirtyRect {
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
}

const MAX_DIRTY_RECTS: usize = 64;
const MERGE_PAD: i64 = 4;

static DIRTY_LOCK: AtomicBool = AtomicBool::new(false);

#[inline]
fn dirty_lock() {
    while DIRTY_LOCK.compare_exchange_weak(
        false, true, Ordering::Acquire, Ordering::Relaxed
    ).is_err() {
        core::hint::spin_loop();
    }
}

#[inline]
fn dirty_unlock() {
    DIRTY_LOCK.store(false, Ordering::Release);
}

static mut DIRTY_RECTS: [DirtyRect; MAX_DIRTY_RECTS] = [
    DirtyRect { x: 0, y: 0, w: 0, h: 0 }; MAX_DIRTY_RECTS
];
static mut DIRTY_COUNT: usize = 0;
static mut DIRTY_OVERFLOW: bool = false;

/// Clear all pending rects.  Called from gui_request_redraw() before a
/// full-screen render.
#[no_mangle]
pub extern "C" fn gui_dirty_reset() {
    dirty_lock();
    unsafe {
        DIRTY_COUNT = 0;
        DIRTY_OVERFLOW = false;
    }
    dirty_unlock();
}

/// Add a rect.  Returns 0 on success, -1 on overflow (caller should fall
/// back to a full redraw).
#[no_mangle]
pub extern "C" fn gui_dirty_add(x: i32, y: i32, w: u32, h: u32) -> i32 {
    if w == 0 || h == 0 { return 0; }
    dirty_lock();
    let result = unsafe { add_unsafe(x, y, w, h) };
    dirty_unlock();
    result
}

unsafe fn add_unsafe(x: i32, y: i32, w: u32, h: u32) -> i32 {
    if DIRTY_OVERFLOW { return -1; }

    let nx1 = x as i64 + w as i64;
    let ny1 = y as i64 + h as i64;

    // Try to merge with an existing rect (overlap or within MERGE_PAD).
    for i in 0..DIRTY_COUNT {
        let r = &mut DIRTY_RECTS[i];
        let rx1 = r.x as i64 + r.w as i64;
        let ry1 = r.y as i64 + r.h as i64;
        if x as i64 <= rx1 + MERGE_PAD && nx1 + MERGE_PAD >= r.x as i64
            && y as i64 <= ry1 + MERGE_PAD && ny1 + MERGE_PAD >= r.y as i64
        {
            let mx = r.x.min(x) as i64;
            let my = r.y.min(y) as i64;
            let mx1 = rx1.max(nx1);
            let my1 = ry1.max(ny1);
            r.x = mx as i32;
            r.y = my as i32;
            r.w = (mx1 - mx) as u32;
            r.h = (my1 - my) as u32;
            // The merged rect may now touch others; cascade.
            merge_all_unsafe();
            return 0;
        }
    }

    if DIRTY_COUNT >= MAX_DIRTY_RECTS {
        DIRTY_OVERFLOW = true;
        DIRTY_COUNT = 0;
        return -1;
    }
    DIRTY_RECTS[DIRTY_COUNT] = DirtyRect { x, y, w, h };
    DIRTY_COUNT += 1;
    0
}

unsafe fn merge_all_unsafe() {
    let mut changed = true;
    while changed {
        changed = false;
        let mut i = 0;
        while i < DIRTY_COUNT {
            let mut j = i + 1;
            while j < DIRTY_COUNT {
                let ax = DIRTY_RECTS[i].x as i64;
                let ay = DIRTY_RECTS[i].y as i64;
                let ax1 = ax + DIRTY_RECTS[i].w as i64;
                let ay1 = ay + DIRTY_RECTS[i].h as i64;
                let bx = DIRTY_RECTS[j].x as i64;
                let by = DIRTY_RECTS[j].y as i64;
                let bx1 = bx + DIRTY_RECTS[j].w as i64;
                let by1 = by + DIRTY_RECTS[j].h as i64;
                if ax <= bx1 + MERGE_PAD && ax1 + MERGE_PAD >= bx
                    && ay <= by1 + MERGE_PAD && ay1 + MERGE_PAD >= by
                {
                    let mx = DIRTY_RECTS[i].x.min(DIRTY_RECTS[j].x) as i64;
                    let my = DIRTY_RECTS[i].y.min(DIRTY_RECTS[j].y) as i64;
                    let mx1 = ax1.max(bx1);
                    let my1 = ay1.max(by1);
                    DIRTY_RECTS[i].x = mx as i32;
                    DIRTY_RECTS[i].y = my as i32;
                    DIRTY_RECTS[i].w = (mx1 - mx) as u32;
                    DIRTY_RECTS[i].h = (my1 - my) as u32;
                    DIRTY_RECTS[j] = DIRTY_RECTS[DIRTY_COUNT - 1];
                    DIRTY_COUNT -= 1;
                    changed = true;
                } else {
                    j += 1;
                }
            }
            i += 1;
        }
    }
}

/// Atomically copy all pending rects into `out` and clear the internal list.
/// Returns the number copied (0 when nothing is pending).  The compositor
/// calls this once per tick, then iterates the copy with no locking.
#[no_mangle]
pub extern "C" fn gui_dirty_snapshot(out: *mut DirtyRect, max: usize) -> usize {
    if out.is_null() || max == 0 { return 0; }
    dirty_lock();
    let n = unsafe { DIRTY_COUNT.min(max) };
    for i in 0..n {
        unsafe {
            core::ptr::write(out.add(i), DIRTY_RECTS[i]);
        }
    }
    unsafe {
        DIRTY_COUNT = 0;
        DIRTY_OVERFLOW = false;
    }
    dirty_unlock();
    n
}