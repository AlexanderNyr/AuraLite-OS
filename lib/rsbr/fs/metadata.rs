//! File metadata and stat-family wrappers.

use crate::common::error::Result;
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use crate::common::error::{Error, ENOSYS};
use crate::syscall;

#[cfg(target_arch = "x86_64")]
use super::abi::{
    VFS_TYPE_CHARDEV, VFS_TYPE_DIR, VFS_TYPE_FIFO, VFS_TYPE_FILE, VFS_TYPE_SYMLINK,
};
use super::path::with_c_path;

#[cfg(target_arch = "x86_64")]
use super::abi::VfsStat;
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use super::abi::AuraStat;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FileType {
    Regular,
    Directory,
    CharacterDevice,
    Symlink,
    Fifo,
    Unknown(u32),
}

impl FileType {
    #[cfg(target_arch = "x86_64")]
    pub(crate) const fn from_vfs_type(value: u32) -> Self {
        match value {
            VFS_TYPE_FILE => Self::Regular,
            VFS_TYPE_DIR => Self::Directory,
            VFS_TYPE_CHARDEV => Self::CharacterDevice,
            VFS_TYPE_SYMLINK => Self::Symlink,
            VFS_TYPE_FIFO => Self::Fifo,
            other => Self::Unknown(other),
        }
    }

    pub const fn is_file(self) -> bool {
        matches!(self, Self::Regular)
    }

    pub const fn is_dir(self) -> bool {
        matches!(self, Self::Directory)
    }
}

/// Portable metadata value.  Fields unavailable on the bring-up ports are 0.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Metadata {
    pub file_type: FileType,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: u64,
    pub inode: u64,
    pub nlink: u32,
    pub blocks: u32,
    pub mtime: u64,
    pub ctime: u64,
    pub atime: u64,
}

impl Metadata {
    pub const fn is_file(&self) -> bool {
        self.file_type.is_file()
    }

    pub const fn is_dir(&self) -> bool {
        self.file_type.is_dir()
    }
}

/// Return metadata for a path.
///
/// x86_64 receives the full `vfs_stat` payload.  AArch64/RV64 currently expose
/// only file size and the directory bit, so the other fields are zeroed.
pub fn metadata(path: &str) -> Result<Metadata> {
    #[cfg(target_arch = "x86_64")]
    {
        let mut raw = VfsStat::zeroed();
        with_c_path(path, |c_path| {
            syscall::call2(
                syscall::SYS_STAT_X86_64,
                c_path as usize,
                &mut raw as *mut VfsStat as usize,
            )
            .map(|_| from_vfs_stat(raw))
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let mut raw = AuraStat::zeroed();
        with_c_path(path, |c_path| {
            syscall::call2(
                syscall::SYS_STAT_PORTABLE,
                c_path as usize,
                &mut raw as *mut AuraStat as usize,
            )
            .map(|_| Metadata {
                file_type: if raw.is_dir != 0 {
                    FileType::Directory
                } else {
                    FileType::Regular
                },
                mode: 0,
                uid: 0,
                gid: 0,
                size: raw.size,
                inode: 0,
                nlink: 0,
                blocks: 0,
                mtime: 0,
                ctime: 0,
                atime: 0,
            })
        })
    }
}

/// Return metadata without following the final symbolic link.
///
/// The current AArch64/RV64 file ABI has no lstat operation, so those ports
/// currently use the same behavior as `metadata`.
pub fn symlink_metadata(path: &str) -> Result<Metadata> {
    #[cfg(target_arch = "x86_64")]
    {
        let mut raw = VfsStat::zeroed();
        with_c_path(path, |c_path| {
            syscall::call2(
                syscall::SYS_LSTAT,
                c_path as usize,
                &mut raw as *mut VfsStat as usize,
            )
            .map(|_| from_vfs_stat(raw))
        })
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        metadata(path)
    }
}

pub(crate) fn metadata_for_fd(fd: usize) -> Result<Metadata> {
    #[cfg(target_arch = "x86_64")]
    {
        let mut raw = VfsStat::zeroed();
        syscall::call2(
            syscall::SYS_FSTAT,
            fd,
            &mut raw as *mut VfsStat as usize,
        )
        .map(|_| from_vfs_stat(raw))
    }

    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    {
        let _ = fd;
        Err(Error::Syscall(ENOSYS))
    }
}

#[cfg(target_arch = "x86_64")]
fn from_vfs_stat(raw: VfsStat) -> Metadata {
    Metadata {
        file_type: FileType::from_vfs_type(raw.kind),
        mode: raw.mode,
        uid: raw.uid,
        gid: raw.gid,
        size: raw.size,
        inode: raw.inode,
        nlink: raw.nlink,
        blocks: raw.blocks,
        mtime: raw.mtime,
        ctime: raw.ctime,
        atime: raw.atime,
    }
}

