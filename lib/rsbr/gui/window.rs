//! `Window` — RAII handle over a kernel compositor window.

use core::marker::PhantomData;

use super::{abi::*, gui_call, gui_event, with_cstr};
use crate::common::error::{Error, Result};

/// A live window owned by the current thread.
///
/// `Drop` destroys the window (kernel-side resources are freed and
/// any waiting clients are woken). The RAII contract is one `Window`
/// per kernel window; use [`Window::into_raw`] to hand the id to
/// another owner without destroying it.
///
/// `Window` is `!Send + !Sync`: a GUI window belongs to the thread
/// that created it.
pub struct Window {
    wid: i32,
    _marker: PhantomData<*mut ()>,
}

impl Window {
    // ---------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------

    /// Creates a window at `(x, y)` with outer size `(w, h)`.
    ///
    /// `title` is copied into a stack buffer and handed to the
    /// kernel, which copies it into its own table before the syscall
    /// returns. The window starts hidden; call [`show`](Self::show)
    /// to make it visible.
    pub fn create(
        x: i32, y: i32,
        w: u32, h: u32,
        title: Option<&str>,
        flags: u32,
    ) -> Result<Self> {
        let raw = match title {
            Some(t) => with_cstr(t, |p| {
                gui_call(OP_CREATE,
                    pack2(x, y),
                    pack2(w as i32, h as i32),
                    p as usize,
                    flags as usize)
            })?,
            None => gui_call(OP_CREATE,
                pack2(x, y),
                pack2(w as i32, h as i32),
                0,
                flags as usize)?,
        };
        if raw > i32::MAX as usize {
            return Err(Error::InvalidSyscallResult);
        }
        Ok(Self { wid: raw as i32, _marker: PhantomData })
    }

    /// Returns the kernel window id (integer handle).
    pub const fn wid(&self) -> i32 { self.wid }

    /// Destroys the wrapper without destroying the window. The
    /// caller becomes responsible for calling `OP_DESTROY`
    /// eventually (usually via `Window::from_raw` in another scope).
    pub fn into_raw(self) -> i32 {
        let wid = self.wid;
        core::mem::forget(self);
        wid
    }

    /// # Safety
    ///
    /// `wid` must be a live window owned by the current process, and
    /// no other `Window` or `from_raw`/`into_raw` pair may reference
    /// it.
    pub unsafe fn from_raw(wid: i32) -> Self {
        Self { wid, _marker: PhantomData }
    }

    // ---------------------------------------------------------------
    // Visibility
    // ---------------------------------------------------------------

