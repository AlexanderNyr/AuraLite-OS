use core::fmt;

use crate::common::error::{Error, Result};
use crate::syscall;

pub const STDIN: usize = 0;
pub const STDOUT: usize = 1;
pub const STDERR: usize = 2;

/// Reads bytes from a file descriptor into `buffer`.
pub fn read(fd: usize, buffer: &mut [u8]) -> Result<usize> {
    if buffer.is_empty() {
        return Ok(0);
    }

    syscall::call3(
        syscall::SYS_READ,
        fd,
        buffer.as_mut_ptr() as usize,
        buffer.len(),
    )
}

/// Performs one write operation to a file descriptor.
pub fn write(fd: usize, buffer: &[u8]) -> Result<usize> {
    if buffer.is_empty() {
        return Ok(0);
    }

    syscall::call3(
        syscall::SYS_WRITE,
        fd,
        buffer.as_ptr() as usize,
        buffer.len(),
    )
}

/// Writes the complete buffer, retrying after partial writes.
pub fn write_all(fd: usize, mut buffer: &[u8]) -> Result<()> {
    while !buffer.is_empty() {
        let written = write(fd, buffer)?;

        if written == 0 {
            return Err(Error::WriteZero);
        }
        if written > buffer.len() {
            return Err(Error::InvalidSyscallResult);
        }

        buffer = &buffer[written..];
    }

    Ok(())
}

pub fn print(message: &str) -> Result<()> {
    write_all(STDOUT, message.as_bytes())
}

pub fn println(message: &str) -> Result<()> {
    print(message)?;
    write_all(STDOUT, b"\n")
}

pub fn eprint(message: &str) -> Result<()> {
    write_all(STDERR, message.as_bytes())
}

pub fn eprintln(message: &str) -> Result<()> {
    eprint(message)?;
    write_all(STDERR, b"\n")
}

struct FdWriter {
    fd: usize,
    error: Option<Error>,
}

impl fmt::Write for FdWriter {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        match write_all(self.fd, text.as_bytes()) {
            Ok(()) => Ok(()),
            Err(error) => {
                self.error = Some(error);
                Err(fmt::Error)
            }
        }
    }
}

/// Writes `core::fmt` arguments to the selected file descriptor.
pub fn write_fmt(fd: usize, arguments: fmt::Arguments<'_>) -> Result<()> {
    let mut writer = FdWriter { fd, error: None };
    let result = fmt::write(&mut writer, arguments);

    match (result, writer.error) {
        (_, Some(error)) => Err(error),
        (Ok(()), None) => Ok(()),
        (Err(_), None) => Err(Error::Formatting),
    }
}

pub fn print_fmt(arguments: fmt::Arguments<'_>) -> Result<()> {
    write_fmt(STDOUT, arguments)
}

pub fn println_fmt(arguments: fmt::Arguments<'_>) -> Result<()> {
    print_fmt(arguments)?;
    write_all(STDOUT, b"\n")
}

pub fn eprint_fmt(arguments: fmt::Arguments<'_>) -> Result<()> {
    write_fmt(STDERR, arguments)
}

pub fn eprintln_fmt(arguments: fmt::Arguments<'_>) -> Result<()> {
    eprint_fmt(arguments)?;
    write_all(STDERR, b"\n")
}
