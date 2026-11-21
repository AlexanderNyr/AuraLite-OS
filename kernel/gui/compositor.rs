//! Data-oriented Rust screen compositor for AuraLite.
//!
//! The compositor owns scene ordering, child traversal, clipping, render
//! command generation, damage consumption and presentation.  Window lifecycle
//! and physical surface allocation are still behind a narrow compatibility
//! boundary in gui.c; C emits scalar window metadata into the pre-reserved SoA
//! snapshot and never receives pointers to Rust-owned arrays.
//!
//! TODO(surface-manager): move window surfaces and their leases into Rust.
//! Until then `gui_surface_render_window_compat(...)` is the only per-window
//! compatibility call. It resolves the C-owned surface and invokes render.rs
//! synchronously. No surface pointer escapes that call. The scalar scene
//! snapshot callback is also temporary until WindowStore is Rust-owned.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::dirty::{self, DirtyRect};
use crate::framebuffer::SystemFramebuffer;
use crate::kvec::{AllocError, KVec};

const GUI_WIN_ALWAYS_TOP: u32 = 0x080;

extern "C" {
    /// Current allocated slot capacity, not the number of live windows.
    fn gui_compositor_window_capacity() -> usize;

    /// With gui_lock held, emit every live window as scalar arguments to
    /// gui_rust_scene_snapshot_push(). No C pointer or C-owned array crosses
    /// the ABI. Returns -1 if the slot capacity changed since it was queried.
    fn gui_compositor_emit_snapshot(expected_capacity: usize) -> i32;

    /// Draw one C-owned window surface using geometry resolved by the Rust scene.
    /// C revalidates the generation and holds the surface lifetime while the
    /// Rust window renderer consumes it.
    fn gui_surface_render_window_compat(
        wid: i32, generation: u32,
        screen_x: i32, screen_y: i32, width: u32, height: u32,
        content_x: i32, content_y: i32, content_width: u32, content_height: u32,
    );

    /// 0 = no snap preview, GUI_SNAP_* values otherwise.
    fn gui_compositor_get_snap_preview() -> u32;
    fn gui_compositor_get_taskbar_height() -> u32;
    fn gui_compositor_has_content_damage() -> i32;

    fn gui_compositor_perf_full(composited_pixels: u64, flipped_pixels: u64);
    fn gui_compositor_perf_partial(composited_pixels: u64, flipped_pixels: u64);

    fn gfx_fill_rect(x: i32, y: i32, width: u32, height: u32, color: u32);
    fn gfx_get_width() -> u32;
    fn gfx_get_height() -> u32;
}

struct Global<T>(UnsafeCell<T>);
unsafe impl<T: Send> Sync for Global<T> {}

static COMPOSITOR_ACTIVE: AtomicBool = AtomicBool::new(false);
static FULL_DIRTY: AtomicBool = AtomicBool::new(true);
static COMPOSITOR_STATE: Global<Option<CompositorState>> = Global(UnsafeCell::new(None));

/// Request a complete composition on the next compositor tick.
#[no_mangle]
pub extern "C" fn gui_rust_compositor_request_full() {
    FULL_DIRTY.store(true, Ordering::Release);
}

#[no_mangle]
pub extern "C" fn gui_rust_compositor_is_full_dirty() -> i32 {
    if FULL_DIRTY.load(Ordering::Acquire) { 1 } else { 0 }
}

/// Used by the kernel compositor thread's sleep/drain loop.
#[no_mangle]
pub extern "C" fn gui_rust_compositor_has_pending_work() -> i32 {
    if FULL_DIRTY.load(Ordering::Acquire)
        || dirty::gui_dirty_is_empty() == 0
        || unsafe { gui_compositor_has_content_damage() != 0 }
    {
        1
    } else {
        0
    }
}

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

#[derive(Clone, Copy)]
struct Clip {
    left: i64,
    top: i64,
    right: i64,
    bottom: i64,
}

impl Clip {
    #[inline]
    fn from_dirty(rect: DirtyRect) -> Self {
        Self {
            left: rect.x as i64,
            top: rect.y as i64,
            right: rect.x as i64 + rect.w as i64,
            bottom: rect.y as i64 + rect.h as i64,
        }
    }

    #[inline]
    fn intersect(self, x: i64, y: i64, width: u32, height: u32) -> Option<Self> {
        let left = self.left.max(x);
        let top = self.top.max(y);
        let right = self.right.min(x + width as i64);
        let bottom = self.bottom.min(y + height as i64);
        if right <= left || bottom <= top {
            None
        } else {
            Some(Self { left, top, right, bottom })
        }
    }

    #[inline]
    fn to_dirty(self) -> DirtyRect {
        DirtyRect {
            x: self.left as i32,
            y: self.top as i32,
            w: (self.right - self.left) as u32,
            h: (self.bottom - self.top) as u32,
        }
    }
}

