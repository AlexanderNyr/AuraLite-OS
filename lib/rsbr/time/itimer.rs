//! AuraLite interval timers.

use crate::common::error::Result;
use crate::syscall;

use super::abi::{ItimerState, ITIMER_REAL, ITIMER_PROF, ITIMER_VIRTUAL};

fn check_which(which: i32) -> bool {
    matches!(which, ITIMER_REAL | ITIMER_VIRTUAL | ITIMER_PROF)
}

/// `getitimer(which, &val)` — current tick counters for the calling task.
pub fn getitimer(which: i32) -> Result<ItimerState> {
    if !check_which(which) {
        return Err(crate::common::error::Error::Syscall(
            crate::common::error::EINVAL,
        ));
    }
    let mut val = ItimerState::default();
    syscall::call2(
        syscall::SYS_GETITIMER,
        which as usize,
        &mut val as *mut ItimerState as usize,
    )?;
    Ok(val)
}

/// `setitimer(which, &new, &old)` — install a timer, return the previous
/// state in `old`. A zero `interval_ticks` makes the timer one-shot; a
/// non-zero value re-arms it every `interval_ticks` ticks.
pub fn setitimer(which: i32, new: ItimerState) -> Result<ItimerState> {
    if !check_which(which) {
        return Err(crate::common::error::Error::Syscall(
            crate::common::error::EINVAL,
        ));
    }
    let mut old = ItimerState::default();
    syscall::call3(
        syscall::SYS_SETITIMER,
        which as usize,
        &new as *const ItimerState as usize,
        &mut old as *mut ItimerState as usize,
    )?;
    Ok(old)
}