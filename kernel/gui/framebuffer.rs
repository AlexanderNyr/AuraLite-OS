//! Rust access layer for the system framebuffer.
//!
//! The actual screen back-buffer remains owned by drivers/framebuffer/graphics.c.
//! This module exposes its geometry and presentation operations without
//! allocating a second full-screen buffer.

extern "C" {
    fn gfx_get_back_buffer() -> *mut u32;
    fn gfx_get_pitch_bytes() -> u32;
    fn gfx_get_width() -> u32;
    fn gfx_get_height() -> u32;

    fn gfx_clip_set(x: i32, y: i32, width: u32, height: u32);
    fn gfx_clip_clear();
    fn gfx_flip();
    fn gfx_flip_rect(x: i32, y: i32, width: u32, height: u32);
    fn gfx_take_px_counter() -> u64;
}

/// A validated view of the existing system back-buffer.
///
/// `stride` is measured in u32 pixels, not bytes. The pointer is deliberately
/// kept raw: callers must not manufacture a slice that ignores the pitch or
/// outlives the framebuffer driver's allocation.
#[derive(Clone, Copy)]
pub(crate) struct SystemFramebuffer {
    pixels: *mut u32,
    width: u32,
    height: u32,
    stride: u32,
    pitch_bytes: u32,
}

#[allow(dead_code)]
impl SystemFramebuffer {
    /// Obtain the current framebuffer metadata. Returns `None` until the
    /// graphics backend has successfully initialized its back-buffer.
    #[inline]
    pub(crate) fn acquire() -> Option<Self> {
        unsafe {
            let pixels = gfx_get_back_buffer();
            let width = gfx_get_width();
            let height = gfx_get_height();
            let pitch_bytes = gfx_get_pitch_bytes();

            if pixels.is_null()
                || width == 0
                || height == 0
                || pitch_bytes == 0
                || (pitch_bytes & 3) != 0
            {
                return None;
            }

            let stride = pitch_bytes / core::mem::size_of::<u32>() as u32;
            if stride < width {
                return None;
            }

            Some(Self {
                pixels,
                width,
                height,
                stride,
                pitch_bytes,
            })
        }
    }

    #[inline]
    pub(crate) const fn width(self) -> u32 {
        self.width
    }

    #[inline]
    pub(crate) const fn height(self) -> u32 {
        self.height
    }

    #[inline]
    pub(crate) const fn stride_pixels(self) -> u32 {
        self.stride
    }

    #[inline]
    pub(crate) const fn pitch_bytes(self) -> u32 {
        self.pitch_bytes
    }

    /// Raw pointer to the backend-owned screen buffer. The pointer is valid
    /// only while the graphics backend remains initialized.
    #[inline]
    pub(crate) fn pixels_mut_ptr(self) -> *mut u32 {
        self.pixels
    }

    #[inline]
    pub(crate) fn is_valid(self) -> bool {
        !self.pixels.is_null()
            && self.width != 0
            && self.height != 0
            && self.stride >= self.width
    }

    /// Restrict subsequent graphics operations to a screen-space rectangle.
    /// The C backend clamps this rectangle to framebuffer bounds.
    #[inline]
    pub(crate) fn set_clip(self, x: i32, y: i32, width: u32, height: u32) {
        unsafe { gfx_clip_set(x, y, width, height) }
    }

    /// Restore the graphics backend's full-screen clip rectangle.
    #[inline]
    pub(crate) fn clear_clip(self) {
        unsafe { gfx_clip_clear() }
    }

    /// Present the complete system back-buffer, including the existing
    /// framebuffer and VirtIO presentation paths.
    #[inline]
    pub(crate) fn present(self) {
        unsafe { gfx_flip() }
    }

    /// Present one screen-space rectangle from the system back-buffer.
    #[inline]
    pub(crate) fn present_rect(self, x: i32, y: i32, width: u32, height: u32) {
        if width != 0 && height != 0 {
            unsafe { gfx_flip_rect(x, y, width, height) }
        }
    }

    /// Return and reset the graphics backend's pixel-write counter.
    #[inline]
    pub(crate) fn take_pixels_written(self) -> u64 {
        unsafe { gfx_take_px_counter() }
    }
}

#[allow(dead_code)]
#[inline]
pub(crate) fn framebuffer_available() -> bool {
    SystemFramebuffer::acquire().map_or(false, |fb| fb.is_valid())
}

