
#![no_std]

mod arch;
#[path = "common/mod.rs"]
pub mod common;
mod syscall;
pub mod io;
pub mod process;
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
    pub use crate::{eprint, eprintln, print, println};
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