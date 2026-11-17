//! AuraLite VFS open flags and seek constants.
//!
//! These values match `kernel/fs/vfs.h` on x86_64.  The current AArch64/RV64
//! bring-up `open` implementation only recognises bit 0 as its create hint;
//! use `File::open` / `File::create_new` there instead of passing this mask.

pub const O_RDONLY: u32 = 0x0000;
pub const O_WRONLY: u32 = 0x0001;
pub const O_RDWR: u32 = 0x0002;
pub const O_ACCMODE: u32 = 0x0003;
pub const O_CREAT: u32 = 0x0040;
pub const O_EXCL: u32 = 0x0080;
pub const O_TRUNC: u32 = 0x0200;
pub const O_APPEND: u32 = 0x0400;
pub const O_NONBLOCK: u32 = 0x0800;
pub const O_DIRECTORY: u32 = 0x10000;
pub const O_CLOEXEC: u32 = 0x80000;

pub const SEEK_SET: u32 = 0;
pub const SEEK_CUR: u32 = 1;
pub const SEEK_END: u32 = 2;
