//! Rust screen compositor.
//!
//! Window ownership and window-surface lifetime remain in the C compatibility
//! layer. Rust owns frame orchestration and consumes a fixed-capacity SoA render
//! batch. The batch contains only slot IDs, generations and clip bounds: raw
//! window-surface pointers never cross the GUI lock boundary.

use core::sync::atomic::{AtomicBool, Ordering};

use crate::dirty::DirtyRect;
use crate::framebuffer::SystemFramebuffer;

const MAX_RENDER_ITEMS: usize = 256;
const MAX_DIRTY_RECTS: usize = 64;

/// Fixed-capacity structure-of-arrays command buffer shared with gui.c.
///
/// Every column has the same number of entries. The 64-byte-aligned base and
/// 1024-byte column sizes keep each column cache-line aligned. This is static
/// storage: composing a frame does not allocate memory.
#[repr(C, align(64))]
struct RenderBatch {
    wid: [i32; MAX_RENDER_ITEMS],
    epoch: [u32; MAX_RENDER_ITEMS],
    clip_x: [i32; MAX_RENDER_ITEMS],
    clip_y: [i32; MAX_RENDER_ITEMS],
    clip_w: [u32; MAX_RENDER_ITEMS],
    clip_h: [u32; MAX_RENDER_ITEMS],
}

const EMPTY_RENDER_BATCH: RenderBatch = RenderBatch {
    wid: [-1; MAX_RENDER_ITEMS],
    epoch: [0; MAX_RENDER_ITEMS],
    clip_x: [0; MAX_RENDER_ITEMS],
    clip_y: [0; MAX_RENDER_ITEMS],
    clip_w: [0; MAX_RENDER_ITEMS],
    clip_h: [0; MAX_RENDER_ITEMS],
};

// Compile-time ABI/layout guard: six 256-element u32-sized columns.
const _: [(); MAX_RENDER_ITEMS * 6 * core::mem::size_of::<u32>()] =
    [(); core::mem::size_of::<RenderBatch>()];

static mut RENDER_BATCH: RenderBatch = EMPTY_RENDER_BATCH;
static COMPOSITOR_ACTIVE: AtomicBool = AtomicBool::new(false);

struct ComposeGuard;

impl ComposeGuard {
    #[inline]
    fn acquire() -> Option<Self> {
        COMPOSITOR_ACTIVE
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .ok()
            .map(|_| Self)
    }
}

impl Drop for ComposeGuard {
    #[inline]
    fn drop(&mut self) {
        COMPOSITOR_ACTIVE.store(false, Ordering::Release);
    }
}

/// A transient AoS value reconstructed from the SoA batch for one draw.
/// Keeping this short-lived descriptor AoS is intentional: render.rs consumes
/// all of one window's properties together.
#[derive(Clone, Copy)]
struct RenderItem {
    wid: i32,
    epoch: u32,
    clip_x: i32,
    clip_y: i32,
    clip_w: u32,
    clip_h: u32,
}

extern "C" {
    /// Build an immutable, pointer-free SoA batch from the C window manager.
    fn gui_compositor_collect_items(
        clip_x: i32,
        clip_y: i32,
        clip_w: u32,
        clip_h: u32,
        out: *mut RenderBatch,
        capacity: usize,
    ) -> usize;

    /// Revalidate one slot generation and render it while its surface pointers
    /// are protected by gui_lock.
    fn gui_compositor_render_window(wid: i32, epoch: u32);

    fn gui_compositor_draw_snap_preview();
    fn gui_compositor_perf_full(composited_pixels: u64, flipped_pixels: u64);
    fn gui_compositor_perf_partial(composited_pixels: u64, flipped_pixels: u64);
}

#[inline]
fn render_batch_ptr() -> *mut RenderBatch {
    core::ptr::addr_of_mut!(RENDER_BATCH)
}

#[inline]
unsafe fn read_render_item(batch: *const RenderBatch, index: usize) -> RenderItem {
    // Read each SoA column independently. index is bounded by the count
    // returned from gui_compositor_collect_items().
    unsafe {
        RenderItem {
            wid: core::ptr::addr_of!((*batch).wid).cast::<i32>().add(index).read(),
            epoch: core::ptr::addr_of!((*batch).epoch).cast::<u32>().add(index).read(),
            clip_x: core::ptr::addr_of!((*batch).clip_x).cast::<i32>().add(index).read(),
            clip_y: core::ptr::addr_of!((*batch).clip_y).cast::<i32>().add(index).read(),
            clip_w: core::ptr::addr_of!((*batch).clip_w).cast::<u32>().add(index).read(),
            clip_h: core::ptr::addr_of!((*batch).clip_h).cast::<u32>().add(index).read(),
        }
    }
}

#[inline]
fn clip_to_screen(rect: DirtyRect, width: u32, height: u32) -> Option<DirtyRect> {
    if rect.w == 0 || rect.h == 0 || width == 0 || height == 0 {
        return None;
    }

    let left = (rect.x as i64).max(0);
    let top = (rect.y as i64).max(0);
    let right = (rect.x as i64 + rect.w as i64).min(width as i64);
    let bottom = (rect.y as i64 + rect.h as i64).min(height as i64);

    if right <= left || bottom <= top {
        return None;
    }

    Some(DirtyRect {
        x: left as i32,
        y: top as i32,
        w: (right - left) as u32,
        h: (bottom - top) as u32,
    })
}

