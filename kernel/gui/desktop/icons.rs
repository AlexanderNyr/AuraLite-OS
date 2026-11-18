
// Desktop icon storage, rendering and interaction.

use core::cell::UnsafeCell;
use core::ffi::{c_char, CStr};
use core::sync::atomic::{AtomicBool, AtomicI32, Ordering};

use super::{
    draw_text_fit, fill_rect, gui_screen_text_width, mark_dirty, request_redraw, theme,
    SMALL_TEXT_SIZE,
};

pub const MAX_ICONS: usize = 32;

const ICON_LABEL_CAPACITY: usize = 128;
const ICON_LABEL_WIDTH_MULTIPLIER: u32 = 3;

#[derive(Copy, Clone)]
struct Icon {
    in_use: i32,
    x: i32,
    y: i32,
    label: [u8; ICON_LABEL_CAPACITY],
    owner_pid: i32,
    icon_id: i32,
}

impl Icon {
    const EMPTY: Self = Self {
        in_use: 0,
        x: 0,
        y: 0,
        label: [0; ICON_LABEL_CAPACITY],
        owner_pid: 0,
        icon_id: 0,
    };
}

struct IconStorage(UnsafeCell<[Icon; MAX_ICONS]>);
unsafe impl Sync for IconStorage {}

static ICONS: IconStorage = IconStorage(UnsafeCell::new([Icon::EMPTY; MAX_ICONS]));
static ICON_LOCK: AtomicBool = AtomicBool::new(false);
static ICON_SELECTED: AtomicI32 = AtomicI32::new(-1);

extern "C" {
    fn timer_get_ticks() -> u64;
    fn gui_is_double_click(now: u32, x: i32, y: i32, wid: i32, part: i32, button: u8) -> i32;
    fn gui_record_click(now: u32, x: i32, y: i32, wid: i32, part: i32, button: u8);
    fn gui_current_pid() -> u64;
}

