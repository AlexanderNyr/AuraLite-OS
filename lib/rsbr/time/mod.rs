//! Time: clocks, timers, sleep.

pub mod abi;

mod clock;
mod itimer;
mod sleep;

pub use self::abi::{
    ItimerState, TimeSpec, TimeVal, TimeT,
    CLOCK_REALTIME, CLOCK_MONOTONIC,
    CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
    ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF,
    NANOS_PER_SEC, NANOS_PER_MS, NANOS_PER_US, USEC_PER_SEC,
};
pub use self::clock::{clock_getres, clock_gettime, gettimeofday, realtime, time};
pub use self::itimer::{getitimer, setitimer};
pub use self::sleep::{nanosleep, sleep_ms, usleep};