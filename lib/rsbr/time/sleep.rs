//! Sleep primitives. POSIX `nanosleep` semantics: at least the requested
//! duration, interrupted by actionable signals.

use crate::common::error::{Error, Result, EINTR};
use crate::syscall;

use super::abi::{TimeSpec, NANOS_PER_SEC};

/// POSIX `nanosleep(req, &rem)`.
///
/// * `Ok(None)` — slept the full duration.
/// * `Ok(Some(rem))` — interrupted by an actionable signal (`-EINTR`);
///   `rem` is the remaining time to the deadline.
/// * `Err(e)` — real error (`-EINVAL` for an out-of-range `TimeSpec`,
///   `-EFAULT` for a bad pointer).
///
/// The kernel rounds *up* to whole ticks: any non-zero request sleeps for
/// at least one tick (10 ms at 100 Hz).
pub fn nanosleep(req: TimeSpec) -> Result<Option<TimeSpec>> {
    let mut rem = TimeSpec::default();
    match syscall::call2(
        syscall::SYS_NANOSLEEP,
        &req as *const TimeSpec as usize,
        &mut rem as *mut TimeSpec as usize,
    ) {
        Ok(_) => Ok(None),
        Err(Error::Syscall(EINTR)) => Ok(Some(rem)),
        Err(e) => Err(e),
    }
}

/// Sleep for `n` units, retrying on `EINTR`. `units_per_sec` is the
/// number of `n` units in a second (1_000 for ms, 1_000_000 for µs).
fn sleep_units(mut n: u64, units_per_sec: u64) -> Result<()> {
    let nanos_per_unit = (NANOS_PER_SEC as u64) / units_per_sec;
    while n > 0 {
        let req = TimeSpec {
            tv_sec:  (n / units_per_sec) as i64,
            tv_nsec: ((n % units_per_sec) * nanos_per_unit) as i64,
        };
        match nanosleep(req)? {
            None => return Ok(()),
            Some(rem) => {
                // Defensive: rem should never be negative, but cast-guard
                // so a bad kernel reply cannot produce a huge retry.
                let rem_n = (rem.tv_sec.max(0) as u64)
                    .saturating_mul(units_per_sec)
                    .saturating_add((rem.tv_nsec.max(0) as u64) / nanos_per_unit);
                if rem_n == 0 {
                    return Ok(());
                }
                n = rem_n;
            }
        }
    }
    Ok(())
}

/// Sleep for at least `ms` milliseconds, retrying on `EINTR`.
///
/// Sub-tick requests are rounded up by the kernel to one tick, so
/// `sleep_ms(1)` on a 100 Hz PIT sleeps for ~10 ms. If you need to
/// observe `EINTR` yourself, call [`nanosleep`] directly.
pub fn sleep_ms(ms: u64) -> Result<()> {
    sleep_units(ms, 1_000)
}

/// Sleep for at least `us` microseconds, retrying on `EINTR`.
/// Same round-up caveat as [`sleep_ms`].
pub fn usleep(us: u64) -> Result<()> {
    sleep_units(us, 1_000_000)
}