#[inline]
fn lock() {
    while ICON_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

#[inline]
fn unlock() {
    ICON_LOCK.store(false, Ordering::Release);
}

#[inline]
fn icon_label_max_width(size: u32, padding: u32) -> u32 {
    size
        .saturating_mul(ICON_LABEL_WIDTH_MULTIPLIER)
        .saturating_add(padding.saturating_mul(2))
}

pub(crate) fn init() {
    lock();
    unsafe {
        *ICONS.0.get() = [Icon::EMPTY; MAX_ICONS];
    }
    ICON_SELECTED.store(-1, Ordering::Relaxed);
    unlock();
}

fn label_len(label: &[u8; ICON_LABEL_CAPACITY]) -> usize {
    label.iter().position(|byte| *byte == 0).unwrap_or(label.len())
}

fn label_text(label: &[u8; ICON_LABEL_CAPACITY]) -> &str {
    core::str::from_utf8(&label[..label_len(label)]).unwrap_or("")
}

fn dirty_icon(x: i32, y: i32) {
    let active = theme();
    let width = icon_label_max_width(active.icon_size, active.icon_pad);
    let offset = width.saturating_sub(active.icon_size) / 2;
    let left = x.saturating_sub(offset as i32);

    mark_dirty(
        left,
        y,
        width,
        active.icon_size.saturating_add(20),
    );
}

fn draw_icon_label(
    center_x: i32,
    y: i32,
    text: &str,
    max_width: u32,
    color: u32,
) {
    let mut buffer = [0u8; 256];
    let mut end = text.len().min(buffer.len() - 1);

    while !text.is_char_boundary(end) {
        end -= 1;
    }

    let mut width;
    let mut measured;

    loop {
        buffer[..end].copy_from_slice(&text.as_bytes()[..end]);
        buffer[end] = 0;

        width = 0;
        measured = unsafe {
            gui_screen_text_width(
                buffer.as_ptr().cast::<c_char>(),
                SMALL_TEXT_SIZE,
                &mut width,
            ) == 0
        };

        if !measured || width <= max_width || end == 0 {
            break;
        }

        end = text[..end]
            .char_indices()
            .last()
            .map(|(index, _)| index)
            .unwrap_or(0);
    }

    let visible_text = &text[..end];
    let centered_width = if measured { width } else { max_width };
    let centered_width = centered_width.min(i32::MAX as u32) as i32;
    let draw_x = center_x.saturating_sub(centered_width / 2);

    draw_text_fit(
        draw_x,
        y,
        visible_text,
        max_width,
        color,
        SMALL_TEXT_SIZE,
    );
}

#[no_mangle]
pub unsafe extern "C" fn gui_add_icon(
    x: i32,
    y: i32,
    label: *const c_char,
    icon_id: i32,
) -> i32 {
    if label.is_null() {
        return -1;
    }

    let source = CStr::from_ptr(label).to_bytes();
    let mut bytes = source.len().min(ICON_LABEL_CAPACITY - 1);

    while core::str::from_utf8(&source[..bytes]).is_err() && bytes != 0 {
        bytes -= 1;
    }

    let owner_pid = gui_current_pid() as u32 as i32;
    let mut result = -1;

    lock();

    let entries = &mut *ICONS.0.get();

    for (index, entry) in entries.iter_mut().enumerate() {
        if entry.in_use == 0 {
            *entry = Icon::EMPTY;
            entry.in_use = 1;
            entry.x = x;
            entry.y = y;
            entry.label[..bytes].copy_from_slice(&source[..bytes]);
            entry.label[bytes] = 0;
            entry.icon_id = icon_id;
            entry.owner_pid = owner_pid;

            result = index as i32;
            break;
        }
    }

    unlock();

    if result >= 0 {
        dirty_icon(x, y);
    }

    result
}

#[no_mangle]
pub extern "C" fn gui_remove_icon(icon_index: i32) -> i32 {
    if icon_index < 0 || icon_index as usize >= MAX_ICONS {
        return -1;
    }

    let index = icon_index as usize;

    lock();

    let old_position = unsafe {
        let entries = &mut *ICONS.0.get();

        if entries[index].in_use == 0 {
            None
        } else {
            let position = (entries[index].x, entries[index].y);
            entries[index] = Icon::EMPTY;
            Some(position)
        }
    };

    unlock();

    let Some((x, y)) = old_position else {
        return -1;
    };

    if ICON_SELECTED.load(Ordering::Relaxed) == icon_index {
        ICON_SELECTED.store(-1, Ordering::Relaxed);
    }

    dirty_icon(x, y);

    0
}

#[no_mangle]
pub extern "C" fn gui_icon_owned_by(icon_index: i32, owner_pid: u64) -> i32 {
    if icon_index < 0 || icon_index as usize >= MAX_ICONS {
        return 0;
    }

    lock();

    let owned = unsafe {
        let entry = &(*ICONS.0.get())[icon_index as usize];
        entry.in_use != 0 && entry.owner_pid as u32 as u64 == owner_pid
    };

    unlock();

    if owned {
        1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn gui_icon_count() -> i32 {
    lock();

    let count = unsafe {
        (*ICONS.0.get())
            .iter()
            .filter(|entry| entry.in_use != 0)
            .count()
    };

    unlock();

    count as i32
}

#[no_mangle]
pub extern "C" fn shell_cleanup_icons(owner_pid: u64) {
    if owner_pid == 0 {
        return;
    }

    let mut removed_selected = false;

    lock();

    unsafe {
        let entries = &mut *ICONS.0.get();

        for (index, entry) in entries.iter_mut().enumerate() {
            if entry.in_use != 0 && entry.owner_pid as u32 as u64 == owner_pid {
                if ICON_SELECTED.load(Ordering::Relaxed) == index as i32 {
                    removed_selected = true;
                }

                *entry = Icon::EMPTY;
            }
        }
    }

    unlock();

    if removed_selected {
        ICON_SELECTED.store(-1, Ordering::Relaxed);
    }
}

#[no_mangle]
pub extern "C" fn shell_draw_icons() {
    let active = theme();
    let selected = ICON_SELECTED.load(Ordering::Relaxed);
    let mut snapshot = [Icon::EMPTY; MAX_ICONS];

    lock();

    unsafe {
        snapshot.copy_from_slice(&*ICONS.0.get());
    }

    unlock();

    let size = active.icon_size;
    let label_width = icon_label_max_width(size, active.icon_pad);

    for (index, icon) in snapshot.iter().enumerate() {
        if icon.in_use == 0 {
            continue;
        }

        let x = icon.x.max(0) as u32;
        let y = icon.y.max(0) as u32;
        let is_selected = selected == index as i32;
        let base = if is_selected {
            active.icon_selected
        } else {
            0x0040_6080
        };

        fill_rect(x, y, size, size, base);

        let inset = size / 4;

        if size > inset.saturating_mul(2) {
            fill_rect(
                x + inset,
                y + inset,
                size - inset * 2,
                size - inset * 2,
                if is_selected {
                    0x00ff_ffff
                } else {
                    0x0080_b0e0
                },
            );
        }

        let center_x = (x as i32).saturating_add((size / 2) as i32);
        let label_y = (y as i32)
            .saturating_add(size as i32)
            .saturating_add(2);

        draw_icon_label(
            center_x,
            label_y,
            label_text(&icon.label),
            label_width,
            active.icon_text,
        );
    }
}

fn hit_icon(mx: i32, my: i32) -> i32 {
    let size = theme().icon_size as i32;
    let mut snapshot = [Icon::EMPTY; MAX_ICONS];

    lock();

    unsafe {
        snapshot.copy_from_slice(&*ICONS.0.get());
    }

    unlock();

    for (index, icon) in snapshot.iter().enumerate() {
        if icon.in_use != 0
            && mx >= icon.x
            && mx < icon.x + size
            && my >= icon.y
            && my < icon.y + size + 16
        {
            return index as i32;
        }
    }

    -1
}

#[no_mangle]
pub extern "C" fn shell_handle_icon_click(
    mx: i32,
    my: i32,
    left_pressed: i32,
) -> i32 {
    if left_pressed == 0 {
        return 0;
    }

    let index = hit_icon(mx, my);

    if index < 0 {
        ICON_SELECTED.store(-1, Ordering::Relaxed);
        return 0;
    }

    ICON_SELECTED.store(index, Ordering::Relaxed);
    request_redraw();

    let now = unsafe { timer_get_ticks() as u32 };
    let part = 1000 + index;

    let double_click =
        unsafe { gui_is_double_click(now, mx, my, -1, part, 1) != 0 };

    if double_click {
        let icon_id = {
            lock();

            let value = unsafe {
                (*ICONS.0.get())[index as usize].icon_id
            };

            unlock();

            value
        };

        super::apps::launch_by_icon(icon_id);

        unsafe {
            gui_record_click(0, -999, -999, -999, -1, 0);
        }
    } else {
        unsafe {
            gui_record_click(now, mx, my, -1, part, 1);
        }
    }

    1
}