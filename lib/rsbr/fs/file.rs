//! RAII file-descriptor wrapper.

use crate::common::error::{Error, Result};
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use crate::common::error::{EEXIST, ENOENT};
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use crate::common::error::ENOSYS;
use crate::{io, syscall};

use super::abi::PORTABLE_IO_CHUNK;
use super::flags::{O_CREAT, O_EXCL, O_RDONLY, O_RDWR, SEEK_CUR, SEEK_END, SEEK_SET};
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
use super::metadata;
use super::metadata::Metadata;
use super::path::with_c_path;

const INVALID_FD: usize = usize::MAX;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SeekFrom {
    Start(u64),
    Current(i64),
    End(i64),
}

/// An owned AuraLite file descriptor.
///
/// `File` closes its descriptor on drop.  On AArch64/RV64 the kernel's current
/// bring-up file layer has a small fixed descriptor table and does not enforce
/// read/write access modes; the x86_64 implementation uses the full VFS OFD
/// semantics.
pub struct File {
    fd: usize,
}

impl File {
    /// Open an existing path using the kernel's default read-only flags.
    pub fn open(path: &str) -> Result<Self> {
        with_c_path(path, |c_path| {
            syscall::call3(syscall::SYS_OPEN, c_path as usize, O_RDONLY as usize, 0)
                .map(|fd| Self { fd })
        })
    }

    /// Open a path with native x86_64 VFS flags.
    ///
    /// On AArch64/RV64 this returns ENOSYS because those bring-up ports only
    /// implement an O_CREAT-lite hint rather than the full O_* flag contract.
    pub fn open_with(path: &str, flags: u32, mode: u32) -> Result<Self> {
        #[cfg(target_arch = "x86_64")]
        {
            with_c_path(path, |c_path| {
                syscall::call3(
                    syscall::SYS_OPEN,
                    c_path as usize,
                    flags as usize,
                    mode as usize,
                )
                .map(|fd| Self { fd })
            })
        }

        #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
        {
            let _ = (path, flags, mode);
            Err(Error::Syscall(ENOSYS))
        }
    }

    /// Create a new file and open it for reading and writing.
    ///
    /// On x86_64 this uses O_CREAT|O_EXCL|O_RDWR.  The current AArch64/RV64
    /// ABI has no exclusive-create flag, so the wrapper checks for existence
    /// before issuing its O_CREAT-lite open.  That fallback is not atomic if a
    /// future port gains concurrent user processes.
    pub fn create_new(path: &str, mode: u32) -> Result<Self> {
        #[cfg(target_arch = "x86_64")]
        {
            Self::open_with(path, O_CREAT | O_EXCL | O_RDWR, mode)
        }

        #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
        {
            match metadata::metadata(path) {
                Ok(_) => return Err(Error::Syscall(EEXIST)),
                Err(Error::Syscall(code)) if code == ENOENT => {}
                Err(error) => return Err(error),
            }

            let _ = mode; // The bring-up ABI creates with the filesystem default mode.
            with_c_path(path, |c_path| {
                // In the current RV64/AArch64 ABI, bit 0 is O_CREAT-lite.
                syscall::call3(syscall::SYS_OPEN, c_path as usize, 1, 0)
                    .map(|fd| Self { fd })
            })
        }
    }

    /// Return the raw descriptor without transferring ownership.
    pub const fn as_raw_fd(&self) -> usize {
        self.fd
    }

    /// Transfer ownership of the descriptor to the caller.
    pub fn into_raw_fd(mut self) -> usize {
        let fd = self.fd;
        self.fd = INVALID_FD;
        fd
    }

    /// Construct an owned `File` from a descriptor.
    ///
    /// # Safety
    /// `fd` must be a currently open descriptor owned by the calling process,
    /// and no other `File` may close it while this value is alive.
    pub unsafe fn from_raw_fd(fd: usize) -> Self {
        Self { fd }
    }

    /// Read once into `buffer`.  AArch64/RV64 currently cap one transfer at
    /// 4096 bytes; use repeated calls to consume larger buffers.
    pub fn read(&mut self, buffer: &mut [u8]) -> Result<usize> {
        if buffer.is_empty() {
            return Ok(0);
        }

        let limit = if cfg!(target_arch = "x86_64") {
            buffer.len()
        } else {
            buffer.len().min(PORTABLE_IO_CHUNK)
        };
        let read = io::read(self.fd, &mut buffer[..limit])?;
        if read > limit {
            return Err(Error::InvalidSyscallResult);
        }
        Ok(read)
    }

    /// Write once from `buffer`.
    pub fn write(&mut self, buffer: &[u8]) -> Result<usize> {
        if buffer.is_empty() {
            return Ok(0);
        }

        let limit = if cfg!(target_arch = "x86_64") {
            buffer.len()
        } else {
            buffer.len().min(PORTABLE_IO_CHUNK)
        };
        let written = io::write(self.fd, &buffer[..limit])?;
        if written > limit {
            return Err(Error::InvalidSyscallResult);
        }
        Ok(written)
    }

