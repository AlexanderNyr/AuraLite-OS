use core::fmt;

/// Errors returned by the RSBR safe API.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    Syscall(i32),
    WriteZero,
    InvalidSyscallResult,
    Formatting,
}

/// Common errno values that cross the syscall boundary.
/// Kept as bare constants (not enum variants) so existing `match` arms
/// in `io`/`process` keep compiling unchanged.
pub const EINTR:  i32 = 4;
pub const EINVAL: i32 = 22;
pub const EFAULT: i32 = 14;
pub const ESRCH:  i32 = 3;
pub const EBADF:  i32 = 9;
pub const EPERM:  i32 = 1;
pub const ENOMEM: i32 = 12;
pub const ENOSYS: i32 = 38;

impl Error {
    /// Returns the positive errno for syscall errors.
    pub const fn errno(self) -> Option<i32> {
        match self {
            Self::Syscall(code) => Some(code),
            _ => None,
        }
    }

    /// True when a syscall was interrupted by an actionable signal
    /// (`-EINTR`). Used by `time::sleep_ms` to decide whether to retry.
    pub const fn is_eintr(self) -> bool {
        matches!(self, Self::Syscall(EINTR))
    }

    /// True when a syscall rejected an argument (`-EINVAL`).
    pub const fn is_einval(self) -> bool {
        matches!(self, Self::Syscall(EINVAL))
    }
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::Syscall(code) => {
                write!(formatter, "AuraLite syscall failed (errno {})", code)
            }
            Self::WriteZero => formatter.write_str("write returned zero bytes"),
            Self::InvalidSyscallResult => {
                formatter.write_str("invalid result returned by syscall")
            }
            Self::Formatting => formatter.write_str("formatting failed"),
        }
    }
}

pub type Result<T> = core::result::Result<T, Error>;