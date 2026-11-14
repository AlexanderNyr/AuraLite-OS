//! Wall-clock and monotonic reads. `time()` and `gettimeofday()` are
//! built on `clock_gettime(CLOCK_REALTIME)` in the kernel.

use crate::common::error::Result;
use crate::syscall;

use super::abi::{TimeSpec, TimeVal, TimeT, CLOCK_REALTIME};

/// `clock_gettime(clockid, &ts)`.
///
/// The kernel's `kernel_clock_gettime` is `void`, but the syscall wrapper
/// returns `0` on success and `-EFAULT`/`-EINVAL` on a bad pointer or
/// clock id. `CLOCK_REALTIME` returns wall time (epoch from CMOS RTC +
/// PIT ticks); everything else falls back to monotonic ticks.
pub fn clock_gettime(clock: i32) -> Result<TimeSpec> {
    let mut ts = TimeSpec::default();
    syscall::call2(
        syscall::SYS_CLOCK_GETTIME,
        clock as usize,
        &mut ts as *mut TimeSpec as usize,
    )?;
    Ok(ts)
}

/// `clock_getres(clockid, &ts)`.
///
/// AuraLite ignores `clockid` and always reports `1e9 / timer_hz`
/// (10 ms at the default 100 Hz PIT).
pub fn clock_getres(clock: i32) -> Result<TimeSpec> {
    let mut ts = TimeSpec::default();
    syscall::call2(
        syscall::SYS_CLOCK_GETRES,
        clock as usize,
        &mut ts as *mut TimeSpec as usize,
    )?;
    Ok(ts)
}

/// `gettimeofday(&tv, NULL)`.
///
/// AuraLite ignores `tz` (`kernel_gettimeofday: (void)tz`), so there is
/// no parameter for it.
pub fn gettimeofday() -> Result<TimeVal> {
    let mut tv = TimeVal::default();
    syscall::call2(
        syscall::SYS_GETTIMEOFDAY,
        &mut tv as *mut TimeVal as usize,
        0, // tz = NULL
    )?;
    Ok(tv)
}

/// `time(NULL)` — seconds since the Unix epoch.
///
/// AuraLite's number is 520 (Linux uses 201). The kernel falls back to
/// PIT-monotonic seconds if the CMOS RTC answered nonsense at boot, in
/// which case the returned value starts at 0 rather than the real epoch.
pub fn time() -> Result<TimeT> {
    let r = syscall::call1(syscall::SYS_TIME, 0)?;
    Ok(r as TimeT)
}

/// Shorthand for `clock_gettime(CLOCK_REALTIME)`.
pub fn realtime() -> Result<TimeSpec> {
    clock_gettime(CLOCK_REALTIME)
}