//! Directory enumeration.

use crate::common::error::{Error, Result};
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use crate::common::error::ENOTDIR;
use crate::syscall;

use super::abi::{VFS_TYPE_DIR, VFS_TYPE_FILE};
#[cfg(target_arch = "x86_64")]
use super::abi::X86_MAX_DIRENTS;
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use super::abi::PORTABLE_MAX_DIRENTS;
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use super::file::File;
#[cfg(target_arch = "x86_64")]
use super::path::with_c_path;
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use super::metadata::metadata;

#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use super::abi::AuraDirent;

/// Directory entry in the x86_64 `struct vfs_dirent` layout.
///
/// AArch64/RV64 entries are converted from their smaller 64-byte
/// `aura_dirent` structure; size and inode are unavailable on those ports.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DirEntry {
    pub name: [u8; 256],
    pub kind: u32,
    pub size: u64,
    pub inode: u64,
}

impl DirEntry {
    pub const fn empty() -> Self {
        Self {
            name: [0; 256],
            kind: 0,
            size: 0,
            inode: 0,
        }
    }

    /// Name bytes, excluding the first NUL (or all 256 bytes if unterminated).
    pub fn name_bytes(&self) -> &[u8] {
        let length = self
            .name
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(self.name.len());
        &self.name[..length]
    }

    /// UTF-8 view of the entry name. Returns `None` for non-UTF-8 names.
    pub fn name_str(&self) -> Option<&str> {
        core::str::from_utf8(self.name_bytes()).ok()
    }

    pub const fn is_dir(&self) -> bool {
        self.kind == VFS_TYPE_DIR
    }

    pub const fn is_file(&self) -> bool {
        self.kind == VFS_TYPE_FILE
    }
}

const _: [(); 280] = [(); core::mem::size_of::<DirEntry>()];

/// Read up to `entries.len()` immediate children of `path`.
///
/// On x86_64 the kernel implements this as a path-based snapshot, so one call
/// returns the first N entries (up to 256). On AArch64/RV64 the wrapper opens
/// the directory and reads entries by index (currently up to 32 entries).
pub fn read_dir(path: &str, entries: &mut [DirEntry]) -> Result<usize> {
    if entries.is_empty() {
        return Ok(0);
    }

    #[cfg(target_arch = "x86_64")]
    {
        let count = entries.len().min(X86_MAX_DIRENTS);
        with_c_path(path, |c_path| {
            let result = syscall::call3(
                syscall::SYS_LISTDIR,
                c_path as usize,
                entries.as_mut_ptr() as usize,
                count,
            )?;
            if result > count {
                return Err(Error::InvalidSyscallResult);
            }
            Ok(result)
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        read_dir_portable(path, entries)
    }
}

#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
fn read_dir_portable(path: &str, entries: &mut [DirEntry]) -> Result<usize> {
    let info = metadata(path)?;
    if !info.is_dir() {
        return Err(Error::Syscall(ENOTDIR));
    }

    let file = File::open(path)?;
    let limit = entries.len().min(PORTABLE_MAX_DIRENTS);
    let mut written = 0usize;

    while written < limit {
        let mut raw = AuraDirent::zeroed();
        let result = syscall::call3(
            syscall::SYS_READDIR_PORTABLE,
            file.as_raw_fd(),
            written,
            &mut raw as *mut AuraDirent as usize,
        )?;

        if result == 0 {
            break;
        }
        if result != 1 {
            return Err(Error::InvalidSyscallResult);
        }

        let mut entry = DirEntry::empty();
        entry.name[..raw.name.len()].copy_from_slice(&raw.name);
        entry.kind = if raw.is_dir != 0 {
            VFS_TYPE_DIR
        } else {
            VFS_TYPE_FILE
        };
        entries[written] = entry;
        written += 1;
    }

    Ok(written)
}
