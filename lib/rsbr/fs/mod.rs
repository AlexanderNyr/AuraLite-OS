//! Filesystem API for AuraLite user programs.
//!
//! The API is `no_std` and does not require an allocator.  The x86_64 kernel
//! exposes the full VFS syscall surface.  The current AArch64/RV64 bring-up
//! dispatchers expose a smaller file ABI; operations absent from those ports
//! return `Error::Syscall(ENOSYS)` rather than pretending to be implemented.

mod abi;
pub mod directory;
pub mod file;
pub mod flags;
pub use flags::*;
pub mod metadata;
pub mod path;

pub use directory::{read_dir, DirEntry};
pub use file::{File, SeekFrom};
pub use metadata::{metadata, symlink_metadata, FileType, Metadata};
pub use path::{create_dir, read_link, remove_dir, remove_file, rename, symlink, truncate};

use crate::common::error::{Error, Result, ENOSYS};

#[inline]
pub(crate) fn unsupported<T>() -> Result<T> {
    Err(Error::Syscall(ENOSYS))
}
