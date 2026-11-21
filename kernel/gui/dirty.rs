//! Dynamic screen damage tracker.
//!
//! Damage is represented by a screen-sized tile bitmap rather than a bounded
//! array of rectangles.  The number of independent invalidations therefore
//! does not have an artificial ceiling.  Dirty tiles are compacted into
//! dynamically allocated rectangles when the compositor drains the tracker.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::kvec::{AllocError, KVec};

const TILE_SIZE: u32 = 16;

#[derive(Copy, Clone, Debug)]
#[repr(C)]
pub struct DirtyRect {
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
}

#[derive(Copy, Clone)]
pub(crate) struct Run {
    first: u32,
    end: u32,
    rect_index: usize,
}

struct DirtyGrid {
    width: u32,
    height: u32,
    columns: usize,
    rows: usize,
    bits: KVec<u64>,
    spare_bits: KVec<u64>,
}

impl DirtyGrid {
    fn new(width: u32, height: u32) -> Result<Self, AllocError> {
        if width == 0 || height == 0
            || width > i32::MAX as u32
            || height > i32::MAX as u32
        {
            return Err(AllocError);
        }
        let columns = ((width as u64 + TILE_SIZE as u64 - 1) / TILE_SIZE as u64) as usize;
        let rows = ((height as u64 + TILE_SIZE as u64 - 1) / TILE_SIZE as u64) as usize;
        let tile_count = columns.checked_mul(rows).ok_or(AllocError)?;
        let word_count = tile_count.checked_add(63).ok_or(AllocError)? / 64;

        let mut bits = KVec::new();
        let mut spare_bits = KVec::new();
        bits.try_resize(word_count, 0)?;
        spare_bits.try_resize(word_count, 0)?;

        Ok(Self { width, height, columns, rows, bits, spare_bits })
    }

    #[inline]
    fn clear(&mut self) {
        self.bits.as_mut_slice().fill(0);
    }

    fn mark_rect(&mut self, x: i32, y: i32, w: u32, h: u32) {
        if w == 0 || h == 0 {
            return;
        }

        let left = (x as i64).max(0);
        let top = (y as i64).max(0);
        let right = (x as i64 + w as i64).min(self.width as i64);
        let bottom = (y as i64 + h as i64).min(self.height as i64);
        if right <= left || bottom <= top {
            return;
        }

        let first_x = left as u32 / TILE_SIZE;
        let first_y = top as u32 / TILE_SIZE;
        let last_x = ((right - 1) as u32) / TILE_SIZE;
        let last_y = ((bottom - 1) as u32) / TILE_SIZE;

        for ty in first_y..=last_y {
            let row_base = ty as usize * self.columns;
            for tx in first_x..=last_x {
                let bit = row_base + tx as usize;
                self.bits.as_mut_slice()[bit / 64] |= 1u64 << (bit % 64);
            }
        }
    }
}

struct Global<T>(UnsafeCell<T>);
unsafe impl<T: Send> Sync for Global<T> {}

extern "C" {
    // Tiny C wrappers call the kernel's arch_irq_save/restore inline API.
    // This avoids depending on whether the architecture helpers are exported
    // as linker-visible symbols.
    fn gui_dirty_irq_save() -> u64;
    fn gui_dirty_irq_restore(flags: u64);
}

static DIRTY_LOCK: AtomicBool = AtomicBool::new(false);
static DIRTY_GRID: Global<Option<DirtyGrid>> = Global(UnsafeCell::new(None));

struct DirtyGuard {
    irq_flags: u64,
}

#[inline]
fn lock() -> DirtyGuard {
    // Match the kernel's irq-save spinlock discipline. Without this, a local
    // interrupt/preempting context could spin forever on a lock held by the
    // interrupted compositor thread.
    let irq_flags = unsafe { gui_dirty_irq_save() };
    while DIRTY_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
    DirtyGuard { irq_flags }
}

impl Drop for DirtyGuard {
    #[inline]
    fn drop(&mut self) {
        DIRTY_LOCK.store(false, Ordering::Release);
        unsafe { gui_dirty_irq_restore(self.irq_flags) };
    }
}

/// Initialise/reinitialise the damage bitmap for the active display mode.
/// Call after gfx_init() and before the compositor thread starts.
#[no_mangle]
pub extern "C" fn gui_dirty_init(width: u32, height: u32) -> i32 {
    let next = match DirtyGrid::new(width, height) {
        Ok(grid) => grid,
        Err(_) => return -1,
    };

    let _guard = lock();
    unsafe { *DIRTY_GRID.0.get() = Some(next) };
    0
}

/// Clear pending damage. The grid allocation is retained for reuse.
#[no_mangle]
pub extern "C" fn gui_dirty_reset() {
    let _guard = lock();
    unsafe {
        if let Some(grid) = (&mut *DIRTY_GRID.0.get()).as_mut() {
            grid.clear();
        }
    }
}

