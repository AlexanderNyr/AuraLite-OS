//! Window-independent MSDF text renderer for AuraLite GUI.
//!
//! The atlas is shared by every GUI surface. The rasterizer only knows the
//! destination kind and its geometry; it never receives a window ID or reads
//! Window Manager state.
//!
//! On x86_64 CPUs with SSE4.1, four horizontal MSDF samples are interpolated
//! together using integer SIMD. The scalar path remains available for older
//! CPUs and produces the same fixed-point interpolation results.

use core::ffi::{c_char, CStr};
use core::sync::atomic::{AtomicU8, Ordering};

#[cfg(target_arch = "x86_64")]
use core::arch::x86_64::*;

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
const ATLAS_ASCENT_EM: f32 = 0.96875;
const ATLAS_LINE_HEIGHT_EM: f32 = 1.2109375;

#[used]
static ATLAS_RAW: &[u8] = include_bytes!("atlas.bin");

struct Atlas {
    width: u32,
    height: u32,
    glyphs: &'static [u8],
    glyph_count: u32,
    pixels: &'static [u8],
    ascii: [Option<Glyph>; 128],
}

static INIT_STATE: AtomicU8 = AtomicU8::new(0);
const INIT_UNINIT: u8 = 0;
const INIT_OK: u8 = 1;
const INIT_FAILED: u8 = 2;
const INIT_BUSY: u8 = 3;

static mut ATLAS: Option<Atlas> = None;

// CPUID leaf 1, ECX bit 19 advertises SSE4.1. Detection is cached because
// text can be drawn many times per frame.
static SSE41_STATE: AtomicU8 = AtomicU8::new(0);
const SSE41_SUPPORTED: u8 = 1;
const SSE41_UNSUPPORTED: u8 = 2;

#[inline]
fn sse41_available() -> bool {
    #[cfg(target_arch = "x86_64")]
    {
        let cpu_supported = match SSE41_STATE.load(Ordering::Relaxed) {
            SSE41_SUPPORTED => true,
            SSE41_UNSUPPORTED => false,
            _ => {
                let supported = unsafe {
                    (core::arch::x86_64::__cpuid(1).ecx & (1 << 19)) != 0
                };
                SSE41_STATE.store(
                    if supported {
                        SSE41_SUPPORTED
                    } else {
                        SSE41_UNSUPPORTED
                    },
                    Ordering::Relaxed,
                );
                supported
            }
        };

        if !cpu_supported {
            return false;
        }

        // CPUID reports hardware capability, not whether the kernel has
        // enabled SSE execution. Avoid #UD/#NM when the OS has not enabled
        // XMM state or is using lazy FPU switching. The scheduler must still
        // preserve SIMD/FPU state across task and interrupt context switches.
        let (cr0, cr4): (u64, u64);
        unsafe {
            core::arch::asm!(
                "mov {}, cr0",
                out(reg) cr0,
                options(nomem, nostack, preserves_flags),
            );
            core::arch::asm!(
                "mov {}, cr4",
                out(reg) cr4,
                options(nomem, nostack, preserves_flags),
            );
        }

        const CR0_EM: u64 = 1 << 2;
        const CR0_TS: u64 = 1 << 3;
        const CR4_OSFXSR: u64 = 1 << 9;
        const CR4_OSXMMEXCPT: u64 = 1 << 10;

        (cr0 & (CR0_EM | CR0_TS)) == 0
            && (cr4 & (CR4_OSFXSR | CR4_OSXMMEXCPT))
                == (CR4_OSFXSR | CR4_OSXMMEXCPT)
    }

    #[cfg(not(target_arch = "x86_64"))]
    {
        false
    }
}

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

    let mut atlas = Atlas {
        width,
        height,
        glyphs: &raw[glyphs_off..glyphs_end],
        glyph_count,
        pixels: &raw[glyphs_end..pixels_end],
        ascii: [None; 128],
    };
    // Validate common UI glyphs once, rather than scanning the unsorted
    // atlas and re-validating coordinates for each character on every frame.
    for codepoint in 0..atlas.ascii.len() {
        atlas.ascii[codepoint] = find_glyph(&atlas, codepoint as u32);
    }
    Some(atlas)
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

