// AuraLite D4 syscall numbers. These values are shared by the supported ISAs.

pub const SYS_READ:  usize = 0;
pub const SYS_WRITE: usize = 1;
pub const SYS_BRK:   usize = 12;
pub const SYS_GETPID: usize = 39;
pub const SYS_EXIT:  usize = 60;

// POSIX clocks & timers.
pub const SYS_NANOSLEEP:     usize = 35;
pub const SYS_GETITIMER:     usize = 36;
pub const SYS_SETITIMER:     usize = 38;
pub const SYS_GETTIMEOFDAY:  usize = 96;
pub const SYS_CLOCK_GETTIME: usize = 228;
pub const SYS_CLOCK_GETRES:  usize = 229;
pub const SYS_TIME:          usize = 520;

// Native x86_64 filesystem syscalls and shared bring-up filesystem ABI.
pub const SYS_OPEN: usize = 2;
pub const SYS_CLOSE: usize = 3;
pub const SYS_FSTAT: usize = 5;
pub const SYS_LSTAT: usize = 6;
pub const SYS_LSEEK: usize = 8;
pub const SYS_PREAD64: usize = 17;
pub const SYS_PWRITE64: usize = 18;
pub const SYS_DUP: usize = 32;
pub const SYS_FSYNC: usize = 74;
pub const SYS_READDIR_PORTABLE: usize = 78;
pub const SYS_LISTDIR: usize = 80;
pub const SYS_SYMLINK: usize = 88;
pub const SYS_READLINK: usize = 89;
pub const SYS_MKDIR: usize = 100;
pub const SYS_RMDIR: usize = 101;
pub const SYS_UNLINK: usize = 102;
pub const SYS_RENAME: usize = 103;
pub const SYS_TRUNCATE: usize = 104;
pub const SYS_STAT_X86_64: usize = 105;
pub const SYS_STAT_PORTABLE: usize = 4;

// Process and descriptor management.
pub const SYS_PIPE: usize = 22;
pub const SYS_DUP2: usize = 33;
pub const SYS_WAIT4: usize = 61;
pub const SYS_SPAWN: usize = 81;

// Working directory.
pub const SYS_GETCWD: usize = 540;
pub const SYS_CHDIR: usize = 541;
pub const SYS_FCNTL: usize = 72;

pub const SYS_FORK: usize = 57;
pub const SYS_EXECVE: usize = 59;