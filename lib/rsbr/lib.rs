#![no_std]

mod arch;
#[path = "common/mod.rs"]
pub mod common;
pub mod syscall;
pub mod io;
pub mod process;
pub mod time;
pub mod gui;
pub mod fs;
mod runtime;

pub use common::error::{Error, Result};
pub use io::{print, println};
pub use process::{exit, getpid};

/// Convenient imports for AuraLite applications.
pub mod prelude {
    pub use crate::io::{
        eprint, eprintln,
        read, write, write_all,
    };
    pub use crate::process::{exit, getpid};
    pub use crate::time::{
        clock_getres, clock_gettime, gettimeofday, time,
        nanosleep, sleep_ms, usleep,
        realtime,
        TimeSpec, TimeVal, TimeT,
        CLOCK_REALTIME, CLOCK_MONOTONIC,
        CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
    };
    pub use crate::{eprint, eprintln, print, println};
    pub use crate::gui::{
        Window, Event, Color, Cursor, Snap, Theme, FontMetrics,
        rgb,
        EVT_CLOSE_REQ, EVT_KEY_DOWN, EVT_MOUSE_DOWN, EVT_MOUSE_UP,
        FLAG_HAS_TITLE, FLAG_HAS_CLOSE, FLAG_RESIZABLE, FLAG_ALWAYS_TOP,
    };
    pub use crate::fs::{
           create_dir, metadata, read_dir, read_link, remove_dir, remove_file,
           rename, symlink, symlink_metadata, truncate, DirEntry, File,
           FileType, Metadata, SeekFrom,
       };
}

/// Writes formatted text to stdout.
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{
        let _ = $crate::io::print_fmt(core::format_args!($($arg)*));
    }};
}

/// Writes formatted text followed by a newline to stdout.
#[macro_export]
macro_rules! println {
    () => {{
        let _ = $crate::io::println("");
    }};
    ($($arg:tt)*) => {{
        let _ = $crate::io::println_fmt(core::format_args!($($arg)*));
    }};
}

/// Writes formatted text to stderr.
#[macro_export]
macro_rules! eprint {
    ($($arg:tt)*) => {{
        let _ = $crate::io::eprint_fmt(core::format_args!($($arg)*));
    }};
}

/// Writes formatted text followed by a newline to stderr.
#[macro_export]
macro_rules! eprintln {
    () => {{
        let _ = $crate::io::eprintln("");
    }};
    ($($arg:tt)*) => {{
        let _ = $crate::io::eprintln_fmt(core::format_args!($($arg)*));
    }};
}