#[derive(Clone, Copy)]
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
fn find_glyph(atlas: &Atlas, codepoint: u32) -> Option<Glyph> {
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
fn glyph_for(atlas: &Atlas, codepoint: u32) -> Option<Glyph> {
    match atlas.ascii.get(codepoint as usize) {
        Some(glyph) => *glyph,
        None => find_glyph(atlas, codepoint),
    }
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

// Do the inner coverage loop in fixed point. The kernel Rust target uses
// soft-float, so bilinear interpolation of three f32 channels per pixel
// otherwise calls software arithmetic even at ordinary UI text sizes.
const SAMPLE_ONE: i64 = 1 << 20;
const WEIGHT_ONE: i64 = 1 << 16;

struct GlyphSampler<'a> {
    atlas: &'a Atlas,
    min_x: u32,
    min_y: u32,
    max_x: u32,
    max_y: u32,
    range: i64, // Q12 screen-pixel range
}

#[inline]
fn sample_axis(pos: i64, lo: u32, hi: u32) -> (usize, usize, i64) {
    let pos = pos.clamp(i64::from(lo) * SAMPLE_ONE, i64::from(hi) * SAMPLE_ONE);
    let p = (pos / SAMPLE_ONE) as usize;
    let weight = (pos % SAMPLE_ONE) >> 4;
    (p, (p + 1).min(hi as usize), weight)
}

impl GlyphSampler<'_> {
    #[inline]
    fn coverage(&self, x: i64, y: (usize, usize, i64)) -> u8 {
        let (x0, x1, tx) = sample_axis(x, self.min_x, self.max_x);
        let (y0, y1, ty) = y;
        let pitch = self.atlas.width as usize * 3;
        let i00 = y0 * pitch + x0 * 3;
        let i01 = y1 * pitch + x0 * 3;
        let i10 = y0 * pitch + x1 * 3;
        let i11 = y1 * pitch + x1 * 3;
        let p = self.atlas.pixels;
        let channel = |c: usize| {
            let top = i64::from(p[i00 + c]) * (WEIGHT_ONE - tx)
                + i64::from(p[i10 + c]) * tx;
            let bot = i64::from(p[i01 + c]) * (WEIGHT_ONE - tx)
                + i64::from(p[i11 + c]) * tx;
            (top * (WEIGHT_ONE - ty) + bot * ty) >> 16
        };
        let r = channel(0);
        let g = channel(1);
        let b = channel(2);
        let median = r.min(g).max(r.max(g).min(b));
        // median is Q16 in byte units. Convert the signed distance around
        // 127.5 to coverage, bias by 127.5 and round to the nearest byte.
        let delta = median - (255 * WEIGHT_ONE / 2);
        (((delta * self.range) + (128 << 28)) >> 28).clamp(0, 255) as u8
    }

    // Four consecutive output pixels share the same pair of source rows.
    // The SSE4.1 path gathers their texels and performs all RGB interpolation
    // and the median operation in four 32-bit lanes. The fallback preserves
    // the original scalar calculation exactly.
    #[inline]
    fn coverage4(
        &self,
        first_x: i64,
        dx: i64,
        y: (usize, usize, i64),
        use_sse41: bool,
        output: &mut [u8; 4],
    ) {
        #[cfg(target_arch = "x86_64")]
        if use_sse41 {
            unsafe {
                coverage4_sse41(self, first_x, dx, y, output);
            }
            return;
        }

        #[cfg(not(target_arch = "x86_64"))]
        let _ = use_sse41;

        let mut x = first_x;
        for alpha in output.iter_mut() {
            *alpha = self.coverage(x, y);
            x += dx;
        }
    }
}