/// All per-window values consumed while building a frame are kept in separate
/// columns. `slot_to_scene` maps the public C slot id to the compact snapshot
/// row; the other index arrays contain compact scene-row indices.
struct SceneSoA {
    ids: KVec<i32>,
    generations: KVec<u32>,
    parents: KVec<i32>,
    z_order: KVec<i32>,
    flags: KVec<u32>,
    visible: KVec<u8>,
    minimized: KVec<u8>,
    content_dirty: KVec<u8>,

    // x/y are parent-content-relative for child windows and screen-relative
    // for roots. Absolute coordinates are resolved by the Rust scene walk.
    x: KVec<i32>,
    y: KVec<i32>,
    absolute_x: KVec<i32>,
    absolute_y: KVec<i32>,
    renderable: KVec<u8>,
    width: KVec<u32>,
    height: KVec<u32>,
    content_offset_x: KVec<i32>,
    content_offset_y: KVec<i32>,
    content_width: KVec<u32>,
    content_height: KVec<u32>,
    transform_stack: KVec<usize>,

    slot_to_scene: KVec<i32>,
    child_order: KVec<usize>,
    child_begin: KVec<usize>,
    child_end: KVec<usize>,
    roots_normal: KVec<usize>,
    roots_always_top: KVec<usize>,
    slot_capacity: usize,
}

impl SceneSoA {
    const fn new() -> Self {
        Self {
            ids: KVec::new(),
            generations: KVec::new(),
            parents: KVec::new(),
            z_order: KVec::new(),
            flags: KVec::new(),
            visible: KVec::new(),
            minimized: KVec::new(),
            content_dirty: KVec::new(),
            x: KVec::new(),
            y: KVec::new(),
            absolute_x: KVec::new(),
            absolute_y: KVec::new(),
            renderable: KVec::new(),
            width: KVec::new(),
            height: KVec::new(),
            content_offset_x: KVec::new(),
            content_offset_y: KVec::new(),
            content_width: KVec::new(),
            content_height: KVec::new(),
            transform_stack: KVec::new(),
            slot_to_scene: KVec::new(),
            child_order: KVec::new(),
            child_begin: KVec::new(),
            child_end: KVec::new(),
            roots_normal: KVec::new(),
            roots_always_top: KVec::new(),
            slot_capacity: 0,
        }
    }

    fn reserve_snapshot(&mut self, capacity: usize) -> Result<(), AllocError> {
        macro_rules! reserve {
            ($($field:ident),+ $(,)?) => { $(self.$field.try_reserve_exact(capacity)?;)+ };
        }
        reserve!(
            ids, generations, parents, z_order, flags, visible, minimized,
            content_dirty, x, y, absolute_x, absolute_y, renderable, width, height,
            content_offset_x, content_offset_y, content_width, content_height,
            child_order, roots_normal, roots_always_top, transform_stack
        );
        self.slot_to_scene.try_resize(capacity, -1)?;
        self.child_begin.try_resize(capacity, 0)?;
        self.child_end.try_resize(capacity, 0)?;

        self.ids.clear();
        self.generations.clear();
        self.parents.clear();
        self.z_order.clear();
        self.flags.clear();
        self.visible.clear();
        self.minimized.clear();
        self.content_dirty.clear();
        self.x.clear();
        self.y.clear();
        self.absolute_x.clear();
        self.absolute_y.clear();
        self.renderable.clear();
        self.width.clear();
        self.height.clear();
        self.content_offset_x.clear();
        self.content_offset_y.clear();
        self.content_width.clear();
        self.content_height.clear();
        self.transform_stack.clear();
        self.child_order.clear();
        self.roots_normal.clear();
        self.roots_always_top.clear();
        self.slot_to_scene.as_mut_slice().fill(-1);
        self.child_begin.as_mut_slice().fill(0);
        self.child_end.as_mut_slice().fill(0);
        self.slot_capacity = capacity;
        Ok(())
    }

    /// This is called only by gui_rust_scene_snapshot_push while the compositor
    /// guard is held. Capacity was reserved before C acquired gui_lock, so
    /// these appends never invoke the allocator while gui_lock is held.
    fn push_snapshot(
        &mut self,
        id: i32,
        generation: u32,
        parent: i32,
        z: i32,
        flags: u32,
        visible: u8,
        minimized: u8,
        content_dirty: u8,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        content_offset_x: i32,
        content_offset_y: i32,
        content_width: u32,
        content_height: u32,
    ) -> Result<(), AllocError> {
        if id < 0 || id as usize >= self.slot_capacity || self.ids.len() >= self.slot_capacity {
            return Err(AllocError);
        }

        let row = self.ids.len();
        self.ids.push_no_grow(id)?;
        self.generations.push_no_grow(generation)?;
        self.parents.push_no_grow(parent)?;
        self.z_order.push_no_grow(z)?;
        self.flags.push_no_grow(flags)?;
        self.visible.push_no_grow(visible)?;
        self.minimized.push_no_grow(minimized)?;
        self.content_dirty.push_no_grow(content_dirty)?;
        self.x.push_no_grow(x)?;
        self.y.push_no_grow(y)?;
        self.absolute_x.push_no_grow(0)?;
        self.absolute_y.push_no_grow(0)?;
        self.renderable.push_no_grow(0)?;
        self.width.push_no_grow(width)?;
        self.height.push_no_grow(height)?;
        self.content_offset_x.push_no_grow(content_offset_x)?;
        self.content_offset_y.push_no_grow(content_offset_y)?;
        self.content_width.push_no_grow(content_width)?;
        self.content_height.push_no_grow(content_height)?;

        self.slot_to_scene.as_mut_slice()[id as usize] = row as i32;
        Ok(())
    }

