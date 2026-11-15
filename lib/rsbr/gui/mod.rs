//! # GUI — AuraLite compositor interface
//!
//! Thin, zero-allocation wrappers over `SYS_GUI_CALL` (200),
//! `SYS_GUI_EVENT` (201), and `SYS_GUI_THEME` (202). This is the
//! Rust counterpart to the low-level part of `libauragui`; the C
//! widget toolkit (`ag_add_button`, `ag_view_run`, …) is deliberately
//! omitted — it is userland policy, not kernel ABI.
//!
//! ## Fast path
//!
//! Every `Window` method costs **one syscall**. For animation, draw
//! into a local `[u32]` buffer and call [`Window::blit`] once per
//! frame: one syscall, one kernel-side row-wise copy (SSE2 on x86_64).
//!
//! ## Threading
//!
//! [`Window`] is `!Send + !Sync`: a GUI window belongs to the thread
//! that created it. Use [`Window::wid`] if you need the integer id
//! from another thread, or [`Window::into_raw`] to drop the RAII
//! wrapper.
//!
//! ## Example
//!
//! ```ignore
//! use rsbr::gui::{Window, rgb, EVT_CLOSE_REQ, FLAG_HAS_TITLE, FLAG_HAS_CLOSE};
//!
//! let w = Window::create(100, 100, 480, 320, Some("Demo"),
//!                        FLAG_HAS_TITLE | FLAG_HAS_CLOSE)?;
//! w.show()?;
//! w.clear(rgb(24, 32, 64))?;
//! w.draw_text(20, 20, "Hello, AuraLite!", rgb(255, 255, 255))?;
//! w.invalidate()?;
//! while w.wait()?.kind != EVT_CLOSE_REQ {}
//! ```

pub mod abi;
mod window;
mod app;

pub use self::abi::*;
pub use self::window::Window;
pub use self::app::{App, Flow, run};

pub use crate::common::error::{Error, Result};

use crate::syscall;

// ===================================================================
// Syscall numbers
// ===================================================================

pub(crate) const SYS_GUI_CALL:  usize = 200;
pub(crate) const SYS_GUI_EVENT: usize = 201;
pub(crate) const SYS_GUI_THEME: usize = 202;

// ===================================================================
// Raw syscall helpers
// ===================================================================

#[inline(always)]
pub(crate) fn gui_call(
    op: u32,
    a2: usize, a3: usize, a4: usize, a5: usize,
) -> Result<usize> {
    syscall::call6(SYS_GUI_CALL, op as usize, a2, a3, a4, a5, 0)
}

#[inline(always)]
pub(crate) fn gui_event(
    wid: i32, ev: &mut Event, blocking: bool,
) -> Result<usize> {
    syscall::call6(
        SYS_GUI_EVENT,
        wid as usize,
        ev as *mut Event as usize,
        blocking as usize,
        0, 0, 0,
    )
}

#[inline(always)]
pub(crate) fn gui_theme_get(t: &mut Theme) -> Result<usize> {
    syscall::call6(SYS_GUI_THEME, 1, t as *mut Theme as usize, 0, 0, 0, 0)
}

#[inline(always)]
pub(crate) fn gui_theme_set(t: &Theme) -> Result<usize> {
    syscall::call6(SYS_GUI_THEME, 2, t as *const Theme as usize, 0, 0, 0, 0)
}

// ===================================================================
// Global queries / actions (not tied to a Window)
// ===================================================================

pub fn screen_size() -> Result<(u32, u32)> {
    let mut out = [0u32; 2];
    gui_call(OP_GET_SCREEN, out.as_mut_ptr() as usize, 0, 0, 0)?;
    Ok((out[0], out[1]))
}

/// Returns `Some((x, y))` if a mouse is attached, `None` otherwise.
/// Kernel convention: `gui_mouse_position` returns 1 when the driver
/// is up, 0 when there is no pointer. The syscall layer forwards it;
/// we accept either encoding.
pub fn mouse_position() -> Result<Option<(i32, i32)>> {
    let mut out = [0i32; 2];
    let r = gui_call(OP_GET_MOUSE, out.as_mut_ptr() as usize, 0, 0, 0)?;
    let present = r == 1 || (r == 0 && (out[0] != 0 || out[1] != 0));
    Ok(if present { Some((out[0], out[1])) } else { None })
}

pub fn focused_window() -> Result<i32> {
    Ok(gui_call(OP_GET_FOCUSED, 0, 0, 0, 0)? as i32)
}

pub fn top_window() -> Result<i32> {
    Ok(gui_call(OP_TOP_WINDOW, 0, 0, 0, 0)? as i32)
}

pub fn set_cursor(c: Cursor) -> Result<()> {
    gui_call(OP_SET_CURSOR, c as usize, 0, 0, 0)?;
    Ok(())
}

/// Forces the compositor to present a frame immediately. Mostly
/// useful after a burst of drawing primitives when you want the
/// result visible without waiting for the next tick.
pub fn render_now() -> Result<()> {
    gui_call(OP_RENDER, 0, 0, 0, 0)?;
    Ok(())
}

pub fn theme() -> Result<Theme> {
    let mut t = Theme::default();
    gui_theme_get(&mut t)?;
    Ok(t)
}

/// Applies a theme. Requires the caller to own at least one window
/// (kernel-side ACL: `gui_pid_has_windows`).
pub fn set_theme(t: &Theme) -> Result<()> {
    gui_theme_set(t)?;
    Ok(())
}

pub fn font_metrics() -> Result<FontMetrics> {
    let mut m = [0u32; 5];
    gui_call(OP_FONT_INFO, m.as_mut_ptr() as usize, 0, 0, 0)?;
    Ok(FontMetrics {
        width: m[0], height: m[1],
        ascent: m[2], descent: m[3], num_glyphs: m[4],
    })
}

pub fn notify(text: &str, color: Color, duration_ms: u32) -> Result<()> {
    with_cstr(text, |p| {
        gui_call(OP_NOTIFY, p as usize, color as usize, duration_ms as usize, 0)?;
        Ok(())
    })
}

pub fn add_icon(x: i32, y: i32, label: &str, icon_id: i32) -> Result<i32> {
    with_cstr(label, |p| {
        let r = gui_call(OP_ADD_ICON, pack2(x, y), p as usize, icon_id as usize, 0)?;
        Ok(r as i32)
    })
}

pub fn remove_icon(idx: i32) -> Result<()> {
    gui_call(OP_REMOVE_ICON, idx as usize, 0, 0, 0)?;
    Ok(())
}

// ===================================================================
// C-string helper
// ===================================================================

/// Copies `s` into a 256-byte stack buffer with a trailing NUL, then
/// calls `f` with a pointer to it. The kernel reads the string
/// synchronously (during the syscall), so the stack lifetime is
/// sufficient.
#[inline]
pub(crate) fn with_cstr<F, R>(s: &str, f: F) -> Result<R>
where
    F: FnOnce(*const u8) -> Result<R>,
{
    const MAX: usize = 256;
    let mut buf = [0u8; MAX];
    let n = s.len().min(MAX - 1);
    buf[..n].copy_from_slice(&s.as_bytes()[..n]);
    buf[n] = 0;
    f(buf.as_ptr())
}