    pub fn show(&self) -> Result<()> {
        gui_call(OP_SHOW, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn hide(&self) -> Result<()> {
        gui_call(OP_HIDE, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn focus(&self) -> Result<()> {
        gui_call(OP_FOCUS, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn lower(&self) -> Result<()> {
        gui_call(OP_LOWER, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn minimize(&self) -> Result<()> {
        gui_call(OP_MINIMIZE, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn maximize(&self) -> Result<()> {
        gui_call(OP_MAXIMIZE, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn restore(&self) -> Result<()> {
        gui_call(OP_RESTORE, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn snap(&self, s: Snap) -> Result<()> {
        gui_call(OP_SNAP, self.wid as usize, s as usize, 0, 0)?; Ok(())
    }

    // ---------------------------------------------------------------
    // Geometry
    // ---------------------------------------------------------------

    pub fn move_to(&self, x: i32, y: i32) -> Result<()> {
        gui_call(OP_MOVE, self.wid as usize, pack2(x, y), 0, 0)?; Ok(())
    }
    pub fn resize(&self, w: u32, h: u32) -> Result<()> {
        gui_call(OP_RESIZE, self.wid as usize,
                 pack2(w as i32, h as i32), 0, 0)?; Ok(())
    }
    pub fn size(&self) -> Result<(u32, u32)> {
        let mut out = [0u32; 2];
        gui_call(OP_GET_SIZE, self.wid as usize,
                 out.as_mut_ptr() as usize, 0, 0)?;
        Ok((out[0], out[1]))
    }
    pub fn pos(&self) -> Result<(i32, i32)> {
        let mut out = [0i32; 2];
        gui_call(OP_GET_POS, self.wid as usize,
                 out.as_mut_ptr() as usize, 0, 0)?;
        Ok((out[0], out[1]))
    }
    pub fn rect(&self) -> Result<(i32, i32, u32, u32)> {
        let mut out = [0i32; 4];
        gui_call(OP_GET_RECT, self.wid as usize,
                 out.as_mut_ptr() as usize, 0, 0)?;
        Ok((out[0], out[1], out[2] as u32, out[3] as u32))
    }

    // ---------------------------------------------------------------
    // Properties
    // ---------------------------------------------------------------

    pub fn set_title(&self, title: Option<&str>) -> Result<()> {
        match title {
            Some(t) => with_cstr(t, |p| {
                gui_call(OP_TITLE, self.wid as usize, p as usize, 0, 0)?;
                Ok(())
            }),
            None => {
                gui_call(OP_TITLE, self.wid as usize, 0, 0, 0)?;
                Ok(())
            }
        }
    }
    pub fn flags(&self) -> Result<u32> {
        Ok(gui_call(OP_GET_FLAGS, self.wid as usize, 0, 0, 0)? as u32)
    }
    pub fn set_flags(&self, flags: u32) -> Result<()> {
        gui_call(OP_SET_FLAGS, self.wid as usize, flags as usize, 0, 0)?; Ok(())
    }
    pub fn z(&self) -> Result<i32> {
        Ok(gui_call(OP_GET_Z, self.wid as usize, 0, 0, 0)? as i32)
    }
    pub fn set_parent(&self, parent: Option<&Window>) -> Result<()> {
        let p = parent.map_or(-1i64, |w| w.wid as i64);
        gui_call(OP_SET_PARENT, self.wid as usize, p as usize, 0, 0)?; Ok(())
    }
    pub fn set_hole(&self, l: i32, t: i32, r: i32, b: i32) -> Result<()> {
        gui_call(OP_SET_HOLE, self.wid as usize,
                 pack2(l, t), pack2(r, b), 0)?; Ok(())
    }

    // ---------------------------------------------------------------
    // Capture
    // ---------------------------------------------------------------

    /// Captures all mouse events to this window. Returns the id of
    /// the previously capturing window, or `-1`.
    pub fn set_capture(&self) -> Result<i32> {
        Ok(gui_call(OP_SET_CAPTURE, self.wid as usize, 0, 0, 0)? as i32)
    }
    pub fn get_capture() -> Result<i32> {
        Ok(gui_call(OP_GET_CAPTURE, 0, 0, 0, 0)? as i32)
    }

    // ---------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------

    pub fn clear(&self, color: Color) -> Result<()> {
        gui_call(OP_CLEAR, self.wid as usize, color as usize, 0, 0)?; Ok(())
    }
    pub fn frame_begin(&self) -> Result<()> {
        gui_call(OP_FRAME_BEGIN, self.wid as usize, 0, 0, 0)?;
        Ok(())
    }
    pub fn frame_end(&self) -> Result<()> {
        gui_call(OP_FRAME_END, self.wid as usize, 0, 0, 0)?;
        Ok(())
    }
    pub fn fill_rect(&self, x: i32, y: i32, w: u32, h: u32, color: Color) -> Result<()> {
        gui_call(OP_FILL_RECT, self.wid as usize,
                 pack2(x, y), pack2(w as i32, h as i32), color as usize)?; Ok(())
    }
    pub fn draw_rect(&self, x: i32, y: i32, w: u32, h: u32, color: Color) -> Result<()> {
        gui_call(OP_DRAW_RECT, self.wid as usize,
                 pack2(x, y), pack2(w as i32, h as i32), color as usize)?; Ok(())
    }
    pub fn draw_line(&self, x0: i32, y0: i32, x1: i32, y1: i32, color: Color) -> Result<()> {
        gui_call(OP_DRAW_LINE, self.wid as usize,
                 pack2(x0, y0), pack2(x1, y1), color as usize)?; Ok(())
    }
    pub fn draw_text(&self, x: i32, y: i32, text: &str, color: Color) -> Result<()> {
        with_cstr(text, |p| {
            gui_call(OP_DRAW_TEXT, self.wid as usize,
                     pack2(x, y), p as usize, color as usize)?;
            Ok(())
        })
    }
    pub fn draw_pixel(&self, x: i32, y: i32, color: Color) -> Result<()> {
        gui_call(OP_DRAW_PIXEL, self.wid as usize,
                 pack2(x, y), color as usize, 0)?; Ok(())
    }
    /// Reads a single pixel. The kernel returns `0xAARRGGBB`; the
    /// alpha byte is stripped.
    pub fn get_pixel(&self, x: i32, y: i32) -> Result<Color> {
        let r = gui_call(OP_GET_PIXEL, self.wid as usize,
                         pack2(x, y), 0, 0)?;
        Ok((r as u32) & 0x00FF_FFFF)
    }

    /// Copies a rectangle of pixels into the back buffer.
    ///
    /// `src` must contain at least `h` rows of `stride` pixels each;
    /// pass `stride = 0` for a tightly packed source.
    pub fn blit(&self, x: i32, y: i32, w: u32, h: u32,
                src: &[u32], stride: u32) -> Result<()> {
        self.blit_common(OP_BLIT, x, y, w, h, src, stride)
    }

    /// Like [`blit`](Self::blit), but each source pixel's top byte is
    /// its alpha (0 = transparent, 255 = opaque).
    pub fn blit_alpha(&self, x: i32, y: i32, w: u32, h: u32,
                      src: &[u32], stride: u32) -> Result<()> {
        self.blit_common(OP_BLIT_ALPHA, x, y, w, h, src, stride)
    }

    fn blit_common(&self, op: u32,
                   x: i32, y: i32, w: u32, h: u32,
                   src: &[u32], stride: u32) -> Result<()> {
        let stride = if stride == 0 { w } else { stride };
        let need = (h as usize).checked_mul(stride as usize)
            .ok_or(Error::InvalidSyscallResult)?;
        if src.len() < need {
            return Err(Error::InvalidSyscallResult);
        }
        #[repr(C)]
        struct Args {
            w:      u32,
            h:      u32,
            stride: u32,
            _pad:   u32,
            src:    u64,
        }
        let a = Args { w, h, stride, _pad: 0, src: src.as_ptr() as u64 };
        gui_call(op, self.wid as usize, pack2(x, y),
                 &a as *const Args as usize, 0)?;
        Ok(())
    }

    // ---------------------------------------------------------------
    // Invalidation
    // ---------------------------------------------------------------

    pub fn invalidate(&self) -> Result<()> {
        gui_call(OP_INVALIDATE, self.wid as usize, 0, 0, 0)?; Ok(())
    }
    pub fn invalidate_rect(&self, x: i32, y: i32, w: u32, h: u32) -> Result<()> {
        gui_call(OP_INVAL_RECT, self.wid as usize,
                 pack2(x, y), pack2(w as i32, h as i32), 0)?; Ok(())
    }

    // ---------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------

    /// Non-blocking event poll. Returns `Ok(None)` if the ring is empty.
    pub fn poll(&self) -> Result<Option<Event>> {
        let mut ev = Event::default();
        let r = gui_event(self.wid, &mut ev, false)?;
        Ok(if r == 0 { None } else { Some(ev) })
    }

    /// Blocking event wait. Blocks until an event arrives or the
    /// window is destroyed.
    pub fn wait(&self) -> Result<Event> {
        let mut ev = Event::default();
        gui_event(self.wid, &mut ev, true)?;
        Ok(ev)
    }

    // ---------------------------------------------------------------
    // Clipboard
    // ---------------------------------------------------------------

    pub fn set_clipboard(&self, text: &str) -> Result<()> {
        with_cstr(text, |p| {
            gui_call(OP_SET_CLIPBOARD, p as usize, 0, 0, 0)?;
            Ok(())
        })
    }
    pub fn get_clipboard(&self, buf: &mut [u8]) -> Result<()> {
        if buf.is_empty() { return Ok(()); }
        gui_call(OP_GET_CLIPBOARD, buf.as_mut_ptr() as usize,
                 buf.len(), 0, 0)?;
        Ok(())
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        let _ = gui_call(OP_DESTROY, self.wid as usize, 0, 0, 0);
    }
}