    fn build_indices(&mut self) -> Result<(), AllocError> {
        self.child_order.clear();
        self.roots_normal.clear();
        self.roots_always_top.clear();
        self.slot_to_scene.as_mut_slice().fill(-1);

        for row in 0..self.ids.len() {
            let id = self.ids.as_slice()[row];
            if id >= 0 && (id as usize) < self.slot_capacity {
                self.slot_to_scene.as_mut_slice()[id as usize] = row as i32;
            }
        }

        for row in 0..self.ids.len() {
            if self.visible.as_slice()[row] == 0 || self.minimized.as_slice()[row] != 0 {
                continue;
            }

            let parent = self.parents.as_slice()[row];
            if parent < 0 {
                if self.flags.as_slice()[row] & GUI_WIN_ALWAYS_TOP != 0 {
                    self.roots_always_top.push_no_grow(row)?;
                } else {
                    self.roots_normal.push_no_grow(row)?;
                }
            } else if (parent as usize) < self.slot_capacity
                && self.slot_to_scene.as_slice()[parent as usize] >= 0
            {
                self.child_order.push_no_grow(row)?;
            }
        }

        let parents = self.parents.as_slice();
        let z_order = self.z_order.as_slice();
        let ids = self.ids.as_slice();
        self.child_order.as_mut_slice().sort_unstable_by(|&a, &b| {
            parents[a]
                .cmp(&parents[b])
                .then_with(|| z_order[a].cmp(&z_order[b]))
                .then_with(|| ids[a].cmp(&ids[b]))
        });

        let z_order = self.z_order.as_slice();
        let ids = self.ids.as_slice();
        self.roots_normal.as_mut_slice().sort_unstable_by(|&a, &b| {
            z_order[a].cmp(&z_order[b]).then_with(|| ids[a].cmp(&ids[b]))
        });
        self.roots_always_top.as_mut_slice().sort_unstable_by(|&a, &b| {
            z_order[a].cmp(&z_order[b]).then_with(|| ids[a].cmp(&ids[b]))
        });

        let mut cursor = 0usize;
        for parent_id in 0..self.slot_capacity {
            self.child_begin.as_mut_slice()[parent_id] = cursor;
            while cursor < self.child_order.len() {
                let row = self.child_order.as_slice()[cursor];
                if self.parents.as_slice()[row] != parent_id as i32 {
                    break;
                }
                cursor += 1;
            }
            self.child_end.as_mut_slice()[parent_id] = cursor;
        }

        self.resolve_transforms()
    }

    fn resolve_transforms(&mut self) -> Result<(), AllocError> {
        self.absolute_x.as_mut_slice().fill(0);
        self.absolute_y.as_mut_slice().fill(0);
        self.renderable.as_mut_slice().fill(0);
        self.transform_stack.clear();

        // Roots already have screen-space positions. Descendant positions are
        // resolved from their parent's content origin, not by C recursion.
        for root_index in 0..self.roots_normal.len() {
            let root_row = self.roots_normal.as_slice()[root_index];
            self.absolute_x.as_mut_slice()[root_row] = self.x.as_slice()[root_row];
            self.absolute_y.as_mut_slice()[root_row] = self.y.as_slice()[root_row];
            self.renderable.as_mut_slice()[root_row] = 1;
            self.transform_stack.push_no_grow(root_row)?;
        }
        for root_index in 0..self.roots_always_top.len() {
            let root_row = self.roots_always_top.as_slice()[root_index];
            self.absolute_x.as_mut_slice()[root_row] = self.x.as_slice()[root_row];
            self.absolute_y.as_mut_slice()[root_row] = self.y.as_slice()[root_row];
            self.renderable.as_mut_slice()[root_row] = 1;
            self.transform_stack.push_no_grow(root_row)?;
        }

        while !self.transform_stack.is_empty() {
            let last = self.transform_stack.len() - 1;
            let parent_row = *self.transform_stack.get(last).ok_or(AllocError)?;
            self.transform_stack.truncate(last);

            let parent_id = self.ids.as_slice()[parent_row];
            if parent_id < 0 || parent_id as usize >= self.slot_capacity {
                continue;
            }

            let content_origin_x = self.absolute_x.as_slice()[parent_row] as i64
                + self.content_offset_x.as_slice()[parent_row] as i64;
            let content_origin_y = self.absolute_y.as_slice()[parent_row] as i64
                + self.content_offset_y.as_slice()[parent_row] as i64;
            let begin = self.child_begin.as_slice()[parent_id as usize];
            let end = self.child_end.as_slice()[parent_id as usize];

            for child_index in begin..end {
                let child_row = self.child_order.as_slice()[child_index];
                let child_x = content_origin_x + self.x.as_slice()[child_row] as i64;
                let child_y = content_origin_y + self.y.as_slice()[child_row] as i64;
                self.absolute_x.as_mut_slice()[child_row] = clamp_i64_to_i32(child_x);
                self.absolute_y.as_mut_slice()[child_row] = clamp_i64_to_i32(child_y);
                self.renderable.as_mut_slice()[child_row] = 1;
                self.transform_stack.push_no_grow(child_row)?;
            }
        }

        Ok(())
    }

