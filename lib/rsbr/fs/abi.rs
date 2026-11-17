//! Private C-layout structures shared with the kernel syscall ABI.

use core::mem::size_of;

/// The x86_64 syscall path accepts 255 path bytes plus the trailing NUL.
/// The current AArch64/RV64 bring-up path copies into a 100-byte buffer; use
/// 99 as the conservative buffer size so both ports can terminate the path.
pub(crate) const PATH_BUFFER_SIZE: usize = if cfg!(target_arch = "x86_64") {
    256
} else {
    99
};

/// AArch64/RV64 file read/write syscalls currently process at most 4096 bytes
/// per call.  x86_64 has an internal chunking loop and accepts larger slices.
pub(crate) const PORTABLE_IO_CHUNK: usize = 4096;
pub(crate) const X86_MAX_DIRENTS: usize = 256;
pub(crate) const PORTABLE_MAX_DIRENTS: usize = 32;

pub(crate) const VFS_TYPE_FILE: u32 = 1;
pub(crate) const VFS_TYPE_DIR: u32 = 2;
pub(crate) const VFS_TYPE_CHARDEV: u32 = 3;
pub(crate) const VFS_TYPE_SYMLINK: u32 = 4;
pub(crate) const VFS_TYPE_FIFO: u32 = 5;

/// x86_64 `struct vfs_stat` used by native stat/fstat/lstat syscalls.
#[cfg(target_arch = "x86_64")]
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct VfsStat {
    pub kind: u32,
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

#[cfg(target_arch = "x86_64")]
impl VfsStat {
    pub const fn zeroed() -> Self {
        Self {
            kind: 0,
            mode: 0,
            uid: 0,
            gid: 0,
            size: 0,
            inode: 0,
            nlink: 0,
            blocks: 0,
            mtime: 0,
            ctime: 0,
            atime: 0,
        }
    }
}

/// Shared bring-up stat structure used by the current AArch64/RV64 ports.
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct AuraStat {
    pub size: u64,
    pub is_dir: u32,
    pub reserved: u32,
}

#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
impl AuraStat {
    pub const fn zeroed() -> Self {
        Self {
            size: 0,
            is_dir: 0,
            reserved: 0,
        }
    }
}

/// The full x86_64 VFS directory-entry layout.
#[cfg(target_arch = "x86_64")]
#[repr(C)]
pub(crate) struct VfsDirentLayout {
    pub name: [u8; 256],
    pub kind: u32,
    pub size: u64,
    pub inode: u64,
}

/// Compact shared directory-entry structure used by AArch64/RV64 bring-up.
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct AuraDirent {
    pub name: [u8; 60],
    pub is_dir: u32,
}

#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
impl AuraDirent {
    pub const fn zeroed() -> Self {
        Self {
            name: [0; 60],
            is_dir: 0,
        }
    }
}

#[cfg(target_arch = "x86_64")]
const _: [(); 64] = [(); size_of::<VfsStat>()];
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
const _: [(); 16] = [(); size_of::<AuraStat>()];
#[cfg(target_arch = "x86_64")]
const _: [(); 280] = [(); size_of::<VfsDirentLayout>()];
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
const _: [(); 64] = [(); size_of::<AuraDirent>()];
