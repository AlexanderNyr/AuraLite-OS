//! Small dynamically-growing vector backed by AuraLite's kernel heap.
//!
//! This crate is `no_std` and currently has no global Rust allocator.  KVec is
//! intentionally limited to `Copy` element types used by GUI SoA columns and
//! scratch buffers.  It owns its allocation and never allocates on `push`
//! after `try_reserve` has provided enough capacity.

use core::ffi::c_void;
use core::marker::PhantomData;
use core::mem::{align_of, size_of};
use core::ptr;
use core::slice;

extern "C" {
    fn kmalloc(size: u64) -> *mut c_void;
    fn kfree(ptr: *mut c_void);
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AllocError;

/// Kernel-heap vector for trivially-copyable data.
///
/// Current uses are primitive SoA columns (`u8`, `u32`, `i32`, `u64`, `usize`)
/// and small POD records.  The kernel heap guarantees at least 8-byte
/// alignment.  Types requiring stronger alignment are rejected by reserve.
pub struct KVec<T: Copy> {
    ptr: *mut T,
    len: usize,
    cap: usize,
    _marker: PhantomData<T>,
}

impl<T: Copy> KVec<T> {
    #[inline]
    pub const fn new() -> Self {
        Self {
            ptr: ptr::null_mut(),
            len: 0,
            cap: 0,
            _marker: PhantomData,
        }
    }

    #[inline]
    pub const fn len(&self) -> usize {
        self.len
    }

    #[inline]
    pub const fn capacity(&self) -> usize {
        self.cap
    }

    #[inline]
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }

    #[inline]
    pub fn clear(&mut self) {
        self.len = 0;
    }

    /// Ensure capacity for at least `additional` more elements.
    pub fn try_reserve(&mut self, additional: usize) -> Result<(), AllocError> {
        let required = self.len.checked_add(additional).ok_or(AllocError)?;
        if required <= self.cap {
            return Ok(());
        }
        self.try_reserve_exact(required)
    }

    /// Ensure capacity for at least `required` elements.
    pub fn try_reserve_exact(&mut self, required: usize) -> Result<(), AllocError> {
        if required <= self.cap {
            return Ok(());
        }
        if size_of::<T>() == 0 || align_of::<T>() > 8 {
            return Err(AllocError);
        }

        let mut new_cap = self.cap.max(8);
        while new_cap < required {
            new_cap = new_cap.checked_mul(2).ok_or(AllocError)?;
        }

        let bytes = new_cap.checked_mul(size_of::<T>()).ok_or(AllocError)?;
        if bytes == 0 || bytes > u64::MAX as usize || bytes > isize::MAX as usize {
            return Err(AllocError);
        }

        // TODO(kheap-alignment): confirm kmalloc returns at least 8-byte
        // aligned memory. GUI SoA element types are deliberately capped at
        // that alignment until the kernel allocator contract is documented.
        let old_bytes = self.len.checked_mul(size_of::<T>()).ok_or(AllocError)?;
        let new_ptr = unsafe { kmalloc(bytes as u64) as *mut T };
        if new_ptr.is_null() {
            return Err(AllocError);
        }

        // Use only the allocator APIs already required by this kernel. Growth
        // allocates first, copies initialized POD elements, then releases the
        // old block. On failure, the old allocation remains untouched.
        if !self.ptr.is_null() {
            if old_bytes != 0 {
                unsafe { ptr::copy_nonoverlapping(self.ptr, new_ptr, self.len) };
            }
            unsafe { kfree(self.ptr.cast::<c_void>()) };
        }

        self.ptr = new_ptr;
        self.cap = new_cap;
        Ok(())
    }

    #[inline]
    pub fn try_push(&mut self, value: T) -> Result<(), AllocError> {
        if self.len == self.cap {
            self.try_reserve(1)?;
        }
        self.push_reserved(value)
    }

    /// Append without allocating.  Used by the scalar C->Rust snapshot
    /// callback while gui_lock is held.
    #[inline]
    pub fn push_no_grow(&mut self, value: T) -> Result<(), AllocError> {
        if self.len >= self.cap {
            return Err(AllocError);
        }
        self.push_reserved(value)
    }

    #[inline]
    fn push_reserved(&mut self, value: T) -> Result<(), AllocError> {
        if self.ptr.is_null() || self.len >= self.cap {
            return Err(AllocError);
        }
        unsafe { self.ptr.add(self.len).write(value) };
        self.len += 1;
        Ok(())
    }

    #[inline]
    pub fn truncate(&mut self, new_len: usize) {
        if new_len < self.len {
            self.len = new_len;
        }
    }

    pub fn try_resize(&mut self, new_len: usize, value: T) -> Result<(), AllocError> {
        if new_len > self.len {
            self.try_reserve(new_len - self.len)?;
            while self.len < new_len {
                self.push_reserved(value)?;
            }
        } else {
            self.len = new_len;
        }
        Ok(())
    }

    #[inline]
    pub fn as_slice(&self) -> &[T] {
        if self.len == 0 {
            return &[];
        }
        // SAFETY: ptr owns cap initialized slots and len <= cap.
        unsafe { slice::from_raw_parts(self.ptr, self.len) }
    }

    #[inline]
    pub fn as_mut_slice(&mut self) -> &mut [T] {
        if self.len == 0 {
            return &mut [];
        }
        // SAFETY: ptr owns cap initialized slots and len <= cap.
        unsafe { slice::from_raw_parts_mut(self.ptr, self.len) }
    }

    #[inline]
    pub fn get(&self, index: usize) -> Option<&T> {
        if index >= self.len {
            None
        } else {
            // SAFETY: index is bounded by len.
            Some(unsafe { &*self.ptr.add(index) })
        }
    }

    #[inline]
    pub fn get_mut(&mut self, index: usize) -> Option<&mut T> {
        if index >= self.len {
            None
        } else {
            // SAFETY: index is bounded by len and &mut self is exclusive.
            Some(unsafe { &mut *self.ptr.add(index) })
        }
    }

    #[inline]
    pub fn swap(&mut self, a: usize, b: usize) {
        debug_assert!(a < self.len && b < self.len);
        unsafe { ptr::swap(self.ptr.add(a), self.ptr.add(b)) };
    }
}

impl<T: Copy> Default for KVec<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Copy> Drop for KVec<T> {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { kfree(self.ptr.cast::<c_void>()) };
        }
        self.ptr = ptr::null_mut();
        self.len = 0;
        self.cap = 0;
    }
}

// KVec is only moved between kernel contexts while protected by its owning
// subsystem lock.  T is Copy, and the allocation is ordinary kernel heap RAM.
unsafe impl<T: Copy + Send> Send for KVec<T> {}
