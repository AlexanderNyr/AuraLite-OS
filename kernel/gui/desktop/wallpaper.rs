//! Desktop wallpaper rendering.
//!
//! Procedurally generates a layered, flowing ribbon wallpaper inspired by the
//! attached Windows 11 dark-mode aesthetic, caching it in a static buffer so
//! full-screen and dirty-rect blits are fast and artifact-free.

use core::sync::atomic::{AtomicBool, Ordering};
use super::{screen_height, screen_width};

extern "C" {
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
}

// Максимальное поддерживаемое разрешение (Full HD). При необходимости можно увеличить.
const MAX_W: usize = 1920;
const MAX_H: usize = 1080;

static CACHE_LOCK: AtomicBool = AtomicBool::new(false);
static mut CACHED_W: u32 = 0;
static mut CACHED_H: u32 = 0;
static mut WALLPAPER_BUF: [u32; MAX_W * MAX_H] = [0; MAX_W * MAX_H];

#[inline]
fn lock_cache() {
    while CACHE_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

#[inline]
fn unlock_cache() {
    CACHE_LOCK.store(false, Ordering::Release);
}

/// Быстрый синус на целочисленной/фиксированной математике для `#![no_std]`.
/// `phase` в диапазоне [0..65536), результат в [-32768..32767].
#[inline]
fn fast_sin(phase: i32) -> i32 {
    let p = (phase & 0xFFFF) as i64;
    // Приводим [0..65536) к четвертям [0..16384)
    let quad = p >> 14;
    let mut x = p & 0x3FFF;
    if (quad & 1) != 0 {
        x = 16384 - x;
    }
    // Полиномиальная аппроксимация sin(pi/2 * x / 16384) * 32768
    let x2 = (x * x) >> 14;
    let res = (x * (51472 - ((x2 * 8433) >> 14))) >> 14;
    if quad >= 2 {
        -res as i32
    } else {
        res as i32
    }
}

#[inline]
fn lerp_channel(c0: u32, c1: u32, t: u32) -> u32 {
    let inv = 256 - t;
    (c0 * inv + c1 * t) >> 8
}

/// Плавная интерполяция двух цветов 0x00RRGGBB (`t` от 0 до 256).
#[inline]
fn lerp_color(a: u32, b: u32, t: u32) -> u32 {
    let t = t.min(256);
    let r = lerp_channel((a >> 16) & 0xFF, (b >> 16) & 0xFF, t);
    let g = lerp_channel((a >> 8) & 0xFF, (b >> 8) & 0xFF, t);
    let bl = lerp_channel(a & 0xFF, b & 0xFF, t);
    (r << 16) | (g << 8) | bl
}

/// Смешивание поверх фонового цвета (`alpha` от 0 до 256).
#[inline]
fn blend_over(dst: u32, src: u32, alpha: u32) -> u32 {
    if alpha == 0 {
        return dst;
    }
    if alpha >= 256 {
        return src;
    }
    lerp_color(dst, src, alpha)
}

unsafe fn regenerate_wallpaper(w: u32, h: u32) {
    let width = (w as usize).min(MAX_W);
    let height = (h as usize).min(MAX_H);
    if width == 0 || height == 0 {
        return;
    }

    // Цветовая палитра (0x00RRGGBB) по референсу:
    // Верхний фон: глубокий чёрный
    const BG_TOP: u32 = 0x00020306;
    // Нижний правый угол: глубокий тёмно-синий (индиго)
    const BG_BOT_RIGHT: u32 = 0x00051638;

    // Цвета градиента слева направо (Бирюзовый -> Голубой -> Фиолетовый)
    const WAVE_LEFT: u32 = 0x000CE8D8;   // яркий циан/бирюза
    const WAVE_MID: u32 = 0x002B8EF2;    // насыщенный голубой
    const WAVE_RIGHT: u32 = 0x006A67E8;  // мягкий индиго/фиолетовый

    for x in 0..width {
        // Нормализованная координата X [0..256]
        let u = ((x as u32) << 8) / (width as u32).max(1);

        // Базовый цвет волны в текущей колонке X (трехточечный градиент)
        let base_wave_color = if u < 128 {
            lerp_color(WAVE_LEFT, WAVE_MID, u * 2)
        } else {
            lerp_color(WAVE_MID, WAVE_RIGHT, (u - 128) * 2)
        };

        // Светлый блик на верхней кромке ленты (смесь с белым)
        let rim_color = lerp_color(base_wave_color, 0x00E6FFFF, 155);

        // Вычисляем Y-координаты 4 перекрывающихся волн для колонки X.
        // Фазы подобраны так, чтобы повторить S-образный изгиб с референса.
        let ph1 = ((u as i32 * 220) + 12000) as i32;
        let y_wave1 = (height as i32 * 30 / 100)
            + ((fast_sin(ph1) * (height as i32 * 13 / 100)) >> 15);

        let ph2 = ((u as i32 * 240) + 6000) as i32;
        let y_wave2 = (height as i32 * 40 / 100)
            + ((fast_sin(ph2) * (height as i32 * 16 / 100)) >> 15);

        let ph3 = ((u as i32 * 265) + 59000) as i32;
        let y_wave3 = (height as i32 * 53 / 100)
            + ((fast_sin(ph3) * (height as i32 * 19 / 100)) >> 15);

        let ph4 = ((u as i32 * 250) + 50000) as i32;
        let y_wave4 = (height as i32 * 68 / 100)
            + ((fast_sin(ph4) * (height as i32 * 18 / 100)) >> 15);

        for y in 0..height {
            let v = ((y as u32) << 8) / (height as u32).max(1);

            // 1. Базовый тёмный фон с лёгким синим свечением в правом нижнем углу
            let bg_factor = (u * v) >> 8;
            let mut pixel = lerp_color(BG_TOP, BG_BOT_RIGHT, bg_factor);

            let yi = y as i32;

            // 2. Дальняя верхняя волна (приглушённая, уходит в сине-фиолетовый)
            if yi >= y_wave1 {
                let depth = (yi - y_wave1) as u32;
                let max_d = (height as u32 / 3).max(1);
                let shade = 256u32.saturating_sub((depth << 8) / max_d);
                let c = lerp_color(0x0007122B, base_wave_color, (shade * 165) >> 8);
                let rim = if depth < 5 { 210 - depth * 35 } else { 0 };
                pixel = blend_over(c, rim_color, rim);
            }

            // 3. Вторая лента (глубже по тону, создаёт объём складки)
            if yi >= y_wave2 {
                let depth = (yi - y_wave2) as u32;
                let max_d = (height as u32 / 3).max(1);
                let shade = 256u32.saturating_sub((depth << 8) / max_d);
                let body = lerp_color(0x00082046, base_wave_color, (shade * 200) >> 8);
                // Мягкое затенение в самом начале под складкой
                let rim = if depth < 6 { 235 - depth * 35 } else { 0 };
                pixel = blend_over(body, rim_color, rim);
            }

            // 4. Третья центральная S-волна (самая контрастная)
            if yi >= y_wave3 {
                let depth = (yi - y_wave3) as u32;
                let max_d = (height as u32 * 45 / 100).max(1);
                let shade = 256u32.saturating_sub((depth << 8) / max_d);
                // Слева ярче, справа уходит в глубокую тень
                let side_fade = 256u32.saturating_sub((u * 130) >> 8);
                let intensity = ((shade * side_fade) >> 8).max(30);
                let body = lerp_color(0x00041430, base_wave_color, intensity);
                let rim = if depth < 7 { 250 - depth * 32 } else { 0 };
                pixel = blend_over(body, rim_color, rim);
            }

            // 5. Передняя нижняя бирюзовая волна
            if yi >= y_wave4 {
                let depth = (yi - y_wave4) as u32;
                let max_d = (height as u32 * 45 / 100).max(1);
                let shade = 256u32.saturating_sub((depth << 8) / max_d);
                let front_color = lerp_color(WAVE_LEFT, base_wave_color, u >> 1);
                let body = lerp_color(0x00063847, front_color, 90 + ((shade * 166) >> 8));
                let rim = if depth < 7 { 255 - depth * 34 } else { 0 };
                pixel = blend_over(body, rim_color, rim);
            }

            WALLPAPER_BUF[y * width + x] = pixel;
        }
    }
}

/// Draw the wallpaper into the active compositor clip.
#[no_mangle]
pub extern "C" fn desktop_wallpaper_render() {
    let width = screen_width();
    let height = screen_height();
    if width == 0 || height == 0 {
        return;
    }

    lock_cache();
    unsafe {
        if CACHED_W != width || CACHED_H != height {
            regenerate_wallpaper(width, height);
            CACHED_W = width;
            CACHED_H = height;
        }

        let blit_w = width.min(MAX_W as u32);
        let blit_h = height.min(MAX_H as u32);

        let _ = gfx_blit_rgb888(
            0,
            0,
            blit_w,
            blit_h,
            WALLPAPER_BUF.as_ptr(),
            blit_w,
            core::ptr::null(),
            0,
        );
    }
    unlock_cache();
}
