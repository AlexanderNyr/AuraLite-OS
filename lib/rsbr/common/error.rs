use core::fmt;

/// Errors returned by the RSBR safe API.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    /// AuraLite returned a negative errno value. The stored errno is positive.
    Syscall(i32),
    /// A write operation reported success but wrote zero bytes.
    WriteZero,
    /// A syscall returned a value that cannot be represented by the API.
    InvalidSyscallResult,
    /// A `core::fmt::Write` implementation failed independently of a syscall.
    Formatting,
}

impl Error {
    /// Returns the positive errno for syscall errors.
    pub const fn errno(self) -> Option<i32> {
        match self {
            Self::Syscall(code) => Some(code),
            _ => None,
        }
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
