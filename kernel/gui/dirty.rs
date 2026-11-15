//! Kernel GUI dirty-rectangle tracker.
//!
//! The public C ABI is intentionally unchanged. Rectangles are kept as a
//! bounded list and overlapping or nearby rectangles are merged before the
//! compositor snapshots them.

use core::sync::atomic::{AtomicBool, Ordering};

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

static mut DIRTY_RECTS: [DirtyRect; MAX_DIRTY_RECTS] = [
    DirtyRect { x: 0, y: 0, w: 0, h: 0 }; MAX_DIRTY_RECTS
];
static mut DIRTY_COUNT: usize = 0;
static mut DIRTY_OVERFLOW: bool = false;

#[inline]
fn dirty_lock() {
    while DIRTY_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

#[inline]
fn dirty_unlock() {
    DIRTY_LOCK.store(false, Ordering::Release);
}

/// Clear all pending rectangles. Called before a full-screen redraw.
#[no_mangle]
pub extern "C" fn gui_dirty_reset() {
    dirty_lock();
    unsafe {
        DIRTY_COUNT = 0;
        DIRTY_OVERFLOW = false;
    }
    dirty_unlock();
}

/// Add a screen-space rectangle. Returns -1 on bounded-list overflow.
#[no_mangle]
pub extern "C" fn gui_dirty_add(x: i32, y: i32, w: u32, h: u32) -> i32 {
    if w == 0 || h == 0 {
        return 0;
    }

    dirty_lock();
    let result = unsafe { add_locked(x, y, w, h) };
    dirty_unlock();
    result
}

unsafe fn add_locked(x: i32, y: i32, w: u32, h: u32) -> i32 {
    if DIRTY_OVERFLOW {
        return -1;
    }

    let nx1 = x as i64 + w as i64;
    let ny1 = y as i64 + h as i64;

    let mut i = 0;
    while i < DIRTY_COUNT {
        let rect = &mut DIRTY_RECTS[i];
        let rx1 = rect.x as i64 + rect.w as i64;
        let ry1 = rect.y as i64 + rect.h as i64;

        if x as i64 <= rx1 + MERGE_PAD
            && nx1 + MERGE_PAD >= rect.x as i64
            && y as i64 <= ry1 + MERGE_PAD
            && ny1 + MERGE_PAD >= rect.y as i64
        {
            let mx = (rect.x as i64).min(x as i64);
            let my = (rect.y as i64).min(y as i64);
            let mx1 = rx1.max(nx1);
            let my1 = ry1.max(ny1);

            rect.x = mx as i32;
            rect.y = my as i32;
            rect.w = (mx1 - mx) as u32;
            rect.h = (my1 - my) as u32;

            merge_all_locked();
            return 0;
        }
        i += 1;
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

unsafe fn merge_all_locked() {
    let mut changed = true;

    while changed {
        changed = false;
        let mut i = 0;

        while i < DIRTY_COUNT {
            let mut j = i + 1;

            while j < DIRTY_COUNT {
                let a = DIRTY_RECTS[i];
                let b = DIRTY_RECTS[j];

                let ax = a.x as i64;
                let ay = a.y as i64;
                let ax1 = ax + a.w as i64;
                let ay1 = ay + a.h as i64;

                let bx = b.x as i64;
                let by = b.y as i64;
                let bx1 = bx + b.w as i64;
                let by1 = by + b.h as i64;

                if ax <= bx1 + MERGE_PAD
                    && ax1 + MERGE_PAD >= bx
                    && ay <= by1 + MERGE_PAD
                    && ay1 + MERGE_PAD >= by
                {
                    let mx = ax.min(bx);
                    let my = ay.min(by);
                    let mx1 = ax1.max(bx1);
                    let my1 = ay1.max(by1);

                    DIRTY_RECTS[i] = DirtyRect {
                        x: mx as i32,
                        y: my as i32,
                        w: (mx1 - mx) as u32,
                        h: (my1 - my) as u32,
                    };

                    DIRTY_COUNT -= 1;
                    DIRTY_RECTS[j] = DIRTY_RECTS[DIRTY_COUNT];
                    changed = true;
                } else {
                    j += 1;
                }
            }
            i += 1;
        }
    }
}

/// Copy up to `max` pending rectangles to `out`, then clear the list.
#[no_mangle]
pub unsafe extern "C" fn gui_dirty_snapshot(out: *mut DirtyRect, max: usize) -> usize {
    if out.is_null() || max == 0 {
        return 0;
    }

    dirty_lock();
    let count = DIRTY_COUNT.min(max);

    for i in 0..count {
        core::ptr::write(out.add(i), DIRTY_RECTS[i]);
    }

    DIRTY_COUNT = 0;
    DIRTY_OVERFLOW = false;
    dirty_unlock();
    count
}

/// Returns 1 when there are no pending rectangles, otherwise 0.
#[no_mangle]
pub extern "C" fn gui_dirty_is_empty() -> i32 {
    dirty_lock();
    let empty = unsafe { DIRTY_COUNT == 0 };
    dirty_unlock();
    if empty { 1 } else { 0 }
}
