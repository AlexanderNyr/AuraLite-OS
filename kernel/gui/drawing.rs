//! Low-level framebuffer operations for GUI window back buffers.
//!
//! This module deliberately knows nothing about window IDs, window ownership,
//! dirty state, or compositor scheduling. C validates the window and passes
//! its buffer, logical allocation dimensions, and stride here.
//!
//! Pixel format: 0x00RRGGBB. For alpha blits the source alpha is in bits 24..31.

use core::ptr;

#[inline]
fn valid_buffer(buffer: *const u32, width: u32, height: u32, stride: u32) -> bool {
    !buffer.is_null() && width != 0 && height != 0 && stride >= width
}

#[inline]
fn blend_value(dst: u32, src: u32, alpha: u8) -> u32 {
    if alpha == 0 {
        return dst;
    }
    if alpha == 255 {
        return src;
    }

    let a = alpha as u32;
    let ia = 255 - a;

    let sr = (src >> 16) & 0xff;
    let sg = (src >> 8) & 0xff;
    let sb = src & 0xff;
    let dr = (dst >> 16) & 0xff;
    let dg = (dst >> 8) & 0xff;
    let db = dst & 0xff;

    let r = (sr * a + dr * ia) / 255;
    let g = (sg * a + dg * ia) / 255;
    let b = (sb * a + db * ia) / 255;

    (r << 16) | (g << 8) | b
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn fill32_sse2(dst: *mut u32, value: u32, mut count: usize) {
    use core::arch::x86_64::{_mm_set1_epi32, _mm_storeu_si128};

    let vector = _mm_set1_epi32(value as i32);
    let mut out = dst;

    while count >= 4 {
        _mm_storeu_si128(out.cast(), vector);
        out = out.add(4);
        count -= 4;
    }

    while count != 0 {
        out.write(value);
        out = out.add(1);
        count -= 1;
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn copy32_sse2(dst: *mut u32, src: *const u32, mut count: usize) {
    use core::arch::x86_64::{_mm_loadu_si128, _mm_storeu_si128};

    let mut out = dst;
    let mut input = src;

    while count >= 4 {
        let vector = _mm_loadu_si128(input.cast());
        _mm_storeu_si128(out.cast(), vector);
        out = out.add(4);
        input = input.add(4);
        count -= 4;
    }

    while count != 0 {
        out.write(input.read());
        out = out.add(1);
        input = input.add(1);
        count -= 1;
    }
}

/*
 * Four-pixel SSE2 alpha blend. This preserves the old C implementation's
 * exact edge semantics: alpha 0 leaves the destination word untouched,
 * alpha 255 copies the source word verbatim, and intermediate alpha emits
 * RGB with the upper byte cleared.
 */
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn blend4_sse2(dst: *mut u32, src: *const u32, alpha: [u8; 4]) {
    use core::arch::x86_64::{
        _mm_and_si128, _mm_cmpeq_epi32, _mm_loadu_si128, _mm_mullo_epi16,
        _mm_or_si128, _mm_packus_epi16, _mm_set1_epi16, _mm_set1_epi32,
        _mm_set_epi16, _mm_set_epi32, _mm_setzero_si128, _mm_srli_epi16, _mm_storeu_si128,
        _mm_sub_epi16, _mm_unpackhi_epi8, _mm_unpacklo_epi8,
        _mm_add_epi16,
    };

    let source = _mm_loadu_si128(src.cast());
    let destination = _mm_loadu_si128(dst.cast());
    let zero = _mm_setzero_si128();

    let source_lo = _mm_unpacklo_epi8(source, zero);
    let source_hi = _mm_unpackhi_epi8(source, zero);
    let dest_lo = _mm_unpacklo_epi8(destination, zero);
    let dest_hi = _mm_unpackhi_epi8(destination, zero);

    let alpha_lo = _mm_set_epi16(
        alpha[1] as i16, alpha[1] as i16, alpha[1] as i16, alpha[1] as i16,
        alpha[0] as i16, alpha[0] as i16, alpha[0] as i16, alpha[0] as i16,
    );
    let alpha_hi = _mm_set_epi16(
        alpha[3] as i16, alpha[3] as i16, alpha[3] as i16, alpha[3] as i16,
        alpha[2] as i16, alpha[2] as i16, alpha[2] as i16, alpha[2] as i16,
    );

    let inv255 = _mm_set1_epi16(255);
    let inverse_lo = _mm_sub_epi16(inv255, alpha_lo);
    let inverse_hi = _mm_sub_epi16(inv255, alpha_hi);

    let mut mixed_lo = _mm_add_epi16(
        _mm_mullo_epi16(source_lo, alpha_lo),
        _mm_mullo_epi16(dest_lo, inverse_lo),
    );
    let mut mixed_hi = _mm_add_epi16(
        _mm_mullo_epi16(source_hi, alpha_hi),
        _mm_mullo_epi16(dest_hi, inverse_hi),
    );

    /* Exact floor(x / 255) for x in [0, 65025]. */
    let one = _mm_set1_epi16(1);
    mixed_lo = _mm_srli_epi16(
        _mm_add_epi16(mixed_lo, _mm_add_epi16(_mm_srli_epi16(mixed_lo, 8), one)),
        8,
    );
    mixed_hi = _mm_srli_epi16(
        _mm_add_epi16(mixed_hi, _mm_add_epi16(_mm_srli_epi16(mixed_hi, 8), one)),
        8,
    );

    let mut result = _mm_packus_epi16(mixed_lo, mixed_hi);
    result = _mm_and_si128(result, _mm_set1_epi32(0x00ff_ffff));

    let alpha_words = _mm_set_epi32(
        alpha[3] as i32,
        alpha[2] as i32,
        alpha[1] as i32,
        alpha[0] as i32,
    );
    let zero_mask = _mm_cmpeq_epi32(alpha_words, zero);
    let full_mask = _mm_cmpeq_epi32(alpha_words, _mm_set1_epi32(255));

    result = _mm_or_si128(result, _mm_and_si128(destination, zero_mask));
    result = _mm_or_si128(result, _mm_and_si128(source, full_mask));
    _mm_storeu_si128(dst.cast(), result);
}

/// Fill a contiguous pixel span. Used by C for window allocation and frame
/// buffer copies as well as by the higher-level drawing operations.
#[no_mangle]
pub unsafe extern "C" fn gui_rust_fill32(dst: *mut u32, value: u32, count: usize) {
    if dst.is_null() || count == 0 {
        return;
    }

    #[cfg(target_arch = "x86_64")]
    if count >= 4 {
        fill32_sse2(dst, value, count);
        return;
    }

    for i in 0..count {
        dst.add(i).write(value);
    }
}

/// Copy a contiguous pixel span. Source and destination must not overlap.
#[no_mangle]
pub unsafe extern "C" fn gui_rust_copy32(dst: *mut u32, src: *const u32, count: usize) {
    if dst.is_null() || src.is_null() || count == 0 || dst.cast_const() == src {
        return;
    }

    #[cfg(target_arch = "x86_64")]
    if count >= 4 {
        copy32_sse2(dst, src, count);
        return;
    }

    ptr::copy_nonoverlapping(src, dst, count);
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_draw_pixel(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    color: u32,
) -> i32 {
    if !valid_buffer(buffer, width, height, stride) {
        return -1;
    }
    if x < 0 || y < 0 || x as u32 >= width || y as u32 >= height {
        return 0;
    }

    let offset = y as usize * stride as usize + x as usize;
    buffer.add(offset).write(color);
    0
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_get_pixel(
    buffer: *const u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    out: *mut u32,
) -> i32 {
    if out.is_null() || !valid_buffer(buffer, width, height, stride) {
        return -1;
    }
    if x < 0 || y < 0 || x as u32 >= width || y as u32 >= height {
        return -1;
    }

    let offset = y as usize * stride as usize + x as usize;
    out.write(buffer.add(offset).read());
    0
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_blend_pixel(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    color: u32,
    alpha: u8,
) -> i32 {
    if !valid_buffer(buffer, width, height, stride) {
        return -1;
    }
    if x < 0 || y < 0 || x as u32 >= width || y as u32 >= height || alpha == 0 {
        return 0;
    }

    let offset = y as usize * stride as usize + x as usize;
    let pixel = buffer.add(offset);
    pixel.write(blend_value(pixel.read(), color, alpha));
    0
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_blend_pixels4(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    color: u32,
    alpha: *const u8,
) -> i32 {
    if alpha.is_null() || !valid_buffer(buffer, width, height, stride) {
        return -1;
    }
    if y < 0 || y as u32 >= height {
        return 0;
    }

    let values = [alpha.read(), alpha.add(1).read(), alpha.add(2).read(), alpha.add(3).read()];
    if values.iter().all(|a| *a == 0) {
        return 0;
    }

    if x >= 0 && (x as u64) + 4 <= width as u64 {
        let offset = y as usize * stride as usize + x as usize;
        let destination = buffer.add(offset);
        let source = [color; 4];

        #[cfg(target_arch = "x86_64")]
        {
            blend4_sse2(destination, source.as_ptr(), values);
            return 0;
        }

        #[cfg(not(target_arch = "x86_64"))]
        {
            for i in 0..4 {
                if values[i] != 0 {
                    let pixel = destination.add(i);
                    pixel.write(blend_value(pixel.read(), color, values[i]));
                }
            }
            return 0;
        }
    }

    for i in 0..4usize {
        if values[i] == 0 {
            continue;
        }
        let px = x as i64 + i as i64;
        if px < 0 || px >= width as i64 {
            continue;
        }
        let offset = y as usize * stride as usize + px as usize;
        let pixel = buffer.add(offset);
        pixel.write(blend_value(pixel.read(), color, values[i]));
    }
    0
}

#[inline]
fn clipped_rect(
    x: i32,
    y: i32,
    w: u32,
    h: u32,
    buffer_w: u32,
    buffer_h: u32,
) -> Option<(u32, u32, u32, u32)> {
    if w == 0 || h == 0 || buffer_w == 0 || buffer_h == 0 {
        return None;
    }

    let left = (x as i64).max(0);
    let top = (y as i64).max(0);
    let right = (x as i64 + w as i64).min(buffer_w as i64);
    let bottom = (y as i64 + h as i64).min(buffer_h as i64);

    if right <= left || bottom <= top {
        return None;
    }

    Some((left as u32, top as u32, (right - left) as u32, (bottom - top) as u32))
}

unsafe fn fill_rect_i64(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i64,
    y: i64,
    w: u32,
    h: u32,
    color: u32,
) {
    if buffer.is_null() || width == 0 || height == 0 || stride < width || w == 0 || h == 0 {
        return;
    }

    let left = x.max(0);
    let top = y.max(0);
    let right = (x + w as i64).min(width as i64);
    let bottom = (y + h as i64).min(height as i64);
    if right <= left || bottom <= top {
        return;
    }

    let row_width = (right - left) as usize;
    let mut row = top as usize;
    while row < bottom as usize {
        let offset = row * stride as usize + left as usize;
        gui_rust_fill32(buffer.add(offset), color, row_width);
        row += 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_fill_rect(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
    color: u32,
) -> i32 {
    if !valid_buffer(buffer, width, height, stride) {
        return -1;
    }

    if let Some((left, top, cw, ch)) = clipped_rect(x, y, w, h, width, height) {
        let mut row = top as usize;
        while row < top as usize + ch as usize {
            let offset = row * stride as usize + left as usize;
            gui_rust_fill32(buffer.add(offset), color, cw as usize);
            row += 1;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_draw_rect(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
    color: u32,
) -> i32 {
    if !valid_buffer(buffer, width, height, stride) {
        return -1;
    }
    if w == 0 || h == 0 {
        return 0;
    }

    fill_rect_i64(buffer, width, height, stride, x as i64, y as i64, w, 1, color);
    if h > 1 {
        fill_rect_i64(
            buffer, width, height, stride,
            x as i64, y as i64 + h as i64 - 1, w, 1, color,
        );
    }
    fill_rect_i64(buffer, width, height, stride, x as i64, y as i64, 1, h, color);
    if w > 1 {
        fill_rect_i64(
            buffer, width, height, stride,
            x as i64 + w as i64 - 1, y as i64, 1, h, color,
        );
    }
    0
}

#[inline]
fn line_out_code(x: i64, y: i64, max_x: i64, max_y: i64) -> u8 {
    let mut code = 0;
    if x < 0 { code |= 1; }
    if x > max_x { code |= 2; }
    if y < 0 { code |= 4; }
    if y > max_y { code |= 8; }
    code
}

#[inline]
fn div_intersection(delta: i64, numerator: i64, denominator: i64) -> i64 {
    ((delta as i128 * numerator as i128) / denominator as i128) as i64
}

fn clip_line(
    mut x0: i64,
    mut y0: i64,
    mut x1: i64,
    mut y1: i64,
    width: u32,
    height: u32,
) -> Option<(i64, i64, i64, i64)> {
    if width == 0 || height == 0 {
        return None;
    }

    let max_x = width as i64 - 1;
    let max_y = height as i64 - 1;

    for _ in 0..16 {
        let c0 = line_out_code(x0, y0, max_x, max_y);
        let c1 = line_out_code(x1, y1, max_x, max_y);

        if (c0 | c1) == 0 {
            return Some((x0, y0, x1, y1));
        }
        if (c0 & c1) != 0 {
            return None;
        }

        let code = if c0 != 0 { c0 } else { c1 };
        let dx = x1 - x0;
        let dy = y1 - y0;
        let (x, y);

        if (code & 8) != 0 {
            if dy == 0 { return None; }
            y = max_y;
            x = x0 + div_intersection(dx, max_y - y0, dy);
        } else if (code & 4) != 0 {
            if dy == 0 { return None; }
            y = 0;
            x = x0 + div_intersection(dx, -y0, dy);
        } else if (code & 2) != 0 {
            if dx == 0 { return None; }
            x = max_x;
            y = y0 + div_intersection(dy, max_x - x0, dx);
        } else {
            if dx == 0 { return None; }
            x = 0;
            y = y0 + div_intersection(dy, -x0, dx);
        }

        if code == c0 {
            x0 = x;
            y0 = y;
        } else {
            x1 = x;
            y1 = y;
        }
    }
    None
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_draw_line(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
    color: u32,
) -> i32 {
    if !valid_buffer(buffer, width, height, stride) {
        return -1;
    }

    let Some((mut x0, mut y0, x1, y1)) = clip_line(
        x0 as i64, y0 as i64, x1 as i64, y1 as i64, width, height,
    ) else {
        return 0;
    };

    let dx = (x1 - x0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let dy = -(y1 - y0).abs();
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut error = dx + dy;

    loop {
        let offset = y0 as usize * stride as usize + x0 as usize;
        buffer.add(offset).write(color);

        if x0 == x1 && y0 == y1 {
            break;
        }

        let twice = error * 2;
        if twice >= dy {
            error += dy;
            x0 += sx;
        }
        if twice <= dx {
            error += dx;
            y0 += sy;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_clear(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    color: u32,
) -> i32 {
    if !valid_buffer(buffer, width, height, stride) {
        return -1;
    }

    for row in 0..height as usize {
        gui_rust_fill32(buffer.add(row * stride as usize), color, width as usize);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_blit(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    copy_width: u32,
    copy_height: u32,
    source: *const u32,
    source_stride: u32,
) -> i32 {
    if source.is_null() || !valid_buffer(buffer, width, height, stride) || source_stride == 0 {
        return -1;
    }
    if copy_width == 0 || copy_height == 0 {
        return 0;
    }

    let Some((left, top, clipped_w, clipped_h)) =
        clipped_rect(x, y, copy_width, copy_height, width, height)
    else {
        return 0;
    };

    let source_x = left as i64 - x as i64;
    let source_y = top as i64 - y as i64;

    for row in 0..clipped_h as usize {
        let dst_offset = (top as usize + row) * stride as usize + left as usize;
        let src_offset = (source_y as usize + row) * source_stride as usize + source_x as usize;
        gui_rust_copy32(buffer.add(dst_offset), source.add(src_offset), clipped_w as usize);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn gui_rust_blit_alpha(
    buffer: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    x: i32,
    y: i32,
    copy_width: u32,
    copy_height: u32,
    source: *const u32,
    source_stride: u32,
) -> i32 {
    if source.is_null() || !valid_buffer(buffer, width, height, stride) || source_stride == 0 {
        return -1;
    }
    if copy_width == 0 || copy_height == 0 {
        return 0;
    }

    let Some((left, top, clipped_w, clipped_h)) =
        clipped_rect(x, y, copy_width, copy_height, width, height)
    else {
        return 0;
    };

    let source_x = left as i64 - x as i64;
    let source_y = top as i64 - y as i64;

    for row in 0..clipped_h as usize {
        let dst_offset = (top as usize + row) * stride as usize + left as usize;
        let src_offset = (source_y as usize + row) * source_stride as usize + source_x as usize;
        let dst_row = buffer.add(dst_offset);
        let src_row = source.add(src_offset);
        let mut col = 0usize;

        #[cfg(target_arch = "x86_64")]
        {
            while col + 4 <= clipped_w as usize {
                let alpha = [
                    (src_row.add(col).read() >> 24) as u8,
                    (src_row.add(col + 1).read() >> 24) as u8,
                    (src_row.add(col + 2).read() >> 24) as u8,
                    (src_row.add(col + 3).read() >> 24) as u8,
                ];
                blend4_sse2(dst_row.add(col), src_row.add(col), alpha);
                col += 4;
            }
        }

        while col < clipped_w as usize {
            let src_pixel = src_row.add(col).read();
            let alpha = (src_pixel >> 24) as u8;
            if alpha != 0 {
                let dst_pixel = dst_row.add(col);
                dst_pixel.write(blend_value(dst_pixel.read(), src_pixel, alpha));
            }
            col += 1;
        }
    }
    0
}