    fn collect_content_damage(&self) -> Result<(), AllocError> {
        for row in 0..self.ids.len() {
            if self.content_dirty.as_slice()[row] == 0
                || self.renderable.as_slice()[row] == 0
            {
                continue;
            }
            let rc = dirty::gui_dirty_add(
                self.absolute_x.as_slice()[row],
                self.absolute_y.as_slice()[row],
                self.width.as_slice()[row],
                self.height.as_slice()[row],
            );
            if rc < 0 {
                return Err(AllocError);
            }
        }
        Ok(())
    }
}

#[inline]
fn clamp_i64_to_i32(value: i64) -> i32 {
    value.clamp(i32::MIN as i64, i32::MAX as i64) as i32
}

/// Render commands use SoA throughout. Renderer iteration reads each column
/// directly; no temporary AoS RenderItem is reconstructed per window.
struct FrameSoA {
    ids: KVec<i32>,
    generations: KVec<u32>,
    screen_x: KVec<i32>,
    screen_y: KVec<i32>,
    width: KVec<u32>,
    height: KVec<u32>,
    content_x: KVec<i32>,
    content_y: KVec<i32>,
    content_width: KVec<u32>,
    content_height: KVec<u32>,
    clip_x: KVec<i32>,
    clip_y: KVec<i32>,
    clip_width: KVec<u32>,
    clip_height: KVec<u32>,
}

impl FrameSoA {
    const fn new() -> Self {
        Self {
            ids: KVec::new(),
            generations: KVec::new(),
            screen_x: KVec::new(),
            screen_y: KVec::new(),
            width: KVec::new(),
            height: KVec::new(),
            content_x: KVec::new(),
            content_y: KVec::new(),
            content_width: KVec::new(),
            content_height: KVec::new(),
            clip_x: KVec::new(),
            clip_y: KVec::new(),
            clip_width: KVec::new(),
            clip_height: KVec::new(),
        }
    }

    fn reserve(&mut self, capacity: usize) -> Result<(), AllocError> {
        self.ids.try_reserve_exact(capacity)?;
        self.generations.try_reserve_exact(capacity)?;
        self.screen_x.try_reserve_exact(capacity)?;
        self.screen_y.try_reserve_exact(capacity)?;
        self.width.try_reserve_exact(capacity)?;
        self.height.try_reserve_exact(capacity)?;
        self.content_x.try_reserve_exact(capacity)?;
        self.content_y.try_reserve_exact(capacity)?;
        self.content_width.try_reserve_exact(capacity)?;
        self.content_height.try_reserve_exact(capacity)?;
        self.clip_x.try_reserve_exact(capacity)?;
        self.clip_y.try_reserve_exact(capacity)?;
        self.clip_width.try_reserve_exact(capacity)?;
        self.clip_height.try_reserve_exact(capacity)?;
        Ok(())
    }

    fn clear(&mut self) {
        self.ids.clear();
        self.generations.clear();
        self.screen_x.clear();
        self.screen_y.clear();
        self.width.clear();
        self.height.clear();
        self.content_x.clear();
        self.content_y.clear();
        self.content_width.clear();
        self.content_height.clear();
        self.clip_x.clear();
        self.clip_y.clear();
        self.clip_width.clear();
        self.clip_height.clear();
    }

