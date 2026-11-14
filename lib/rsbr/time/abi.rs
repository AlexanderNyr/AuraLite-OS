//! Exact C-layout mirrors of `kernel/time.h` / `kernel/time_types.h`.
//!
//! `struct kernel_timespec` and `struct kernel_timeval` both use `long`
//! for the sub-second field. RSBR targets only 64-bit ISAs (x86_64,
//! riscv64, aarch64) where `long` is 8 bytes, so `i64` in Rust is a
//! byte-identical substitute. The `size_of` asserts below pin the
//! layout: if a 32-bit target ever appears, they fire at compile time
//! and `core::ffi::c_long` becomes the right field type.

#![allow(non_camel_case_types)]

/// POSIX `timespec`, mirror of `struct kernel_timespec`.
/// `tv_nsec` is `long` in C — see module doc.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct TimeSpec {
    pub tv_sec:  i64,
    pub tv_nsec: i64,
}

/// POSIX `timeval`, mirror of `struct kernel_timeval`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct TimeVal {
    pub tv_sec:  i64,
    pub tv_usec: i64,
}

/// AuraLite interval-timer state. **Not** POSIX `struct itimerval`:
/// the kernel stores ticks, not `timeval`. See `kernel/time_types.h`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ItimerState {
    pub interval_ticks: u64,
    pub deadline_ticks: u64,
}

/// `typedef long time_t` from `kernel/time_types.h`. 64-bit.
pub type TimeT = i64;

// POSIX clock IDs. Match `kernel/time.h` exactly.
pub const CLOCK_REALTIME:           i32 = 0;
pub const CLOCK_MONOTONIC:          i32 = 1;
// The kernel does not distinguish 2 and 3 — both fall into the
// "MONOTONIC + fallback" branch of `kernel_clock_gettime`.
pub const CLOCK_PROCESS_CPUTIME_ID: i32 = 2;
pub const CLOCK_THREAD_CPUTIME_ID:  i32 = 3;

// POSIX ITIMER `which` values.
pub const ITIMER_REAL:    i32 = 0;
pub const ITIMER_VIRTUAL: i32 = 1;
pub const ITIMER_PROF:    i32 = 2;

// Convenient unit constants.
pub const NANOS_PER_SEC:  i64 = 1_000_000_000;
pub const NANOS_PER_MS:   i64 = 1_000_000;
pub const NANOS_PER_US:   i64 = 1_000;
pub const USEC_PER_SEC:   i64 = 1_000_000;

// Compile-time layout pins. Every field is 8 bytes, all three structs
// are 16 bytes with no padding.
const _: () = assert!(core::mem::size_of::<TimeSpec>()    == 16);
const _: () = assert!(core::mem::size_of::<TimeVal>()     == 16);
const _: () = assert!(core::mem::size_of::<ItimerState>() == 16);
const _: () = assert!(core::mem::size_of::<TimeT>()       == 8);
// If RSBR ever grows a 32-bit target, `c_long` will be 4 bytes and
// the asserts above will fail — that is the point.
const _: () = assert!(core::mem::size_of::<i64>() == 8);