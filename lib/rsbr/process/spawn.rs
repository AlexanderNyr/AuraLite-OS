use crate::common::error::{Error, Result};
use crate::syscall;

use super::exit;

/// Maximum number of arguments supported by `spawn` (including `argv[0]`).
pub const MAX_SPAWN_ARGS: usize = 16;

/// Maximum byte length of a single path or argument string (including trailing NUL).
pub const MAX_ARG_LEN: usize = 256;

/// Optional file descriptors to redirect or close in the child process before `execve`.
#[derive(Clone, Copy, Debug, Default)]
pub struct StdioRedirect {
    pub stdin_fd: Option<i32>,
    pub stdout_fd: Option<i32>,
    pub stderr_fd: Option<i32>,
    pub extra_close_fd: Option<i32>,
}

fn write_c_string(text: &str, out: &mut [u8]) -> Result<()> {
    // Reject embedded NUL bytes and strings that exceed the stack buffer.
    if text.as_bytes().contains(&0) || text.len() + 1 > out.len() {
        return Err(Error::Syscall(22)); // EINVAL
    }

    out[..text.len()].copy_from_slice(text.as_bytes());
    out[text.len()] = 0;
    Ok(())
}

fn redirect_and_close(src_fd: Option<i32>, dst_fd: i32) -> Result<()> {
    if let Some(fd) = src_fd {
        if fd != dst_fd {
            syscall::call2(syscall::SYS_DUP2, fd as usize, dst_fd as usize)?;
        }
    }
    Ok(())
}

fn close_if_distinct(fd: Option<i32>, keep_0: i32, keep_1: i32, keep_2: i32) {
    if let Some(f) = fd {
        if f != keep_0 && f != keep_1 && f != keep_2 {
            let _ = syscall::call1(syscall::SYS_CLOSE, f as usize);
        }
    }
}

/// Spawns a new background process executing `path` with the given `args`.
///
/// `args` contains the arguments *after* the executable name; `argv[0]` is
/// automatically populated with `path`.
pub fn spawn(path: &str, args: &[&str]) -> Result<i32> {
    spawn_with_stdio(path, args, StdioRedirect::default())
}

/// Spawns a new process with optional `stdin`, `stdout`, and `stderr` redirection.
///
/// Useful for terminal emulators or pipelines that capture child output.
pub fn spawn_with_stdio(
    path: &str,
    args: &[&str],
    stdio: StdioRedirect,
) -> Result<i32> {
    if args.len() + 1 > MAX_SPAWN_ARGS {
        return Err(Error::Syscall(22)); // EINVAL
    }

    // Prepare NUL-terminated strings on the stack prior to `fork()` so the
    // child process does minimal work before calling `execve()`.
    let mut path_buf = [0u8; MAX_ARG_LEN];
    write_c_string(path, &mut path_buf)?;

    let mut arg_storage = [[0u8; MAX_ARG_LEN]; MAX_SPAWN_ARGS];
    let mut arg_ptrs = [0usize; MAX_SPAWN_ARGS + 1];

    // Conventionally, argv[0] points to the executable path.
    arg_ptrs[0] = path_buf.as_ptr() as usize;

    for (index, &arg) in args.iter().enumerate() {
        write_c_string(arg, &mut arg_storage[index])?;
        arg_ptrs[index + 1] = arg_storage[index].as_ptr() as usize;
    }

    // Terminate the `argv` array with a NULL pointer as required by the C ABI.
    arg_ptrs[args.len() + 1] = 0;

    let envp: [*const u8; 1] = [core::ptr::null()];

    let pid = syscall::call0(syscall::SYS_FORK)?;

    if pid == 0 {
        // Close any unused pipe end in the child process first.
        if let Some(fd) = stdio.extra_close_fd {
            let _ = syscall::call1(syscall::SYS_CLOSE, fd as usize);
        }

        // Duplicate redirected descriptors onto standard streams (0, 1, 2).
        if redirect_and_close(stdio.stdin_fd, 0).is_err()
            || redirect_and_close(stdio.stdout_fd, 1).is_err()
            || redirect_and_close(stdio.stderr_fd, 2).is_err()
        {
            exit(126);
        }

        // Close original source FDs after all `dup2` calls succeed
        // (handles the case where stdout_fd and stderr_fd share the same pipe FD).
        close_if_distinct(stdio.stdin_fd, 0, 1, 2);
        close_if_distinct(stdio.stdout_fd, 0, 1, 2);
        close_if_distinct(stdio.stderr_fd, 0, 1, 2);

        // Replace the child process image with the target executable.
        let _ = syscall::call3(
            syscall::SYS_EXECVE,
            path_buf.as_ptr() as usize,
            arg_ptrs.as_ptr() as usize,
            envp.as_ptr() as usize,
        );

        // `execve` only returns on failure.
        exit(127);
    }

    if pid > i32::MAX as usize {
        return Err(Error::InvalidSyscallResult);
    }

    Ok(pid as i32)
}