    fn push(
        &mut self, id: i32, generation: u32,
        screen_x: i64, screen_y: i64, width: u32, height: u32,
        content_x: i64, content_y: i64, content_width: u32, content_height: u32,
        clip: Clip,
    ) -> Result<(), AllocError> {
        self.ids.push_no_grow(id)?;
        self.generations.push_no_grow(generation)?;
        self.screen_x.push_no_grow(clamp_i64_to_i32(screen_x))?;
        self.screen_y.push_no_grow(clamp_i64_to_i32(screen_y))?;
        self.width.push_no_grow(width)?;
        self.height.push_no_grow(height)?;
        self.content_x.push_no_grow(clamp_i64_to_i32(content_x))?;
        self.content_y.push_no_grow(clamp_i64_to_i32(content_y))?;
        self.content_width.push_no_grow(content_width)?;
        self.content_height.push_no_grow(content_height)?;
        self.clip_x.push_no_grow(clip.left as i32)?;
        self.clip_y.push_no_grow(clip.top as i32)?;
        self.clip_width.push_no_grow((clip.right - clip.left) as u32)?;
        self.clip_height.push_no_grow((clip.bottom - clip.top) as u32)?;
        Ok(())
    }

    #[inline]
    fn len(&self) -> usize {
        self.ids.len()
    }
}

/// Explicit SoA traversal stack. The only per-level state is clipping and a
/// cursor into the parent's contiguous child range.
struct WalkStackSoA {
    clip_left: KVec<i64>,
    clip_top: KVec<i64>,
    clip_right: KVec<i64>,
    clip_bottom: KVec<i64>,
    content_origin_x: KVec<i64>,
    content_origin_y: KVec<i64>,
    next_child: KVec<usize>,
    child_end: KVec<usize>,
}

impl WalkStackSoA {
    const fn new() -> Self {
        Self {
            clip_left: KVec::new(),
            clip_top: KVec::new(),
            clip_right: KVec::new(),
            clip_bottom: KVec::new(),
            content_origin_x: KVec::new(),
            content_origin_y: KVec::new(),
            next_child: KVec::new(),
            child_end: KVec::new(),
        }
    }

    fn reserve(&mut self, capacity: usize) -> Result<(), AllocError> {
        self.clip_left.try_reserve_exact(capacity)?;
        self.clip_top.try_reserve_exact(capacity)?;
        self.clip_right.try_reserve_exact(capacity)?;
        self.clip_bottom.try_reserve_exact(capacity)?;
        self.content_origin_x.try_reserve_exact(capacity)?;
        self.content_origin_y.try_reserve_exact(capacity)?;
        self.next_child.try_reserve_exact(capacity)?;
        self.child_end.try_reserve_exact(capacity)?;
        Ok(())
    }

    fn clear(&mut self) {
        self.clip_left.clear();
        self.clip_top.clear();
        self.clip_right.clear();
        self.clip_bottom.clear();
        self.content_origin_x.clear();
        self.content_origin_y.clear();
        self.next_child.clear();
        self.child_end.clear();
    }

    fn push(
        &mut self, clip: Clip, content_origin_x: i64, content_origin_y: i64,
        begin: usize, end: usize,
    ) -> Result<(), AllocError> {
        self.clip_left.push_no_grow(clip.left)?;
        self.clip_top.push_no_grow(clip.top)?;
        self.clip_right.push_no_grow(clip.right)?;
        self.clip_bottom.push_no_grow(clip.bottom)?;
        self.content_origin_x.push_no_grow(content_origin_x)?;
        self.content_origin_y.push_no_grow(content_origin_y)?;
        self.next_child.push_no_grow(begin)?;
        self.child_end.push_no_grow(end)?;
        Ok(())
    }

    #[inline]
    fn len(&self) -> usize {
        self.next_child.len()
    }

    fn pop(&mut self) {
        let new_len = self.len().saturating_sub(1);
        // Every column stores Copy values, so truncation needs no destructor.
        self.clip_left.truncate(new_len);
        self.clip_top.truncate(new_len);
        self.clip_right.truncate(new_len);
        self.clip_bottom.truncate(new_len);
        self.content_origin_x.truncate(new_len);
        self.content_origin_y.truncate(new_len);
        self.next_child.truncate(new_len);
        self.child_end.truncate(new_len);
    }
}

struct CompositorState {
    scene: SceneSoA,
    frame: FrameSoA,
    stack: WalkStackSoA,
    dirty_rects: KVec<DirtyRect>,
    dirty_previous_runs: KVec<dirty::Run>,
    dirty_current_runs: KVec<dirty::Run>,
}

impl CompositorState {
    const fn new() -> Self {
        Self {
            scene: SceneSoA::new(),
            frame: FrameSoA::new(),
            stack: WalkStackSoA::new(),
            dirty_rects: KVec::new(),
            dirty_previous_runs: KVec::new(),
            dirty_current_runs: KVec::new(),
        }
    }

    fn reserve_snapshot(&mut self, capacity: usize) -> Result<(), AllocError> {
        self.scene.reserve_snapshot(capacity)?;
        self.frame.reserve(capacity)?;
        self.stack.reserve(capacity)?;
        Ok(())
    }
}

