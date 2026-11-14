use crate::common::error::{Error, Result};
use crate::syscall;

/// Returns the current process/thread identifier used by AuraLite's ABI.
pub fn getpid() -> Result<i32> {
    let value = syscall::call0(syscall::SYS_GETPID)?;

    if value > i32::MAX as usize {
        return Err(Error::InvalidSyscallResult);
    }

    Ok(value as i32)
}

/// Terminates the current process with `code`.
///
/// AuraLite's exit syscall should not return. The spin loop is a defensive
/// fallback in case the kernel unexpectedly returns an error.
pub fn exit(code: i32) -> ! {
    let _ = syscall::call1(syscall::SYS_EXIT, code as usize);

    loop {
        core::hint::spin_loop();
    }
}