// Exact Q16 vertical interpolation using only 32-bit SSE4.1 operations.
//
// `top` and `bottom` are Q16 values in [0, 255 * 65536]. A direct
// multiplication by the Q16 weights needs 48 bits. Split each value into
// integer and fractional halves:
//
//   v = (v_hi * 65536) + v_lo
//
// Then:
//
//   floor((top * inv + bottom * t) / 65536)
//     = top_hi * inv + bottom_hi * t
//       + floor((top_lo * inv + bottom_lo * t) / 65536)
//
// Since `inv + t == 65536` and each low half is <= 65535, the fractional
// numerator is <= 65535 * 65536 and therefore fits in an unsigned 32-bit
// lane. PMULLD keeps its low 32 bits, which are the exact value here.
// This avoids 64-bit SIMD multiplication and the associated lane shuffles.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn vertical_lerp_sse41(
    top: __m128i,
    bottom: __m128i,
    inv_ty: __m128i,
    ty: __m128i,
) -> __m128i {
    unsafe {
        let mask = _mm_set1_epi32(0xFFFF);

        let top_hi = _mm_srli_epi32::<16>(top);
        let bottom_hi = _mm_srli_epi32::<16>(bottom);
        let top_lo = _mm_and_si128(top, mask);
        let bottom_lo = _mm_and_si128(bottom, mask);

        let integer = _mm_add_epi32(
            _mm_mullo_epi32(top_hi, inv_ty),
            _mm_mullo_epi32(bottom_hi, ty),
        );
        let fractional = _mm_add_epi32(
            _mm_mullo_epi32(top_lo, inv_ty),
            _mm_mullo_epi32(bottom_lo, ty),
        );

        _mm_add_epi32(integer, _mm_srli_epi32::<16>(fractional))
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn bilinear_channel_sse41(
    p00: __m128i,
    p10: __m128i,
    p01: __m128i,
    p11: __m128i,
    tx: __m128i,
    inv_tx: __m128i,
    ty: __m128i,
    inv_ty: __m128i,
) -> __m128i {
    unsafe {
        // Horizontal weighted sums are bounded by 255 * 65536 and therefore
        // fit exactly in signed 32-bit lanes.
        let top = _mm_add_epi32(
            _mm_mullo_epi32(p00, inv_tx),
            _mm_mullo_epi32(p10, tx),
        );
        let bottom = _mm_add_epi32(
            _mm_mullo_epi32(p01, inv_tx),
            _mm_mullo_epi32(p11, tx),
        );
        vertical_lerp_sse41(top, bottom, inv_ty, ty)
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.1")]
unsafe fn coverage4_sse41(
    sampler: &GlyphSampler<'_>,
    first_x: i64,
    dx: i64,
    y: (usize, usize, i64),
    output: &mut [u8; 4],
) {
    let (y0, y1, ty_fixed) = y;
    let ty_scalar = ty_fixed as i32;
    let inv_ty_scalar = (WEIGHT_ONE - ty_fixed) as i32;

    let mut tx_lanes = [0i32; 4];
    let mut inv_tx_lanes = [0i32; 4];

    let mut r00 = [0i32; 4];
    let mut g00 = [0i32; 4];
    let mut b00 = [0i32; 4];
    let mut r10 = [0i32; 4];
    let mut g10 = [0i32; 4];
    let mut b10 = [0i32; 4];
    let mut r01 = [0i32; 4];
    let mut g01 = [0i32; 4];
    let mut b01 = [0i32; 4];
    let mut r11 = [0i32; 4];
    let mut g11 = [0i32; 4];
    let mut b11 = [0i32; 4];

    let pitch = sampler.atlas.width as usize * 3;
    let pixels = sampler.atlas.pixels;
    let row0 = y0 * pitch;
    let row1 = y1 * pitch;
    let mut sample_x = first_x;

    // SSE4.1 has no general gather instruction, so texel addresses are
    // gathered scalarly while all interpolation arithmetic is vectorized.
    for lane in 0..4 {
        let (x0, x1, tx_fixed) = sample_axis(
            sample_x,
            sampler.min_x,
            sampler.max_x,
        );
        sample_x += dx;

        let tx = tx_fixed as i32;
        tx_lanes[lane] = tx;
        inv_tx_lanes[lane] = (WEIGHT_ONE as i32) - tx;

        let i00 = row0 + x0 * 3;
        let i10 = row0 + x1 * 3;
        let i01 = row1 + x0 * 3;
        let i11 = row1 + x1 * 3;

        r00[lane] = pixels[i00] as i32;
        g00[lane] = pixels[i00 + 1] as i32;
        b00[lane] = pixels[i00 + 2] as i32;
        r10[lane] = pixels[i10] as i32;
        g10[lane] = pixels[i10 + 1] as i32;
        b10[lane] = pixels[i10 + 2] as i32;
        r01[lane] = pixels[i01] as i32;
        g01[lane] = pixels[i01 + 1] as i32;
        b01[lane] = pixels[i01 + 2] as i32;
        r11[lane] = pixels[i11] as i32;
        g11[lane] = pixels[i11 + 1] as i32;
        b11[lane] = pixels[i11 + 2] as i32;
    }

    let mut median_lanes = [0i32; 4];

    unsafe {
        let tx = _mm_loadu_si128(tx_lanes.as_ptr() as *const __m128i);
        let inv_tx = _mm_loadu_si128(inv_tx_lanes.as_ptr() as *const __m128i);
        let ty = _mm_set1_epi32(ty_scalar);
        let inv_ty = _mm_set1_epi32(inv_ty_scalar);

        let r = bilinear_channel_sse41(
            _mm_loadu_si128(r00.as_ptr() as *const __m128i),
            _mm_loadu_si128(r10.as_ptr() as *const __m128i),
            _mm_loadu_si128(r01.as_ptr() as *const __m128i),
            _mm_loadu_si128(r11.as_ptr() as *const __m128i),
            tx,
            inv_tx,
            ty,
            inv_ty,
        );
        let g = bilinear_channel_sse41(
            _mm_loadu_si128(g00.as_ptr() as *const __m128i),
            _mm_loadu_si128(g10.as_ptr() as *const __m128i),
            _mm_loadu_si128(g01.as_ptr() as *const __m128i),
            _mm_loadu_si128(g11.as_ptr() as *const __m128i),
            tx,
            inv_tx,
            ty,
            inv_ty,
        );
        let b = bilinear_channel_sse41(
            _mm_loadu_si128(b00.as_ptr() as *const __m128i),
            _mm_loadu_si128(b10.as_ptr() as *const __m128i),
            _mm_loadu_si128(b01.as_ptr() as *const __m128i),
            _mm_loadu_si128(b11.as_ptr() as *const __m128i),
            tx,
            inv_tx,
            ty,
            inv_ty,
        );

        // Equivalent to min(r,g).max(max(r,g).min(b)), lane by lane.
        let min_rg = _mm_min_epu32(r, g);
        let max_rg = _mm_max_epu32(r, g);
        let median = _mm_max_epu32(min_rg, _mm_min_epu32(max_rg, b));
        _mm_storeu_si128(
            median_lanes.as_mut_ptr() as *mut __m128i,
            median,
        );
    }

    // Keep the final signed-distance conversion bit-identical to the scalar
    // path. The three-channel interpolation and median are already computed
    // in SIMD; only the final four coverage conversions remain scalar.
    let half = 255 * WEIGHT_ONE / 2;
    for lane in 0..4 {
        let delta = i64::from(median_lanes[lane]) - half;
        output[lane] = (((delta * sampler.range) + (128 << 28)) >> 28)
            .clamp(0, 255) as u8;
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
    use_sse41: bool,
) {
    if !glyph.drawable {
        return;
    }

    let x0 = pen_x + glyph.plane_l * px_size;
    let y0 = baseline_y - glyph.plane_t * px_size;
    let x1 = pen_x + glyph.plane_r * px_size;
    let y1 = baseline_y - glyph.plane_b * px_size;

    let screen_w = x1 - x0;
    let screen_h = y1 - y0;

    if !x0.is_finite()
        || !y0.is_finite()
        || !x1.is_finite()
        || !y1.is_finite()
        || !screen_w.is_finite()
        || !screen_h.is_finite()
        || screen_w <= 0.0
        || screen_h <= 0.0
    {
        return;
    }

    // Rasterize only pixels whose centers are inside the glyph quad.
    // Do not snap each glyph origin independently.
    let left_f = ceil_f(x0 - 0.5);
    let top_f = ceil_f(y0 - 0.5);
    let right_f = ceil_f(x1 - 0.5);
    let bottom_f = ceil_f(y1 - 0.5);

    let left64 = left_f as i64;
    let top64 = top_f as i64;
    let right64 = right_f as i64;
    let bottom64 = bottom_f as i64;

    if left64 < i32::MIN as i64
        || top64 < i32::MIN as i64
        || right64 > i32::MAX as i64
        || bottom64 > i32::MAX as i64
    {
        return;
    }

    let quad_w64 = right64 - left64;
    let quad_h64 = bottom64 - top64;

    if quad_w64 <= 0
        || quad_h64 <= 0
        || quad_w64 > 4096
        || quad_h64 > 4096
    {
        return;
    }

    let left = left64 as i32;
    let top = top64 as i32;
    let quad_w = quad_w64 as i32;
    let quad_h = quad_h64 as i32;

    let (mut first_x, mut first_y, mut end_x, mut end_y) = (0, 0, quad_w, quad_h);
    if let TextTarget::Buffer { width, height, .. } = target {
        first_x = (-left64).clamp(0, quad_w64) as i32;
        first_y = (-top64).clamp(0, quad_h64) as i32;
        end_x = (i64::from(width) - left64).clamp(0, quad_w64) as i32;
        end_y = (i64::from(height) - top64).clamp(0, quad_h64) as i32;
        if first_x >= end_x || first_y >= end_y { return; }
    }

    // Original floating-point atlas dimensions.
    let atlas_glyph_w = glyph.atlas_r - glyph.atlas_l;
    let atlas_glyph_h = glyph.atlas_t - glyph.atlas_b;

    if atlas_glyph_w <= 0.0 || atlas_glyph_h <= 0.0 {
        return;
    }

    // Convert bottom-left atlas coordinates to top-left image coordinates.
    let atlas_top = atlas.height as f32 - glyph.atlas_t;
    let atlas_bottom = atlas.height as f32 - glyph.atlas_b;

    // Include the bilinear sampling footprint around the glyph bounds.
    // Integer indices represent texel centers in sample_bilinear().
    let gx0 = floor_f((glyph.atlas_l - 0.5).max(0.0)) as u32;
    let gx1 = (ceil_f(glyph.atlas_r - 0.5) + 1.0)
        .min(atlas.width as f32) as u32;

    let gy0 = floor_f((atlas_top - 0.5).max(0.0)) as u32;
    let gy1 = (ceil_f(atlas_bottom - 0.5) + 1.0)
        .min(atlas.height as f32) as u32;

    if gx1 <= gx0 || gy1 <= gy0 {
        return;
    }

    // Preserve the exact screen-to-atlas scale on both axes.
    let dfx = atlas_glyph_w / screen_w;
    let dfy = atlas_glyph_h / screen_h;

    // Pixel centers in screen coordinates.
    let first_screen_x = left_f + 0.5;
    let first_screen_y = top_f + 0.5;

    // Atlas coordinates are converted to texel-index coordinates
    // by subtracting the half-texel offset.
    let fx_start = glyph.atlas_l
        + (first_screen_x - x0) * dfx
        - 0.5;

    let fy_start = atlas_top
        + (first_screen_y - y0) * dfy
        - 0.5;

    // MSDF range converted into screen pixels.
    let screen_px_range = (ATLAS_PX_RANGE
        * (screen_w / atlas_glyph_w)
            .max(screen_h / atlas_glyph_h))
        .max(1.0);

    let sampler = GlyphSampler {
        atlas,
        min_x: gx0,
        min_y: gy0,
        max_x: gx1 - 1,
        max_y: gy1 - 1,
        // Clamping protects integer arithmetic for degenerate atlas input.
        range: ((screen_px_range * 4096.0) as i64).min(i64::from(i32::MAX)),
    };
    let fixed = |value: f32| {
        // All real atlas coordinates and nonempty steps are much smaller
        // than this; bounding also protects the 4096-pixel loop increments.
        ((value * SAMPLE_ONE as f32) as i64).clamp(-(1 << 50), 1 << 50)
    };
    let fx0 = fixed(fx_start);
    let fy0 = fixed(fy_start);
    let dx = fixed(dfx);
    let dy = fixed(dfy);
    let mut alpha_buf = [0u8; 4];

    for py in first_y..end_y {
        let y = sample_axis(fy0 + dy * i64::from(py), sampler.min_y, sampler.max_y);
        let mut fx = fx0 + dx * i64::from(first_x);
        let mut px = first_x;
        while px + 4 <= end_x {
            sampler.coverage4(fx, dx, y, use_sse41, &mut alpha_buf);
            if alpha_buf != [0; 4] {
                unsafe { blend_four(target, left + px, top + py, color, &alpha_buf); }
            }
            fx += dx * 4;
            px += 4;
        }
        while px < end_x {
            let alpha = sampler.coverage(fx, y);
            if alpha != 0 {
                unsafe { blend_one(target, left + px, top + py, color, alpha); }
            }
            fx += dx;
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
    let use_sse41 = sse41_available();

    for ch in text.chars() {
        if ch == '\n' {
            pen_x = x as f32;
            baseline_y += line_height;
            continue;
        }

        if let Some(glyph) = glyph_for_char(atlas, ch) {
            draw_glyph(
                target,
                atlas,
                &glyph,
                pen_x,
                baseline_y,
                color,
                px_size_f,
                use_sse41,
            );
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

// ===================================================================
// Test hooks (GPU_ACCEL_PLAN H2 guard)
// ===================================================================
//
// 4628af0 shipped a vectorised path whose comment asserts it is "bit-
// identical to the scalar path", with no test and no measurement.  These
// three functions exist so tests/unit/test_sdf_simd.rs can compare the
// two paths on the REAL file instead of a copy of it — the project's
// rule (GL2/OPT: host tests link the shipping source).
//
// They compile to nothing in the kernel: `cfg(test_sdf_host)` is set only
// by the test harness's rustc invocation, so `make iso` sees no new
// symbol, no new branch and no size change.  The names say what they are.

#[cfg(test_sdf_host)]
pub const WEIGHT_ONE_TEST: i64 = WEIGHT_ONE;

/// The scalar vertical lerp the SIMD path is required to reproduce.
#[cfg(test_sdf_host)]
pub fn test_vertical_lerp(top: u64, bottom: u64, inv: u64, ty: u64) -> u64 {
    ((top as i128 * inv as i128 + bottom as i128 * ty as i128) >> 16) as u64
}

#[cfg(test_sdf_host)]
pub fn test_coverage4_scalar<'a>(
    pixels: &'a [u8], w: u32, h: u32,
    first_x: i64, dx: i64, ty: i64, range: i64,
    out: &mut [u8; 4],
) {
    // Build the smallest Atlas the sampler needs.
    // Safety: every hook call is synchronous and the borrowed slice
    // outlives it; the 'static bound is Atlas's storage detail, not a
    // promise the hook makes.
    let pixels: &'static [u8] = unsafe { core::mem::transmute(pixels) };
    let atlas = Atlas {
        width: w,
        height: h,
        glyphs: &[],
        glyph_count: 0,
        pixels,
        ascii: [None; 128],
    };
    let s = GlyphSampler {
        atlas: &atlas,
        min_x: 0,
        min_y: 0,
        max_x: w - 1,
        max_y: h - 1,
        range,
    };
    let y = (0usize, 1usize, ty);
    let mut x = first_x;
    for slot in out.iter_mut() {
        *slot = s.coverage(x, y);
        x += dx;
    }
}

#[cfg(test_sdf_host)]
pub fn test_coverage4_simd<'a>(
    pixels: &'a [u8], w: u32, h: u32,
    first_x: i64, dx: i64, ty: i64, range: i64,
    out: &mut [u8; 4],
) {
    let pixels: &'static [u8] = unsafe { core::mem::transmute(pixels) };
    let atlas = Atlas {
        width: w,
        height: h,
        glyphs: &[],
        glyph_count: 0,
        pixels,
        ascii: [None; 128],
    };
    let s = GlyphSampler {
        atlas: &atlas,
        min_x: 0,
        min_y: 0,
        max_x: w - 1,
        max_y: h - 1,
        range,
    };
    let y = (0usize, 1usize, ty);
    unsafe { s.coverage4(first_x, dx, y, true, out) };
}