unsafe fn state_ptr() -> *mut CompositorState {
    // Return a raw pointer rather than &'static mut: the C snapshot adapter
    // synchronously re-enters through gui_rust_scene_snapshot_push(), so a
    // long-lived mutable reference here would alias the callback's access.
    let slot = unsafe { &mut *COMPOSITOR_STATE.0.get() };
    if slot.is_none() {
        *slot = Some(CompositorState::new());
    }
    match slot.as_mut() {
        Some(state) => state as *mut CompositorState,
        None => core::ptr::null_mut(),
    }
}

/// Scalar callback from gui_compositor_emit_snapshot().  This is deliberately
/// a value-only ABI: no pointer, slice, C struct, allocator callback or
/// temporary bridge buffer is passed across the language boundary.
#[no_mangle]
pub extern "C" fn gui_rust_scene_snapshot_push(
    id: i32,
    generation: u32,
    parent: i32,
    z: i32,
    flags: u32,
    visible: u32,
    minimized: u32,
    content_dirty: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    content_offset_x: i32,
    content_offset_y: i32,
    content_width: u32,
    content_height: u32,
) -> i32 {
    if !COMPOSITOR_ACTIVE.load(Ordering::Acquire) {
        return -1;
    }
    let state = unsafe { state_ptr() };
    if state.is_null() {
        return -1;
    }
    let result = unsafe {
        (*state).scene.push_snapshot(
            id,
            generation,
            parent,
            z,
            flags,
            visible as u8,
            minimized as u8,
            content_dirty as u8,
            x,
            y,
            width,
            height,
            content_offset_x,
            content_offset_y,
            content_width,
            content_height,
        )
    };
    if result.is_ok() { 0 } else { -1 }
}

unsafe fn snapshot_scene(
    state: *mut CompositorState,
    collect_content_damage: bool,
) -> Result<(), i32> {
    // Do not keep an `&mut CompositorState` alive across the C call: C invokes
    // gui_rust_scene_snapshot_push synchronously, and that callback accesses
    // the same global state. Raw-pointer phases keep the two exclusive borrows
    // non-overlapping.
    for _attempt in 0..4 {
        let capacity = unsafe { gui_compositor_window_capacity() };
        unsafe { (*state).reserve_snapshot(capacity) }.map_err(|_| -12)?;

        let rc = unsafe { gui_compositor_emit_snapshot(capacity) };
        if rc == 0 {
            unsafe { (*state).scene.build_indices() }.map_err(|_| -12)?;
            if collect_content_damage {
                unsafe { (*state).scene.collect_content_damage() }.map_err(|_| -12)?;
            }
            return Ok(());
        }

        // Retry only if the C slot table actually grew between the query and
        // snapshot. Other errors are allocation/callback failures.
        let current_capacity = unsafe { gui_compositor_window_capacity() };
        if current_capacity <= capacity {
            return Err(-12);
        }
    }
    Err(-12)
}

#[inline]
fn clip_to_screen(rect: DirtyRect, width: u32, height: u32) -> Option<DirtyRect> {
    if rect.w == 0 || rect.h == 0 || width == 0 || height == 0 {
        return None;
    }
    Clip::from_dirty(rect)
        .intersect(0, 0, width, height)
        .map(Clip::to_dirty)
}

fn append_descendants(
    scene: &SceneSoA,
    frame: &mut FrameSoA,
    stack: &mut WalkStackSoA,
    parent_id: usize,
    parent_content_clip: Clip,
    parent_content_origin_x: i64,
    parent_content_origin_y: i64,
) -> Result<(), AllocError> {
    if parent_id >= scene.slot_capacity {
        return Ok(());
    }
    let begin = scene.child_begin.as_slice()[parent_id];
    let end = scene.child_end.as_slice()[parent_id];
    if begin >= end {
        return Ok(());
    }

    stack.clear();
    stack.push(
        parent_content_clip,
        parent_content_origin_x,
        parent_content_origin_y,
        begin,
        end,
    )?;

    while stack.len() != 0 {
        let top = stack.len() - 1;
        let next = stack.next_child.as_slice()[top];
        let child_end = stack.child_end.as_slice()[top];
        if next >= child_end {
            stack.pop();
            continue;
        }

        stack.next_child.as_mut_slice()[top] = next + 1;
        let child_row = scene.child_order.as_slice()[next];
        let parent_clip = Clip {
            left: stack.clip_left.as_slice()[top],
            top: stack.clip_top.as_slice()[top],
            right: stack.clip_right.as_slice()[top],
            bottom: stack.clip_bottom.as_slice()[top],
        };
        let child_x = stack.content_origin_x.as_slice()[top] as i64
            + scene.x.as_slice()[child_row] as i64;
        let child_y = stack.content_origin_y.as_slice()[top] as i64
            + scene.y.as_slice()[child_row] as i64;

        let Some(child_clip) = parent_clip.intersect(
            child_x,
            child_y,
            scene.width.as_slice()[child_row],
            scene.height.as_slice()[child_row],
        ) else {
            continue;
        };

        let content_origin_x = child_x + scene.content_offset_x.as_slice()[child_row] as i64;
        let content_origin_y = child_y + scene.content_offset_y.as_slice()[child_row] as i64;
        let child_id = scene.ids.as_slice()[child_row];
        frame.push(
            child_id,
            scene.generations.as_slice()[child_row],
            child_x,
            child_y,
            scene.width.as_slice()[child_row],
            scene.height.as_slice()[child_row],
            content_origin_x,
            content_origin_y,
            scene.content_width.as_slice()[child_row],
            scene.content_height.as_slice()[child_row],
            child_clip,
        )?;

        let Some(content_clip) = child_clip.intersect(
            content_origin_x,
            content_origin_y,
            scene.content_width.as_slice()[child_row],
            scene.content_height.as_slice()[child_row],
        ) else {
            continue;
        };

        if child_id >= 0 {
            let child_slot = child_id as usize;
            let child_begin = scene.child_begin.as_slice()[child_slot];
            let child_end = scene.child_end.as_slice()[child_slot];
            if child_begin < child_end {
                stack.push(
                    content_clip,
                    content_origin_x,
                    content_origin_y,
                    child_begin,
                    child_end,
                )?;
            }
        }
    }

    Ok(())
}