/// Ask C for a z-sorted preorder batch, then render it in paint order.
/// The SoA contains no pointers, so window destruction/reuse between batch
/// construction and drawing is handled by the per-item epoch revalidation.
unsafe fn render_windows(fb: SystemFramebuffer, clip: DirtyRect) -> i32 {
    let batch = render_batch_ptr();
    let count = unsafe {
        gui_compositor_collect_items(
            clip.x,
            clip.y,
            clip.w,
            clip.h,
            batch,
            MAX_RENDER_ITEMS,
        )
    };

    if count > MAX_RENDER_ITEMS {
        return -1;
    }

    for index in 0..count {
        let item = unsafe { read_render_item(batch, index) };
        if item.wid < 0 || item.clip_w == 0 || item.clip_h == 0 {
            continue;
        }

        fb.set_clip(item.clip_x, item.clip_y, item.clip_w, item.clip_h);
        unsafe { gui_compositor_render_window(item.wid, item.epoch) };
    }

    fb.set_clip(clip.x, clip.y, clip.w, clip.h);
    0
}

#[inline]
fn render_desktop_base() {
    crate::desktop::wallpaper::desktop_wallpaper_render();
    crate::desktop::icons::shell_draw_icons();
}

#[inline]
fn render_desktop_chrome() {
    unsafe { gui_compositor_draw_snap_preview() };
    crate::desktop::taskbar::shell_draw_taskbar();
    crate::desktop::notifications::shell_draw_notifications();
}

#[inline]
fn render_cursor(have_cursor: bool, cursor_x: i32, cursor_y: i32) {
    if have_cursor {
        crate::desktop::cursor::gui_cursor_draw(cursor_x, cursor_y);
    }
}

#[inline]
fn commit_cursor(have_cursor: bool, cursor_x: i32, cursor_y: i32) {
    crate::desktop::cursor::gui_cursor_commit_presented(
        if have_cursor { 1 } else { 0 },
        cursor_x,
        cursor_y,
    );
}

/// Compose and present a complete screen frame.
#[no_mangle]
pub extern "C" fn gui_rust_compositor_render_full(
    have_cursor: i32,
    cursor_x: i32,
    cursor_y: i32,
) -> i32 {
    let Some(_guard) = ComposeGuard::acquire() else {
        return -16;
    };
    let Some(fb) = SystemFramebuffer::acquire() else {
        return 0;
    };

    let have_cursor = have_cursor != 0;
    let full_clip = DirtyRect {
        x: 0,
        y: 0,
        w: fb.width(),
        h: fb.height(),
    };

    fb.clear_clip();
    render_desktop_base();

    let window_result = unsafe { render_windows(fb, full_clip) };
    if window_result != 0 {
        fb.clear_clip();
        return window_result;
    }

    fb.clear_clip();
    if !crate::desktop::presentation_active() {
        render_desktop_chrome();
    }

    render_cursor(have_cursor, cursor_x, cursor_y);
    fb.present();
    commit_cursor(have_cursor, cursor_x, cursor_y);

    let composited_pixels = fb.take_pixels_written();
    let flipped_pixels = fb.width() as u64 * fb.height() as u64;
    unsafe { gui_compositor_perf_full(composited_pixels, flipped_pixels) };
    0
}

/// Recompose and present independent dirty screen rectangles.
#[no_mangle]
pub unsafe extern "C" fn gui_rust_compositor_render_dirty(
    rects: *const DirtyRect,
    count: usize,
    have_cursor: i32,
    cursor_x: i32,
    cursor_y: i32,
) -> i32 {
    if count > MAX_DIRTY_RECTS || (count != 0 && rects.is_null()) {
        return -1;
    }
    if count == 0 {
        return 0;
    }

    let Some(_guard) = ComposeGuard::acquire() else {
        return -16;
    };
    let Some(fb) = SystemFramebuffer::acquire() else {
        return 0;
    };

    let have_cursor = have_cursor != 0;
    let mut flipped_pixels = 0u64;

    for index in 0..count {
        let source_rect = unsafe { rects.add(index).read() };
        let Some(clip) = clip_to_screen(source_rect, fb.width(), fb.height()) else {
            continue;
        };

        fb.set_clip(clip.x, clip.y, clip.w, clip.h);
        render_desktop_base();

        if unsafe { render_windows(fb, clip) } != 0 {
            fb.clear_clip();
            return -1;
        }

        fb.set_clip(clip.x, clip.y, clip.w, clip.h);
        if !crate::desktop::presentation_active() {
            render_desktop_chrome();
        }

        if have_cursor
            && crate::desktop::cursor::gui_cursor_intersects_rect(
                cursor_x,
                cursor_y,
                clip.x,
                clip.y,
                clip.w,
                clip.h,
            ) != 0
        {
            render_cursor(true, cursor_x, cursor_y);
        }

        fb.present_rect(clip.x, clip.y, clip.w, clip.h);
        flipped_pixels = flipped_pixels.saturating_add(clip.w as u64 * clip.h as u64);
    }

    fb.clear_clip();
    commit_cursor(have_cursor, cursor_x, cursor_y);

    let composited_pixels = fb.take_pixels_written();
    unsafe { gui_compositor_perf_partial(composited_pixels, flipped_pixels) };
    0
}