    /// Write the entire buffer, retrying after partial writes.
    pub fn write_all(&mut self, mut buffer: &[u8]) -> Result<()> {
        while !buffer.is_empty() {
            let written = self.write(buffer)?;
            if written == 0 {
                return Err(Error::WriteZero);
            }
            if written > buffer.len() {
                return Err(Error::InvalidSyscallResult);
            }
            buffer = &buffer[written..];
        }
        Ok(())
    }

    /// Change the shared file offset and return the resulting position.
    pub fn seek(&mut self, from: SeekFrom) -> Result<u64> {
        let (offset, whence) = match from {
            SeekFrom::Start(offset) => {
                if offset > i64::MAX as u64 {
                    return Err(Error::Syscall(crate::common::error::EINVAL));
                }
                (offset as i64, SEEK_SET)
            }
            SeekFrom::Current(offset) => {
                if offset == i64::MIN {
                    return Err(Error::Syscall(crate::common::error::EINVAL));
                }
                (offset, SEEK_CUR)
            }
            SeekFrom::End(offset) => {
                if offset == i64::MIN {
                    return Err(Error::Syscall(crate::common::error::EINVAL));
                }
                (offset, SEEK_END)
            }
        };

        syscall::call3(
            syscall::SYS_LSEEK,
            self.fd,
            offset as usize,
            whence as usize,
        )
        .map(|position| position as u64)
    }

    /// Return the current file offset.
    pub fn position(&mut self) -> Result<u64> {
        self.seek(SeekFrom::Current(0))
    }

    /// Duplicate the descriptor, sharing its open-file-description offset.
    /// Supported by the current x86_64 VFS syscall layer only.
    pub fn try_clone(&self) -> Result<Self> {
        #[cfg(target_arch = "x86_64")]
        {
            syscall::call1(syscall::SYS_DUP, self.fd).map(|fd| Self { fd })
        }

        #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
        {
            Err(Error::Syscall(ENOSYS))
        }
    }

    /// Read at an explicit offset without changing the shared file offset.
    /// Supported by the current x86_64 VFS syscall layer only.
    pub fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<usize> {
        #[cfg(target_arch = "x86_64")]
        {
            if offset > i64::MAX as u64 {
                return Err(Error::Syscall(crate::common::error::EINVAL));
            }
            if buffer.is_empty() {
                return Ok(0);
            }
            syscall::call4(
                syscall::SYS_PREAD64,
                self.fd,
                buffer.as_mut_ptr() as usize,
                buffer.len(),
                offset as usize,
            )
            .and_then(|read| {
                if read > buffer.len() {
                    Err(Error::InvalidSyscallResult)
                } else {
                    Ok(read)
                }
            })
        }

        #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
        {
            let _ = (offset, buffer);
            Err(Error::Syscall(ENOSYS))
        }
    }

    /// Write at an explicit offset without changing the shared file offset.
    /// Supported by the current x86_64 VFS syscall layer only.
    pub fn write_at(&self, offset: u64, buffer: &[u8]) -> Result<usize> {
        #[cfg(target_arch = "x86_64")]
        {
            if offset > i64::MAX as u64 {
                return Err(Error::Syscall(crate::common::error::EINVAL));
            }
            if buffer.is_empty() {
                return Ok(0);
            }
            syscall::call4(
                syscall::SYS_PWRITE64,
                self.fd,
                buffer.as_ptr() as usize,
                buffer.len(),
                offset as usize,
            )
            .and_then(|written| {
                if written > buffer.len() {
                    Err(Error::InvalidSyscallResult)
                } else {
                    Ok(written)
                }
            })
        }

        #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
        {
            let _ = (offset, buffer);
            Err(Error::Syscall(ENOSYS))
        }
    }

    /// Get metadata from the open descriptor.  The current bring-up ports do
    /// not yet provide fstat, so this returns ENOSYS there.
    pub fn metadata(&self) -> Result<Metadata> {
        super::metadata::metadata_for_fd(self.fd)
    }

    /// Flush file data and filesystem metadata.  Supported by the current
    /// x86_64 VFS only.
    pub fn sync(&self) -> Result<()> {
        #[cfg(target_arch = "x86_64")]
        {
            syscall::call1(syscall::SYS_FSYNC, self.fd).map(|_| ())
        }

        #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
        {
            Err(Error::Syscall(ENOSYS))
        }
    }

    /// Close now and report any close error.  Drop becomes a no-op afterwards.
    pub fn close(mut self) -> Result<()> {
        let fd = self.fd;
        self.fd = INVALID_FD;
        if fd == INVALID_FD {
            return Ok(());
        }
        syscall::call1(syscall::SYS_CLOSE, fd).map(|_| ())
    }
}

impl Drop for File {
    fn drop(&mut self) {
        if self.fd != INVALID_FD {
            let _ = syscall::call1(syscall::SYS_CLOSE, self.fd);
            self.fd = INVALID_FD;
        }
    }
}