fn build_frame_commands(
    scene: &SceneSoA,
    frame: &mut FrameSoA,
    stack: &mut WalkStackSoA,
    base: DirtyRect,
) -> Result<(), AllocError> {
    frame.clear();
    stack.clear();
    let base_clip = Clip::from_dirty(base);

    for group in 0..2 {
        let roots = if group == 0 {
            scene.roots_normal.as_slice()
        } else {
            scene.roots_always_top.as_slice()
        };

        for &root_row in roots {
            let root_x = scene.absolute_x.as_slice()[root_row] as i64;
            let root_y = scene.absolute_y.as_slice()[root_row] as i64;
            let Some(root_clip) = base_clip.intersect(
                root_x,
                root_y,
                scene.width.as_slice()[root_row],
                scene.height.as_slice()[root_row],
            ) else {
                continue;
            };

            let content_origin_x = root_x + scene.content_offset_x.as_slice()[root_row] as i64;
            let content_origin_y = root_y + scene.content_offset_y.as_slice()[root_row] as i64;
            frame.push(
                scene.ids.as_slice()[root_row],
                scene.generations.as_slice()[root_row],
                root_x,
                root_y,
                scene.width.as_slice()[root_row],
                scene.height.as_slice()[root_row],
                content_origin_x,
                content_origin_y,
                scene.content_width.as_slice()[root_row],
                scene.content_height.as_slice()[root_row],
                root_clip,
            )?;

            let Some(content_clip) = root_clip.intersect(
                content_origin_x,
                content_origin_y,
                scene.content_width.as_slice()[root_row],
                scene.content_height.as_slice()[root_row],
            ) else {
                continue;
            };

            let root_id = scene.ids.as_slice()[root_row];
            if root_id >= 0 {
                append_descendants(
                    scene,
                    frame,
                    stack,
                    root_id as usize,
                    content_clip,
                    content_origin_x,
                    content_origin_y,
                )?;
            }
        }
    }

    Ok(())
}

fn render_windows(
    fb: SystemFramebuffer,
    clip: DirtyRect,
    state: &mut CompositorState,
) -> Result<(), i32> {
    build_frame_commands(&state.scene, &mut state.frame, &mut state.stack, clip)
        .map_err(|_| -12)?;

    for index in 0..state.frame.len() {
        let id = state.frame.ids.as_slice()[index];
        let generation = state.frame.generations.as_slice()[index];
        let screen_x = state.frame.screen_x.as_slice()[index];
        let screen_y = state.frame.screen_y.as_slice()[index];
        let window_width = state.frame.width.as_slice()[index];
        let window_height = state.frame.height.as_slice()[index];
        let content_x = state.frame.content_x.as_slice()[index];
        let content_y = state.frame.content_y.as_slice()[index];
        let content_width = state.frame.content_width.as_slice()[index];
        let content_height = state.frame.content_height.as_slice()[index];
        let clip_x = state.frame.clip_x.as_slice()[index];
        let clip_y = state.frame.clip_y.as_slice()[index];
        let clip_width = state.frame.clip_width.as_slice()[index];
        let clip_height = state.frame.clip_height.as_slice()[index];

        if id < 0 || clip_width == 0 || clip_height == 0 {
            continue;
        }

        fb.set_clip(clip_x, clip_y, clip_width, clip_height);
        unsafe {
            gui_surface_render_window_compat(
                id, generation, screen_x, screen_y, window_width, window_height,
                content_x, content_y, content_width, content_height,
            )
        };
    }

    fb.set_clip(clip.x, clip.y, clip.w, clip.h);
    Ok(())
}