/// Mark a screen-space rectangle. No allocation occurs on this path.
#[no_mangle]
pub extern "C" fn gui_dirty_add(x: i32, y: i32, w: u32, h: u32) -> i32 {
    if w == 0 || h == 0 {
        return 0;
    }

    let _guard = lock();
    let result = unsafe {
        match (&mut *DIRTY_GRID.0.get()).as_mut() {
            Some(grid) => {
                grid.mark_rect(x, y, w, h);
                0
            }
            None => -1,
        }
    };
    result
}

#[no_mangle]
pub extern "C" fn gui_dirty_is_empty() -> i32 {
    let _guard = lock();
    let empty = unsafe {
        match (&*DIRTY_GRID.0.get()).as_ref() {
            Some(grid) => grid.bits.as_slice().iter().all(|word| *word == 0),
            None => true,
        }
    };
    if empty { 1 } else { 0 }
}

/// Drain dirty tiles and compact them into rectangles. This is a Rust-internal
/// API used by compositor.rs; there is no caller-provided fixed-size output
/// array and no 64-rectangle limit.
pub(crate) fn take_dirty_rects_into(
    output: &mut KVec<DirtyRect>,
    previous: &mut KVec<Run>,
    current: &mut KVec<Run>,
) -> Result<(), AllocError> {
    output.clear();
    previous.clear();
    current.clear();

    // Detach the previous bitmap under the lock, then compress it without
    // blocking producers. The spare bitmap becomes the new live bitmap.
    let (width, height, columns, rows, mut snapshot) = {
        let detached = {
            let _guard = lock();
            unsafe {
                match (&mut *DIRTY_GRID.0.get()).as_mut() {
                    Some(grid) => {
                        core::mem::swap(&mut grid.bits, &mut grid.spare_bits);
                        grid.bits.as_mut_slice().fill(0);
                        let old = core::mem::replace(&mut grid.spare_bits, KVec::new());
                        Some((grid.width, grid.height, grid.columns, grid.rows, old))
                    }
                    None => None,
                }
            }
        };
        match detached {
            Some(value) => value,
            None => return Err(AllocError),
        }
    };

    let result = compress_tiles(
        width,
        height,
        columns,
        rows,
        &snapshot,
        output,
        previous,
        current,
    );

    // Recycle the detached bitmap for the next drain. Always clear it, even
    // after OOM: the compositor will promote that frame to a full redraw.
    snapshot.as_mut_slice().fill(0);
    {
        let _guard = lock();
        unsafe {
            if let Some(grid) = (&mut *DIRTY_GRID.0.get()).as_mut() {
                if grid.width == width && grid.height == height {
                    grid.spare_bits = snapshot;
                }
            }
        }
    }

    result
}

#[inline]
fn tile_is_dirty(
    columns: usize,
    snapshot: &KVec<u64>,
    x: usize,
    y: usize,
) -> bool {
    let index = y * columns + x;
    let word = index / 64;
    let bit = index % 64;

    snapshot
        .get(word)
        .map_or(false, |value| (*value & (1u64 << bit)) != 0)
}

fn compress_tiles(
    width: u32,
    height: u32,
    columns: usize,
    rows: usize,
    snapshot: &KVec<u64>,
    output: &mut KVec<DirtyRect>,
    previous: &mut KVec<Run>,
    current: &mut KVec<Run>,
) -> Result<(), AllocError> {
    previous.try_reserve_exact(columns)?;
    current.try_reserve_exact(columns)?;

    for row in 0..rows {
        current.clear();
        let mut x = 0usize;

        while x < columns {
            if !tile_is_dirty(columns, snapshot, x, row) {
                x += 1;
                continue;
            }

            let first = x;
            while x < columns && tile_is_dirty(columns, snapshot, x, row) {
                x += 1;
            }
            let end = x;

            let previous_rect = previous.as_slice().binary_search_by(|run| {
                (run.first, run.end).cmp(&(first as u32, end as u32))
            }).ok().map(|index| previous.as_slice()[index].rect_index);

            let y = row as u32 * TILE_SIZE;
            let row_height = TILE_SIZE.min(height - y);
            if let Some(rect_index) = previous_rect {
                if let Some(rect) = output.get_mut(rect_index) {
                    rect.h = rect.h.saturating_add(row_height);
                }
                current.try_push(Run {
                    first: first as u32,
                    end: end as u32,
                    rect_index,
                })?;
            } else {
                let left = first as u32 * TILE_SIZE;
                let right = (end as u32 * TILE_SIZE).min(width);
                let rect_index = output.len();
                output.try_push(DirtyRect {
                    x: left as i32,
                    y: y as i32,
                    w: right - left,
                    h: row_height,
                })?;
                current.try_push(Run {
                    first: first as u32,
                    end: end as u32,
                    rect_index,
                })?;
            }
        }

        core::mem::swap(previous, current);
    }

    Ok(())
}

