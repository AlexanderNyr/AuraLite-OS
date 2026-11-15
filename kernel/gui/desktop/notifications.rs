//! Desktop notification queue and renderer.

use core::cell::UnsafeCell;
use core::ffi::{c_char, CStr};
use core::sync::atomic::{AtomicBool, Ordering};

use super::{center_text_y, draw_text_fit, fill_rect, mark_dirty, request_redraw, screen_height, screen_width, theme, TEXT_SIZE};

pub const MAX_NOTIFICATIONS: usize = 8;

#[derive(Copy, Clone)]
struct Notification {
    in_use: i32,
    text: [u8; 128],
    color: u32,
    start_tick: u32,
    duration_ms: u32,
}

impl Notification {
    const EMPTY: Self = Self {
        in_use: 0,
        text: [0; 128],
        color: 0,
        start_tick: 0,
        duration_ms: 0,
    };
}

struct NotificationStorage(UnsafeCell<[Notification; MAX_NOTIFICATIONS]>);
unsafe impl Sync for NotificationStorage {}

static NOTIFICATIONS: NotificationStorage =
    NotificationStorage(UnsafeCell::new([Notification::EMPTY; MAX_NOTIFICATIONS]));
static NOTIFICATION_LOCK: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn timer_get_ticks() -> u64;
    fn timer_get_frequency() -> u32;
}

#[inline]
fn lock() {
    while NOTIFICATION_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

#[inline]
fn unlock() {
    NOTIFICATION_LOCK.store(false, Ordering::Release);
}

pub(crate) fn init() {
    lock();
    unsafe { *NOTIFICATIONS.0.get() = [Notification::EMPTY; MAX_NOTIFICATIONS]; }
    unlock();
}

fn valid_text_len(bytes: &[u8], max: usize) -> usize {
    let mut length = bytes.len().min(max);
    while core::str::from_utf8(&bytes[..length]).is_err() && length != 0 {
        length -= 1;
    }
    length
}

pub(crate) fn notify_text(text: &str, color: u32, duration_ms: u32) -> i32 {
    let bytes = text.as_bytes();
    let length = valid_text_len(bytes, 127);
    let mut notification = Notification::EMPTY;
    notification.in_use = 1;
    notification.text[..length].copy_from_slice(&bytes[..length]);
    notification.text[length] = 0;
    notification.color = color;
    notification.start_tick = unsafe { timer_get_ticks() as u32 };
    notification.duration_ms = if duration_ms == 0 { 3000 } else { duration_ms };

    let mut selected = None;
    let mut oldest_tick = u32::MAX;
    lock();
    unsafe {
        let entries = &mut *NOTIFICATIONS.0.get();
        for (index, entry) in entries.iter().enumerate() {
            if entry.in_use == 0 {
                selected = Some(index);
                break;
            }
            if entry.start_tick < oldest_tick {
                oldest_tick = entry.start_tick;
                selected = Some(index);
            }
        }
        if let Some(index) = selected {
            entries[index] = notification;
        }
    }
    unlock();

    let slot = selected.map(|index| index as i32).unwrap_or(-1);
    if slot >= 0 {
        mark_notification_area_dirty();
    }
    slot
}

fn mark_notification_area_dirty() {
    let fw = screen_width();
    let fh = screen_height();
    let taskbar_height = theme().taskbar_h;
    mark_dirty(
        fw.saturating_sub(320) as i32,
        fh.saturating_sub(taskbar_height).saturating_sub(60) as i32,
        310,
        50,
    );
}

#[no_mangle]
pub unsafe extern "C" fn gui_notify(
    text: *const c_char,
    color: u32,
    duration_ms: u32,
) -> i32 {
    if text.is_null() {
        return -1;
    }
    let bytes = CStr::from_ptr(text).to_bytes();
    let length = valid_text_len(bytes, 127);
    let value = core::str::from_utf8(&bytes[..length]).unwrap_or("");
    notify_text(value, color, duration_ms)
}

#[no_mangle]
pub extern "C" fn shell_expire_notifications() {
    let mut frequency = unsafe { timer_get_frequency() };
    if frequency == 0 {
        frequency = 100;
    }
    let now = unsafe { timer_get_ticks() as u32 };
    let mut expired = false;

    lock();
    unsafe {
        for entry in (&mut *NOTIFICATIONS.0.get()).iter_mut() {
            if entry.in_use == 0 {
                continue;
            }
            let elapsed = now.wrapping_sub(entry.start_tick) as u64 * 1000 / frequency as u64;
            if elapsed >= entry.duration_ms as u64 {
                entry.in_use = 0;
                expired = true;
            }
        }
    }
    unlock();

    if expired {
        request_redraw();
    }
}

#[no_mangle]
pub extern "C" fn shell_draw_notifications() {
    let active = theme();
    let fw = screen_width();
    let fh = screen_height();
    let mut frequency = unsafe { timer_get_frequency() };
    if frequency == 0 {
        frequency = 100;
    }
    let now = unsafe { timer_get_ticks() as u32 };

    let mut snapshot = [Notification::EMPTY; MAX_NOTIFICATIONS];
    lock();
    unsafe { snapshot.copy_from_slice(&*NOTIFICATIONS.0.get()); }
    unlock();

    let width = 300u32;
    let height = 40u32;
    let x = fw.saturating_sub(width + 8);
    let mut visible_index = 0u32;

    for entry in snapshot.iter() {
        if entry.in_use == 0 {
            continue;
        }
        let elapsed = now.wrapping_sub(entry.start_tick) as u64 * 1000 / frequency as u64;
        if elapsed >= entry.duration_ms as u64 {
            continue;
        }

        let y = fh
            .saturating_sub(active.taskbar_h)
            .saturating_sub(height + 8)
            .saturating_sub(visible_index.saturating_mul(height + 6));

        fill_rect(x, y, width, height, active.notif_bg);
        fill_rect(x, y, 4, height, entry.color);
        let text_length = entry.text.iter().position(|byte| *byte == 0).unwrap_or(entry.text.len());
        if let Ok(text) = core::str::from_utf8(&entry.text[..text_length]) {
            draw_text_fit(
                x as i32 + 10,
                center_text_y(y, height, TEXT_SIZE),
                text,
                width - 20,
                active.notif_text,
                TEXT_SIZE,
            );
        }
        visible_index += 1;
    }
}