#[inline]
fn render_desktop_base() {
    crate::desktop::wallpaper::desktop_wallpaper_render();
    crate::desktop::icons::shell_draw_icons();
}

#[inline]
fn render_snap_preview() {
    let snap = unsafe { gui_compositor_get_snap_preview() };
    if snap == 0 {
        return;
    }

    let width = unsafe { gfx_get_width() };
    let height = unsafe { gfx_get_height() };
    let taskbar_height = unsafe { gui_compositor_get_taskbar_height() };
    let work_height = height.saturating_sub(taskbar_height);
    let half_w = width / 2;
    let half_h = work_height / 2;
    let color = 0x604080C0;

    unsafe {
        match snap {
            1 => gfx_fill_rect(0, 0, half_w, work_height, color),
            2 => gfx_fill_rect(half_w as i32, 0, width - half_w, work_height, color),
            3 => gfx_fill_rect(0, 0, width, half_h, color),
            4 => gfx_fill_rect(0, half_h as i32, width, work_height - half_h, color),
            5 => gfx_fill_rect(0, 0, width, work_height, color),
            _ => {}
        }
    }
}

#[inline]
fn render_desktop_chrome() {
    render_snap_preview();
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

    let state_ptr = unsafe { state_ptr() };
    if state_ptr.is_null() { return -12; }
    if let Err(error) = unsafe { snapshot_scene(state_ptr, false) } {
        return error;
    }
    let state = unsafe { &mut *state_ptr };

    let full_clip = DirtyRect {
        x: 0,
        y: 0,
        w: fb.width(),
        h: fb.height(),
    };
    let have_cursor = have_cursor != 0;

    fb.clear_clip();
    render_desktop_base();

    if let Err(error) = render_windows(fb, full_clip, state) {
        fb.clear_clip();
        return error;
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

/// Drain the unbounded tile damage tracker, build one scene snapshot, and
/// compose every resulting rectangle. No caller stack array or dirty-count
/// limit is involved.
#[no_mangle]
pub extern "C" fn gui_rust_compositor_render_pending(
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

    let state_ptr = unsafe { state_ptr() };
    if state_ptr.is_null() { return -12; }
    if let Err(error) = unsafe { snapshot_scene(state_ptr, true) } {
        return error;
    }
    let state = unsafe { &mut *state_ptr };
    if dirty::take_dirty_rects_into(
        &mut state.dirty_rects,
        &mut state.dirty_previous_runs,
        &mut state.dirty_current_runs,
    ).is_err() {
        return -12;
    }
    if state.dirty_rects.is_empty() {
        return 0;
    }

    let have_cursor = have_cursor != 0;
    let mut flipped_pixels = 0u64;

    for index in 0..state.dirty_rects.len() {
        let source = *state.dirty_rects.get(index).unwrap();
        let Some(clip) = clip_to_screen(source, fb.width(), fb.height()) else {
            continue;
        };

        fb.set_clip(clip.x, clip.y, clip.w, clip.h);
        render_desktop_base();

        if let Err(error) = render_windows(fb, clip, state) {
            fb.clear_clip();
            return error;
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

/// The kernel tick only pumps input and updates desktop timers. Rust decides
/// whether the next frame is full or partial, drains damage and presents it.
#[no_mangle]
pub extern "C" fn gui_rust_compositor_tick(
    have_cursor: i32,
    cursor_x: i32,
    cursor_y: i32,
) -> i32 {
    if FULL_DIRTY.swap(false, Ordering::AcqRel) {
        dirty::gui_dirty_reset();
        let result = gui_rust_compositor_render_full(have_cursor, cursor_x, cursor_y);
        if result < 0 {
            FULL_DIRTY.store(true, Ordering::Release);
        }
        return result;
    }

    if dirty::gui_dirty_is_empty() != 0
        && unsafe { gui_compositor_has_content_damage() == 0 }
    {
        return 0;
    }

    let result = gui_rust_compositor_render_pending(have_cursor, cursor_x, cursor_y);
    if result < 0 {
        FULL_DIRTY.store(true, Ordering::Release);
    }
    result
}

/// Synchronous full redraw used by the legacy GUI_OP_RENDER entry point.
/// Any new full-redraw request arriving during rendering remains pending.
#[no_mangle]
pub extern "C" fn gui_rust_compositor_render_full_now(
    have_cursor: i32,
    cursor_x: i32,
    cursor_y: i32,
) -> i32 {
    let _ = FULL_DIRTY.swap(false, Ordering::AcqRel);
    dirty::gui_dirty_reset();
    let result = gui_rust_compositor_render_full(have_cursor, cursor_x, cursor_y);
    if result < 0 {
        FULL_DIRTY.store(true, Ordering::Release);
    }
    result
}

