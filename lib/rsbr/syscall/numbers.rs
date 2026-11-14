// AuraLite D4 syscall numbers. These values are shared by the supported ISAs.

pub(crate) const SYS_READ:  usize = 0;
pub(crate) const SYS_WRITE: usize = 1;
pub(crate) const SYS_BRK:   usize = 12;
pub(crate) const SYS_GETPID: usize = 39;
pub(crate) const SYS_EXIT:  usize = 60;

// POSIX clocks & timers.
pub(crate) const SYS_NANOSLEEP:     usize = 35;
pub(crate) const SYS_GETITIMER:     usize = 36;
pub(crate) const SYS_SETITIMER:     usize = 38;
pub(crate) const SYS_GETTIMEOFDAY:  usize = 96;
pub(crate) const SYS_CLOCK_GETTIME: usize = 228;
pub(crate) const SYS_CLOCK_GETRES:  usize = 229;
pub(crate) const SYS_TIME:          usize = 520;