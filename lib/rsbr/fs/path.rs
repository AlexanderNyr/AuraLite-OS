//! Path-based filesystem operations.

use crate::common::error::{Error, Result, EINVAL, ENAMETOOLONG};
#[cfg(target_arch = "x86_64")]
use crate::syscall;

use super::abi::PATH_BUFFER_SIZE;

/// Copy a Rust path into a bounded, NUL-terminated stack buffer for a syscall.
pub(crate) fn with_c_path<T>(path: &str, call: impl FnOnce(*const u8) -> Result<T>) -> Result<T> {
    let bytes = path.as_bytes();
    if bytes.is_empty() || bytes.contains(&0) {
        return Err(Error::Syscall(EINVAL));
    }
    if bytes.len() >= PATH_BUFFER_SIZE {
        return Err(Error::Syscall(ENAMETOOLONG));
    }

    let mut buffer = [0u8; PATH_BUFFER_SIZE];
    buffer[..bytes.len()].copy_from_slice(bytes);
    call(buffer.as_ptr())
}

#[cfg(target_arch = "x86_64")]
fn with_two_c_paths<T>(
    first: &str,
    second: &str,
    call: impl FnOnce(*const u8, *const u8) -> Result<T>,
) -> Result<T> {
    with_c_path(first, |first_ptr| {
        with_c_path(second, |second_ptr| call(first_ptr, second_ptr))
    })
}

/// Create a directory.
///
/// Available through the full native VFS syscall layer on x86_64. The current
/// AArch64/RV64 bring-up syscall ABI does not yet expose mkdir.
pub fn create_dir(path: &str, mode: u32) -> Result<()> {
    #[cfg(target_arch = "x86_64")]
    {
        with_c_path(path, |c_path| {
            syscall::call2(syscall::SYS_MKDIR, c_path as usize, mode as usize).map(|_| ())
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = (path, mode);
        super::unsupported()
    }
}

/// Remove an empty directory.
pub fn remove_dir(path: &str) -> Result<()> {
    #[cfg(target_arch = "x86_64")]
    {
        with_c_path(path, |c_path| {
            syscall::call1(syscall::SYS_RMDIR, c_path as usize).map(|_| ())
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = path;
        super::unsupported()
    }
}

/// Remove a file or symbolic link.
pub fn remove_file(path: &str) -> Result<()> {
    #[cfg(target_arch = "x86_64")]
    {
        with_c_path(path, |c_path| {
            syscall::call1(syscall::SYS_UNLINK, c_path as usize).map(|_| ())
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = path;
        super::unsupported()
    }
}

/// Rename a file or directory.
pub fn rename(from: &str, to: &str) -> Result<()> {
    #[cfg(target_arch = "x86_64")]
    {
        with_two_c_paths(from, to, |from_ptr, to_ptr| {
            syscall::call2(syscall::SYS_RENAME, from_ptr as usize, to_ptr as usize).map(|_| ())
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = (from, to);
        super::unsupported()
    }
}

/// Change the length of a file addressed by path.
pub fn truncate(path: &str, length: u64) -> Result<()> {
    #[cfg(target_arch = "x86_64")]
    {
        with_c_path(path, |c_path| {
            syscall::call2(syscall::SYS_TRUNCATE, c_path as usize, length as usize).map(|_| ())
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = (path, length);
        super::unsupported()
    }
}

/// Create a symbolic link.
pub fn symlink(target: &str, link_path: &str) -> Result<()> {
    #[cfg(target_arch = "x86_64")]
    {
        with_two_c_paths(target, link_path, |target_ptr, link_ptr| {
            syscall::call2(syscall::SYS_SYMLINK, target_ptr as usize, link_ptr as usize).map(|_| ())
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = (target, link_path);
        super::unsupported()
    }
}

/// Read the target of a symbolic link into `buffer` and return its byte length.
/// The returned target is not NUL-terminated by this wrapper.
pub fn read_link(path: &str, buffer: &mut [u8]) -> Result<usize> {
    #[cfg(target_arch = "x86_64")]
    {
        if buffer.is_empty() {
            return Err(Error::Syscall(EINVAL));
        }
        let length = buffer.len().min(256);
        with_c_path(path, |c_path| {
            let read = syscall::call3(
                syscall::SYS_READLINK,
                c_path as usize,
                buffer.as_mut_ptr() as usize,
                length,
            )?;
            if read > length {
                return Err(Error::InvalidSyscallResult);
            }
            Ok(read)
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = (path, buffer);
        super::unsupported()
    }
}
