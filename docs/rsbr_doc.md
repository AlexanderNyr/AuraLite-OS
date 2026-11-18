# RSBR — Rust Bridge for AuraLite-OS

> **RSBR** is the low-level Rust userspace library for interacting with AuraLite-OS. It provides a Rust-facing API over the operating system's system-call interface and keeps architecture-specific calling conventions behind a shared interface.

This document is the central RSBR reference. Each subsystem is documented in its own section so that new modules can be added here without splitting the documentation across multiple files.

---

## Contents

- [1. Overview](#1-overview)
- [2. Design goals](#2-design-goals)
- [3. Architecture](#3-architecture)
- [4. Crate layout](#4-crate-layout)
- [5. Common API conventions](#5-common-api-conventions)
- [6. Platform support](#6-platform-support)
- [7. Modules](#7-modules)
  - [7.1. `process`](#71-process)
  - [7.2. `io`](#72-io)
  - [7.3. `time`](#73-time)
  - [7.4. `gui`](#74-gui)
  - [7.5. `fs`](#75-fs)
- [8. Runtime](#8-runtime)
- [9. System-call layer](#9-system-call-layer)
- [10. Adding a new module](#10-adding-a-new-module)
- [11. Current limitations and development status](#11-current-limitations-and-development-status)

---

## 1. Overview

RSBR (Rust Bridge) is a `no_std` library intended for Rust programs running directly on AuraLite-OS. It exposes operating-system services without requiring a hosted Rust environment or a C standard library.

The library sits between an application and the kernel:

```text
Rust application
       |
       v
RSBR public API
       |
       v
Shared syscall interface
       |
       v
Architecture-specific syscall backend
       |
       v
AuraLite-OS kernel
```

Applications should normally use the public modules (`process`, `io`, `time`, `gui`, and `fs`) instead of issuing raw system calls. The lower-level syscall and architecture layers exist to implement those modules and should be treated as implementation details unless an application has a specific low-level requirement.

### Core properties

- **`no_std`:** RSBR is designed for a freestanding userspace environment.
- **No mandatory allocator:** the core APIs use caller-provided buffers and fixed-size data structures where practical.
- **Rust API over native syscalls:** safe wrappers hide register conventions, syscall dispatch, and common ABI details.
- **Architecture-aware:** the syscall backend selects the implementation for the target architecture.
- **Explicit limitations:** an operation that is not available on a target should report an unsupported-operation error rather than silently pretending to provide equivalent behavior.

---

## 2. Design goals

RSBR is intended to provide a small and predictable userspace foundation for native AuraLite applications.

The main design goals are:

1. **A consistent Rust interface.** Applications should not need to know which registers carry syscall arguments on a particular CPU architecture.
2. **A small runtime footprint.** The library should remain suitable for freestanding programs and resource-constrained systems.
3. **Clear ownership.** Resources such as file descriptors and GUI windows should have explicit lifetimes and deterministic cleanup where possible.
4. **ABI correctness.** Structures passed to the kernel must use the exact layout expected by the relevant syscall ABI.
5. **Honest portability.** A shared function name does not imply that every kernel port currently implements the same operation or semantics.
6. **Incremental growth.** New APIs should build on existing kernel interfaces rather than inventing unsupported system calls.

RSBR is not intended to be a replacement for a full POSIX libc. Its API reflects the current AuraLite-OS kernel contract, including native interfaces and platform-specific bring-up subsets.

---

## 3. Architecture

RSBR is divided into several layers.

### 3.1. Public subsystem APIs

The public modules provide the interfaces used by applications:

- `process` — process-related operations.
- `io` — basic file-descriptor input and output.
- `time` — clocks, sleeping, and interval timers.
- `gui` — native window and application/event-loop interfaces.
- `fs` — filesystem and file-descriptor operations.

Each subsystem should keep its public API independent from raw architecture-specific syscall details whenever the underlying kernel interfaces allow it.

### 3.2. Common support

`common` contains types and helpers shared across the library, including the common error representation. This is where APIs obtain the crate-wide `Result` and represent syscall failures.

### 3.3. System-call abstraction

`syscall` provides the common syscall entry points and error decoding. Public modules use this layer instead of embedding architecture-specific assembly in every operation.

### 3.4. Architecture backends

`arch` contains the syscall implementation for each supported architecture:

- `x86_64`
- `aarch64`
- `riscv64`

The backend is selected at compile time. It is responsible for placing syscall numbers and arguments in the correct registers, executing the architecture's syscall instruction, and returning the raw result to the shared syscall layer.

### 3.5. Runtime support

`runtime` provides the freestanding entry-point and compiler/runtime support required by standalone programs. It includes startup, panic handling, and memory intrinsics that a hosted environment would normally provide.

---

## 4. Crate layout

The main source tree is organized by responsibility:

```text
lib/rsbr/
├── lib.rs
├── arch/
│   ├── mod.rs
│   ├── x86_64.rs
│   ├── aarch64.rs
│   └── riscv64.rs
├── common/
│   ├── mod.rs
│   └── error.rs
├── syscall/
│   ├── mod.rs
│   └── numbers.rs
├── runtime/
│   ├── mod.rs
│   ├── entry.rs
│   ├── panic.rs
│   └── mem_intrinsics.rs
├── process/
│   └── mod.rs
│   └── spawn.rs
├── io/
│   └── mod.rs
├── time/
│   ├── mod.rs
│   ├── abi.rs
│   ├── clock.rs
│   ├── sleep.rs
│   └── itimer.rs
├── gui/
│   ├── mod.rs
│   ├── abi.rs
│   ├── app.rs
│   └── window.rs
└── fs/
    ├── mod.rs
    ├── abi.rs
    ├── file.rs
    ├── directory.rs
    ├── flags.rs
    ├── metadata.rs
    └── path.rs
```

The `fs/` tree describes the initial filesystem module layout. If a module is not yet present in a particular checkout, treat its section below as the documented implementation target rather than proof that it has already been merged.

---

## 5. Common API conventions

### 5.1. `no_std`

RSBR is designed to be used from a freestanding Rust program. It uses `core` and does not require `std`. APIs that accept or return variable-length data generally use caller-provided slices instead of allocating memory internally.

### 5.2. Results and errors

Fallible public operations use the shared `Result<T>` type. Syscall failures are represented by the common error type, with the originating errno retained for syscall errors.

The syscall layer decodes the conventional negative errno return range. Subsystem APIs should propagate errors instead of converting every failure into a generic error.

An operation unavailable on the current architecture should return `ENOSYS` (or the corresponding common syscall error). Callers can therefore distinguish an unsupported operation from an invalid path, a missing file, or a permission failure.

### 5.3. Ownership and cleanup

Resource-owning types should document who owns the underlying kernel resource and when it is released. Where a type implements RAII, dropping the value performs best-effort cleanup. An explicit close method may be provided when callers need to observe close errors.

### 5.4. ABI structures

Any structure passed directly to a syscall must match the kernel's layout exactly. Use `#[repr(C)]` for ABI structures and add compile-time size checks where the layout is fixed. Do not assume that two architectures share a structure merely because the conceptual data is the same.

### 5.5. String and buffer handling

Rust strings are not automatically C strings. Before passing a path to a syscall, the wrapper must validate it, ensure that it contains no embedded NUL byte, check the platform's maximum supported length, and provide a NUL-terminated buffer for the duration of the syscall.

---

## 6. Platform support

RSBR currently contains architecture backends for the following targets:

| Target | Backend | Notes |
|---|---|---|
| `x86_64` | `arch/x86_64.rs` | Native AuraLite syscall interface; the filesystem interface is the most complete on this target. |
| `aarch64` | `arch/aarch64.rs` | Uses the AArch64 syscall convention and the current portable kernel userspace ABI. |
| `riscv64` | `arch/riscv64.rs` | Uses the RISC-V syscall convention and the current portable kernel userspace ABI. |

The architecture backend being present means that RSBR has a syscall entry implementation for that target. It does **not** mean that every kernel subsystem exposes identical functionality on all three targets.

Each public module must document meaningful differences in behavior, data availability, limits, and unsupported operations. The filesystem module below is an example of this rule.

---

## 7. Modules

### 7.1. `process`

**Source:** `lib/rsbr/process/`

The `process` module provides process-lifecycle operations exposed by the AuraLite-OS kernel. It wraps process identification, termination, and child-process spawning with arguments and optional standard-stream redirection.

The module is allocator-free: path strings, argument strings, and C-compatible `argv`/`envp` pointer arrays are prepared in bounded stack buffers before issuing system calls.

#### 7.1.1. Design

Process creation uses the `fork` + `execve` execution model:

1. The parent validates `path` and `args`, ensures no embedded NUL bytes are present, and packs NUL-terminated C strings alongside a NULL-terminated `argv` pointer table on the stack.
2. `SYS_FORK` creates the child process with pre-populated argument buffers already in place, minimizing work inside the child before `execve`.
3. If `StdioRedirect` descriptors are supplied, the child duplicates them onto standard file descriptors (`0`, `1`, `2`) via `SYS_DUP2` and closes any redundant source descriptors before replacing its image via `SYS_EXECVE`.
4. If `execve` fails, the child terminates immediately with exit code `127` (`126` if descriptor redirection fails) rather than returning into parent application code.

#### 7.1.2. Source files

| File | Responsibility |
|---|---|
| `mod.rs` | Module declarations, public re-exports, `getpid`, and `exit`. |
| `spawn.rs` | `spawn`, `spawn_with_stdio`, `StdioRedirect`, stack buffer limits, and C-string packing. |

#### 7.1.3. Public API

**Process identity and termination**

| API | Description |
|---|---|
| `getpid()` | Query the current process identifier (`SYS_GETPID`). |
| `exit(code)` | Terminate the current process immediately with `code` (`SYS_EXIT`). Never returns. |

**Process spawning**

| API | Description |
|---|---|
| `spawn(path, args)` | Fork and execute `path` in a new background process with the given `args`. |
| `spawn_with_stdio(path, args, stdio)` | Fork and execute `path` with optional `stdin`, `stdout`, and `stderr` descriptor redirection. |
| `StdioRedirect` | Configuration struct specifying optional `stdin_fd`, `stdout_fd`, `stderr_fd`, and `extra_close_fd`. |
| `MAX_SPAWN_ARGS` | Maximum total argument count supported by `spawn` (`16`, including `argv[0]`). |
| `MAX_ARG_LEN` | Maximum byte length of a single path or argument string (`256`, including the trailing NUL). |

#### 7.1.4. Basic usage

Spawn a background GUI application with a file argument:

```rust
use rsbr::process;

fn open_in_editor(file_path: &str) -> rsbr::Result<i32> {
    // `argv[0]` is automatically populated with "/bin/gedit".
    let child_pid = process::spawn("/bin/gedit", &[file_path])?;
    Ok(child_pid)
}
```

Spawn a command and capture its `stdout` and `stderr` through a pipe:

```rust
use rsbr::process::{spawn_with_stdio, StdioRedirect};

fn run_captured(path: &str, args: &[&str], pipe_read: i32, pipe_write: i32) -> rsbr::Result<i32> {
    let stdio = StdioRedirect {
        stdin_fd: None,
        stdout_fd: Some(pipe_write),
        stderr_fd: Some(pipe_write),
        extra_close_fd: Some(pipe_read),
    };

    spawn_with_stdio(path, args, stdio)
}
```

#### 7.1.5. Limits and syscall mapping

- **`argv[0]` convention:** Callers pass only the arguments *after* the program name in `args`. `spawn` automatically sets `argv[0]` to `path`.
- **Bounded stack buffers:** Up to `MAX_SPAWN_ARGS` (`16`) total arguments (so up to `15` user arguments in `args`) and `MAX_ARG_LEN` (`256`) bytes per string are supported. Exceeding these bounds or passing embedded NUL bytes fails with `EINVAL` (`22`).
- **Empty environment:** `envp` is currently passed as a NULL-terminated empty list (`[NULL]`).
- **Child reaping:** Spawned processes run independently in the background. Callers that spawn short-lived or GUI processes should periodically reap exited children via `SYS_WAIT4` (`WNOHANG`) to avoid zombie entries in the kernel process table.

| Operation | Native syscall | Number |
|---|---|---:|
| Get process ID | `getpid` | `39` |
| Exit process | `exit` | `60` |
| Fork process | `fork` | `57` |
| Execute image | `execve` | `59` |
| Duplicate descriptor (stdio) | `dup2` | `33` |
| Close redundant descriptor | `close` | `3` |

**Status:** implemented and runtime-verified on `x86_64`; child-waiting helpers (`wait.rs`) planned next.


### 7.2. `io`

**Source:** `lib/rsbr/io/mod.rs`

The `io` module provides unbuffered, descriptor-based byte input and output alongside formatted printing helpers built on `core::fmt::Write`. It serves as the foundational byte-stream layer for standard streams (`stdin`, `stdout`, `stderr`) and higher-level subsystems.

The module is allocator-free: read and write operations operate directly on caller-provided slices (`&[u8]` and `&mut [u8]`), and formatted output streams chunks directly to the target file descriptor without intermediate heap buffers.

#### 7.2.1. Design

- **Non-owning descriptor model:** Functions in `io` accept raw descriptor numbers (`usize`) and do not take ownership of them or close them automatically. When RAII cleanup is desired, wrap descriptors in an owning type such as `rsbr::fs::File`.
- **Short-write resilience:** `write` maps directly to a single `SYS_WRITE` call and may perform a partial write. `write_all` loops until the entire slice is transmitted, returning `Error::WriteZero` if the kernel reports `0` bytes written or `Error::InvalidSyscallResult` if the returned byte count exceeds the input slice length.
- **Zero-length fast paths:** Passing an empty slice to `read` or `write` returns `Ok(0)` immediately without issuing a system call.
- **Error-preserving formatting:** `write_fmt` adapts any raw file descriptor to `core::fmt::Write` via an internal `FdWriter`. If an underlying `write_all` call fails during formatting, the original syscall `Error` is preserved and returned rather than being discarded as a generic formatting failure.

#### 7.2.2. Public API

**Standard stream constants**

| Constant | Value | Description |
|---|---:|---|
| `STDIN` | `0` | Standard input file descriptor. |
| `STDOUT` | `1` | Standard output file descriptor. |
| `STDERR` | `2` | Standard error file descriptor. |

**Raw descriptor I/O**

| API | Description |
|---|---|
| `read(fd, buffer)` | Read up to `buffer.len()` bytes from `fd` into `buffer` (`SYS_READ`). |
| `write(fd, buffer)` | Perform a single write of `buffer` to `fd` (`SYS_WRITE`), returning the number of bytes written. |
| `write_all(fd, buffer)` | Repeatedly call `write` until all bytes in `buffer` are written or an error occurs. |

**Standard output and formatting helpers**

| API | Description |
|---|---|
| `print(message)` | Write a `&str` to `STDOUT`. |
| `println(message)` | Write a `&str` followed by `"\n"` to `STDOUT`. |
| `eprint(message)` | Write a `&str` to `STDERR`. |
| `eprintln(message)` | Write a `&str` followed by `"\n"` to `STDERR`. |
| `write_fmt(fd, arguments)` | Write `core::fmt::Arguments` to an arbitrary file descriptor `fd`. |
| `print_fmt(arguments)` | Write `core::fmt::Arguments` to `STDOUT`. |
| `println_fmt(arguments)` | Write `core::fmt::Arguments` followed by `"\n"` to `STDOUT`. |
| `eprint_fmt(arguments)` | Write `core::fmt::Arguments` to `STDERR`. |
| `eprintln_fmt(arguments)` | Write `core::fmt::Arguments` followed by `"\n"` to `STDERR`. |

#### 7.2.3. Basic usage

Write formatted diagnostics and read from a descriptor:

```rust
use rsbr::io::{self, STDIN, STDERR};

fn echo_once() -> rsbr::Result<usize> {
    io::println("Enter input:")?;

    let mut buf = [0u8; 128];
    let count = io::read(STDIN, &mut buf)?;

    io::eprintln_fmt(format_args!("Read {} bytes from stdin", count))?;
    Ok(count)
}
```

#### 7.2.4. Limits and syscall mapping

- **Single-call transfer size:** On `aarch64` and `riscv64`, the current bring-up kernel ABI processes at most `4096` bytes per `read` or `write` syscall. Callers transferring larger buffers directly through `io::read` or `io::write` should loop until all expected bytes are processed.
- **Non-atomic `println` / `eprintln`:** Line-printing helpers issue two separate `write_all` calls (one for the message payload and one for `b"\n"`). Concurrent writers sharing a descriptor may interleave output between the message and the newline.

| Operation | Native syscall | Number |
|---|---|---:|
| Read from descriptor | `read` | `0` |
| Write to descriptor | `write` | `1` |

**Status:** implemented and runtime-verified across `x86_64`, `aarch64`, and `riscv64`.

### 7.3. `time`

**Source:** `lib/rsbr/time/`

The `time` module provides wall-clock and monotonic time queries, high-resolution sleeping, and per-task interval timers over AuraLite-OS's time syscalls.

The module is allocator-free and targets 64-bit architectures (`x86_64`, `aarch64`, `riscv64`), enforcing exact C ABI struct sizes at compile time.

#### 7.3.1. Design

- **Compile-time ABI layout pins:** `TimeSpec`, `TimeVal`, and `ItimerState` are `#[repr(C)]` structs using 64-bit fields (`i64`/`u64`) to match the 64-bit kernel `long` layout (`16` bytes each, zero padding), verified via compile-time `assert!(size_of::<...>() == ...)`.
- **Signal-aware vs. retry-loop sleeping:** `nanosleep` directly exposes POSIX `-EINTR` interruptions by returning `Ok(Some(rem))` with the remaining duration. Convenience wrappers `sleep_ms` and `usleep` automatically retry with the remaining interval until the full duration elapses.
- **Native tick-based interval timers:** Unlike POSIX `itimerval`, AuraLite's `ItimerState` stores raw kernel tick counters (`interval_ticks` and `deadline_ticks`). Setting `interval_ticks == 0` configures a one-shot timer; a non-zero value re-arms the timer periodically.
- **Client-side `which` validation:** `getitimer` and `setitimer` validate the timer selector (`ITIMER_REAL`, `ITIMER_VIRTUAL`, `ITIMER_PROF`) in userspace and return `Error::Syscall(EINVAL)` before issuing a syscall.

#### 7.3.2. Source files

| File | Responsibility |
|---|---|
| `mod.rs` | Module declarations and public re-exports. |
| `abi.rs` | `TimeSpec`, `TimeVal`, `ItimerState`, `TimeT`, clock/timer constants, unit multipliers (`NANOS_PER_*`, `USEC_PER_SEC`), and layout assertions. |
| `clock.rs` | `clock_gettime`, `clock_getres`, `gettimeofday`, `time`, and `realtime`. |
| `sleep.rs` | `nanosleep`, `sleep_ms`, and `usleep`. |
| `itimer.rs` | `getitimer` and `setitimer`. |

#### 7.3.3. Public API

**Clocks & wall time (`clock.rs`)**

| API | Description |
|---|---|
| `clock_gettime(clock)` | Read `TimeSpec` (`tv_sec`, `tv_nsec`) for `CLOCK_REALTIME` (`0`) or monotonic clocks (`1..=3`). |
| `realtime()` | Shorthand for `clock_gettime(CLOCK_REALTIME)`. |
| `clock_getres(clock)` | Query timer resolution as `TimeSpec` (`1e9 / timer_hz`, e.g., `10 ms` at `100 Hz`). |
| `gettimeofday()` | Read wall time as `TimeVal` (`tv_sec`, `tv_usec`); timezone argument is omitted (`NULL`). |
| `time()` | Read seconds since the Unix epoch as `TimeT` (`i64`). |

**Sleeping (`sleep.rs`)**

| API | Description |
|---|---|
| `nanosleep(req)` | Sleep for at least `req`; returns `Ok(None)` on completion or `Ok(Some(rem))` if interrupted by `EINTR`. |
| `sleep_ms(ms)` | Sleep for at least `ms` milliseconds, automatically retrying on `EINTR`. |
| `usleep(us)` | Sleep for at least `us` microseconds, automatically retrying on `EINTR`. |

**Interval timers (`itimer.rs`)**

| API | Description |
|---|---|
| `getitimer(which)` | Query current `ItimerState` (`interval_ticks`, `deadline_ticks`) for `ITIMER_REAL`, `ITIMER_VIRTUAL`, or `ITIMER_PROF`. |
| `setitimer(which, new)` | Install a new `ItimerState` and return the previous `ItimerState`. |

#### 7.3.4. Basic usage

Measure elapsed monotonic time around a millisecond sleep:

```rust
use rsbr::time::{self, clock_gettime, CLOCK_MONOTONIC};

fn measure_sleep() -> rsbr::Result<i64> {
    let start = clock_gettime(CLOCK_MONOTONIC)?;
    time::sleep_ms(50)?;
    let end = clock_gettime(CLOCK_MONOTONIC)?;

    let elapsed_ns = (end.tv_sec - start.tv_sec) * time::NANOS_PER_SEC
        + (end.tv_nsec - start.tv_nsec);
    Ok(elapsed_ns)
}
```

#### 7.3.5. Limits and syscall mapping

- **Tick round-up:** The kernel PIT scheduler rounds non-zero sleep requests *up* to whole ticks. At the default `100 Hz` timer frequency, sub-10 ms requests such as `sleep_ms(1)` or `usleep(500)` sleep for approximately `10 ms` (`1` tick).
- **Clock ID aliasing:** `CLOCK_REALTIME` (`0`) returns epoch time derived from the CMOS RTC plus PIT ticks (falling back to monotonic boot seconds if the RTC reading is invalid). `CLOCK_MONOTONIC` (`1`), `CLOCK_PROCESS_CPUTIME_ID` (`2`), and `CLOCK_THREAD_CPUTIME_ID` (`3`) all resolve to the same monotonic tick source in the kernel, and `clock_getres` ignores `clock`.
- **Non-Linux `SYS_TIME` number:** AuraLite exposes `time` at syscall number `520` (whereas Linux uses `201`).

| Operation | Native syscall | Number |
|---|---|---:|
| Read clock | `clock_gettime` | `228` |
| Query clock resolution | `clock_getres` | `229` |
| Sleep with remainder | `nanosleep` | `35` |
| Get time of day | `gettimeofday` | `96` |
| Get epoch seconds | `time` | `520` |
| Get interval timer | `getitimer` | `36` |
| Set interval timer | `setitimer` | `38` |

**Status:** implemented and runtime-verified on `x86_64`.


### 7.4. `gui`

**Source:** `lib/rsbr/gui/`

The `gui` module exposes AuraLite's native compositor and windowing system. It provides compile-time-verified ABI definitions, an RAII `Window` handle with 2D drawing primitives, global compositor queries, and a dirty-flag-driven application event loop (`App` / `Flow`).

The module is allocator-free: strings are copied into bounded 256-byte stack buffers (`with_cstr`) during syscalls, and bulk pixel blits read directly from caller-provided `&[u32]` slices.

#### 7.4.1. Design

- **RAII window ownership & thread affinity:** `Window` owns a compositor window ID (`wid`) and issues `OP_DESTROY` automatically on `Drop`. It is `!Send + !Sync` (`PhantomData<*mut ()>`) so a window handle cannot be moved across threads. Ownership can be transferred manually via `into_raw` and `unsafe { Window::from_raw(wid) }`.
- **Compile-time ABI assertions:** Wire structures in `abi.rs` use `#[repr(C)]` and compile-time `assert!(size_of::<...>() == ...)` checks (`Event` is padded to `32` bytes; `Theme` is `33 * 4` bytes) to catch kernel ABI drift at build time.
- **Explicit dirty-flag event loop:** `gui::run` drives an `App` implementation by separating state mutation (`update(&mut self, ev) -> Flow`) from rendering (`render(&self, w) -> Result<()>`). Frames are bracketed with `frame_begin` / `frame_end` / `invalidate` only on initial startup, `EVT_RESIZE`, or when `update` explicitly returns `Flow::Changed` or `Flow::Quit`.
- **Single-syscall drawing fast path:** Each primitive (`fill_rect`, `draw_text`, `draw_line`, etc.) maps to a single `SYS_GUI_CALL`, packing coordinate pairs into register-sized arguments via `pack2(lo, hi)`. For software-rendered buffers, `Window::blit` and `Window::blit_alpha` upload an entire rectangle in one syscall.

#### 7.4.2. Source files

| File | Responsibility |
|---|---|
| `mod.rs` | Syscall wrappers (`SYS_GUI_CALL`, `SYS_GUI_EVENT`, `SYS_GUI_THEME`), global compositor queries/actions, and `with_cstr`. |
| `abi.rs` | `Color`, `rgb`, `pack2`, op-codes (`OP_*`), window flags (`FLAG_*`), event constants (`EVT_*`, `KEY_*`, `MOD_*`), `Event`, `Cursor`, `Snap`, `FontMetrics`, and `Theme`. |
| `window.rs` | `Window` lifecycle, geometry, properties, pointer capture, 2D drawing, blitting, invalidation, event polling/waiting, and clipboard helpers. |
| `app.rs` | `App` trait, `Flow` control enum (`Ignore`, `Changed`, `Quit`), and the `run` event loop. |

#### 7.4.3. Public API

**Global compositor queries & actions (`gui::*`)**

| API | Description |
|---|---|
| `screen_size()` | Query desktop resolution `(width, height)` (`OP_GET_SCREEN`). |
| `mouse_position()` | Query global cursor coordinates `Option<(i32, i32)>` (`OP_GET_MOUSE`). |
| `focused_window()` / `top_window()` | Query the currently focused or top-most window ID (`OP_GET_FOCUSED`, `OP_TOP_WINDOW`). |
| `set_cursor(cursor)` | Change the active cursor shape (`Cursor::Arrow`, `IBeam`, `Hand`, etc.). |
| `render_now()` | Force the compositor to present a frame immediately (`OP_RENDER`). |
| `theme()` / `set_theme(theme)` | Read or apply the global `Theme` (`SYS_GUI_THEME`). `set_theme` requires the calling process to own at least one window. |
| `font_metrics()` | Query built-in bitmap font dimensions (`width`, `height`, `ascent`, `descent`, `num_glyphs`). |
| `notify(text, color, duration_ms)` | Display a desktop notification banner (`OP_NOTIFY`). |
| `add_icon(x, y, label, icon_id)` / `remove_icon(idx)` | Register or remove a desktop shortcut icon (`OP_ADD_ICON`, `OP_REMOVE_ICON`). |

**Window lifecycle, geometry & state (`Window`)**

| API | Description |
|---|---|
| `Window::create(x, y, w, h, title, flags)` | Create a hidden window with outer geometry and `FLAG_*` capabilities. |
| `show()` / `hide()` / `focus()` / `lower()` | Control window visibility and Z-order stacking. |
| `minimize()` / `maximize()` / `restore()` / `snap(s)` | Change window state or snap to a screen edge (`Snap::Left`, `Right`, `Top`, `Bottom`, `Maximized`). |
| `move_to(x, y)` / `resize(w, h)` | Update window position or outer size. |
| `size()` / `pos()` / `rect()` | Query current dimensions `(w, h)`, position `(x, y)`, or full rect `(x, y, w, h)`. |
| `set_title(title)` / `flags()` / `set_flags(flags)` / `z()` | Inspect or update window title, flags, and Z-index. |
| `set_parent(parent)` / `set_hole(l, t, r, b)` | Attach a child window to a parent or punch a transparent hit/render cutout. |
| `set_capture()` / `Window::get_capture()` | Capture all pointer events to this window or query the current capture owner. |
| `wid()` / `into_raw()` / `Window::from_raw(wid)` | Access the underlying integer ID or transfer RAII ownership manually. |

**Drawing, blitting & invalidation (`Window`)**

| API | Description |
|---|---|
| `frame_begin()` / `frame_end()` | Bracket batched drawing commands for flicker-free presentation. |
| `clear(color)` | Fill the entire window client buffer with `color` (`0x00RRGGBB`). |
| `fill_rect(x, y, w, h, color)` / `draw_rect(...)` | Draw a filled or outlined rectangle. |
| `draw_line(x0, y0, x1, y1, color)` / `draw_pixel(x, y, color)` | Draw a line segment or single pixel. |
| `draw_text(x, y, text, color)` | Render bitmap font text at `(x, y)`. |
| `get_pixel(x, y)` | Read a client pixel as `0x00RRGGBB` (alpha stripped). |
| `blit(x, y, w, h, src, stride)` / `blit_alpha(...)` | Copy a `u32` pixel buffer into the window (`stride = 0` means tightly packed). |
| `invalidate()` / `invalidate_rect(x, y, w, h)` | Mark the full window or a dirty sub-rectangle for compositor presentation. |

**Events, clipboard & application loop (`Window`, `App`, `Flow`)**

| API | Description |
|---|---|
| `Window::poll()` | Non-blocking event poll (`Ok(None)` when queue is empty). |
| `Window::wait()` | Blocking wait until an event arrives or the window is destroyed. |
| `set_clipboard(text)` / `get_clipboard(buf)` | Write or read the system-wide text clipboard. |
| `App::update(&mut self, ev)` | Handle one `Event` and return `Flow::Ignore`, `Flow::Changed`, or `Flow::Quit`. |
| `App::render(&self, w)` | Render the window state (enforced read-only `&self`). |
| `gui::run(app, window)` | Run the managed event/repaint loop until `Flow::Quit` or window destruction. |

#### 7.4.4. Basic usage

Managed event loop using `App` and `Flow`:

```rust
use rsbr::gui::{
    self, App, Event, Flow, Window, rgb,
    EVT_CLOSE_REQ, EVT_KEY_DOWN, FLAG_HAS_CLOSE, FLAG_HAS_TITLE,
};

struct DemoApp;

impl App for DemoApp {
    fn update(&mut self, ev: &Event) -> Flow {
        match ev.kind {
            EVT_CLOSE_REQ => Flow::Quit,
            EVT_KEY_DOWN if ev.key == 0x1B => Flow::Quit, // Esc
            _ => Flow::Ignore,
        }
    }

    fn render(&self, w: &Window) -> rsbr::Result<()> {
        w.clear(rgb(24, 32, 44))?;
        w.draw_text(20, 20, "Hello, AuraLite!", rgb(232, 232, 240))?;
        Ok(())
    }
}

fn run_demo() -> rsbr::Result<()> {
    let win = Window::create(
        100, 100, 480, 320,
        Some("Demo"),
        FLAG_HAS_TITLE | FLAG_HAS_CLOSE,
    )?;
    win.show()?;
    gui::run(DemoApp, win)
}
```

#### 7.4.5. Limits and syscall mapping

- **String truncation:** `with_cstr` copies at most `255` bytes plus a terminating NUL into a stack buffer; longer window titles, labels, notifications, or `draw_text` strings are silently truncated to 255 bytes.
- **Close-button semantics:** Clicking the window `[X]` decoration hides the window in the compositor and enqueues `EVT_CLOSE_REQ`; applications using `gui::run` must explicitly match `EVT_CLOSE_REQ => Flow::Quit` if they want the process to exit on close.
- **Theme ACL:** `gui::set_theme` fails unless the calling PID owns at least one active window (`gui_pid_has_windows`).

| Operation | Native syscall | Number |
|---|---|---:|
| Window & drawing commands (`OP_*`) | `gui_call` | `200` |
| Poll / wait window events | `gui_event` | `201` |
| Get / set compositor theme | `gui_theme` | `202` |

**Status:** implemented and runtime-verified on `x86_64`.

### 7.5. `fs`

**Source:** `lib/rsbr/fs/`

The `fs` module is the Rust-facing filesystem API for AuraLite applications. It wraps the kernel's file-descriptor and path-based filesystem interfaces and provides an owned `File` type, metadata structures, directory enumeration, and path operations.

The module is allocator-free: callers provide the buffers used for file contents and directory entries.

#### 7.5.1. Design

The module uses two groups of operations:

- **Descriptor-based operations:** open, close, read, write, seek, and descriptor metadata.
- **Path-based operations:** metadata lookup, directory creation/removal, unlink, rename, truncate, and symbolic-link operations where available.

`File` owns an open file descriptor. It closes that descriptor when dropped. The explicit `close(self)` method closes it immediately and returns any close error; after that, `Drop` does not close it again.

The module does not expose kernel-internal VFS objects such as `vnode`, `ofd`, or `fdtab`. Those remain kernel implementation details. RSBR interacts with the kernel through syscalls only.

#### 7.5.2. Source files

| File | Responsibility |
|---|---|
| `mod.rs` | Module declarations, public exports, and the common unsupported-operation helper. |
| `abi.rs` | Private ABI structures, path limits, directory limits, and VFS type constants. |
| `file.rs` | `File`, `SeekFrom`, descriptor ownership, and file I/O. |
| `directory.rs` | `DirEntry` and `read_dir`. |
| `flags.rs` | Open flags and seek constants. |
| `metadata.rs` | `Metadata`, `FileType`, `metadata`, and `symlink_metadata`. |
| `path.rs` | Path-based filesystem operations and bounded C-path conversion. |

#### 7.5.3. Public API

**File handling**

| API | Description |
|---|---|
| `File::open(path)` | Open an existing file using the default read-only mode. |
| `File::create_new(path, mode)` | Create a new file and open it for reading and writing. |
| `File::open_with(path, flags, mode)` | Open using native VFS flags. Currently available on `x86_64`. |
| `File::read(buffer)` | Read one chunk into a caller-provided buffer. |
| `File::write(buffer)` | Write one chunk from a caller-provided buffer. |
| `File::write_all(buffer)` | Continue writing until all bytes are written or an error occurs. |
| `File::seek(from)` | Change the shared file offset. |
| `File::position()` | Query the current file offset. |
| `File::read_at(offset, buffer)` | Read at an explicit offset without changing the shared offset. `x86_64` only. |
| `File::write_at(offset, buffer)` | Write at an explicit offset without changing the shared offset. `x86_64` only. |
| `File::try_clone()` | Duplicate the descriptor. The duplicate shares the underlying open-file-description offset. `x86_64` only. |
| `File::metadata()` | Query metadata through an open descriptor. `x86_64` only. |
| `File::sync()` | Request file and filesystem writeback. `x86_64` only. |
| `File::close()` | Close explicitly and return the close result. |
| `File::as_raw_fd()` | Borrow the raw descriptor number without transferring ownership. |
| `File::into_raw_fd()` | Transfer descriptor ownership to the caller. |
| `File::from_raw_fd(fd)` | Construct an owning wrapper from a raw descriptor. `unsafe`; the caller must uphold the ownership contract. |

**Directory and metadata operations**

| API | Description |
|---|---|
| `read_dir(path, entries)` | Read immediate directory entries into a caller-provided slice. |
| `metadata(path)` | Get metadata for a path. |
| `symlink_metadata(path)` | Get metadata without following the final symbolic link where supported. |
| `DirEntry::name_bytes()` | Get the entry name as bytes, excluding its first NUL byte. |
| `DirEntry::name_str()` | Interpret the entry name as UTF-8; returns `None` if it is not valid UTF-8. |
| `DirEntry::is_dir()` | Test whether the entry is a directory. |
| `DirEntry::is_file()` | Test whether the entry is a regular file. |

**Path operations**

| API | Description |
|---|---|
| `create_dir(path, mode)` | Create a directory. |
| `remove_dir(path)` | Remove an empty directory. |
| `remove_file(path)` | Remove a file or symbolic link. |
| `rename(from, to)` | Rename a file or directory. |
| `truncate(path, length)` | Change a file's length by path. |
| `symlink(target, link_path)` | Create a symbolic link. |
| `read_link(path, buffer)` | Read the link target into a caller-provided byte buffer. |

Path operations not supported by the current AArch64/RV64 userspace ABI return `ENOSYS` on those targets.

#### 7.5.4. Basic usage

Open and read a file:

```rust
use rsbr::fs::File;

fn read_header() -> rsbr::Result<usize> {
    let mut file = File::open("/etc/hostname")?;
    let mut buffer = [0u8; 64];
    let count = file.read(&mut buffer)?;

    // `file` is closed automatically when it goes out of scope.
    Ok(count)
}
```

Create a file and write all bytes:

```rust
use rsbr::fs::File;

fn write_message() -> rsbr::Result<()> {
    let mut file = File::create_new("/tmp/message.txt", 0o644)?;
    file.write_all(b"Hello from RSBR!\n")?;
    Ok(())
}
```

Seek within a file:

```rust
use rsbr::fs::{File, SeekFrom};

fn read_from_start() -> rsbr::Result<usize> {
    let mut file = File::open("/tmp/message.txt")?;
    file.seek(SeekFrom::Start(0))?;

    let mut buffer = [0u8; 32];
    file.read(&mut buffer)
}
```

Read directory entries without allocating:

```rust
use rsbr::fs::{read_dir, DirEntry};

fn list_root() -> rsbr::Result<usize> {
    let mut entries = [DirEntry::empty(); 16];
    let count = read_dir("/", &mut entries)?;

    for entry in &entries[..count] {
        if let Some(name) = entry.name_str() {
            // Process `name` here.
            let _ = name;
        }
    }

    Ok(count)
}
```

The examples use `rsbr::Result`, matching the crate's shared result type. They do not require heap allocation.

#### 7.5.5. Open flags

The `flags` module exposes the native VFS flag values:

| Flag | Value | Meaning |
|---|---:|---|
| `O_RDONLY` | `0x0000` | Read-only access. This is zero, not a bit to test. |
| `O_WRONLY` | `0x0001` | Write-only access. |
| `O_RDWR` | `0x0002` | Read/write access. |
| `O_ACCMODE` | `0x0003` | Access-mode mask. |
| `O_CREAT` | `0x0040` | Create if the path does not exist. |
| `O_EXCL` | `0x0080` | With `O_CREAT`, fail if the path already exists. |
| `O_TRUNC` | `0x0200` | Truncate an existing regular file when opened. |
| `O_APPEND` | `0x0400` | Append writes at the end of the file. |
| `O_NONBLOCK` | `0x0800` | Non-blocking I/O status flag. |
| `O_DIRECTORY` | `0x10000` | Require a directory. |
| `O_CLOEXEC` | `0x80000` | Set close-on-exec for the new descriptor. |

These values match the native VFS definitions in `kernel/fs/vfs.h`. The full flag contract is currently provided by the `x86_64` syscall implementation. The current AArch64/RV64 bring-up ABI does **not** interpret this mask as POSIX open flags; it recognises only a limited create hint.

`SEEK_SET`, `SEEK_CUR`, and `SEEK_END` are exposed for the corresponding seek origins. Application code should generally use the `SeekFrom` enum instead of passing raw values.

#### 7.5.6. Platform behavior and limits

The three supported targets currently expose different filesystem syscall surfaces.

| Capability | `x86_64` | `aarch64` | `riscv64` |
|---|---|---|---|
| Open/read/write/close | Yes | Yes | Yes |
| Seek | Yes | Yes | Yes |
| Full native `O_*` flags | Yes | No | No |
| Create-new with atomic exclusive flag | Yes, via `O_CREAT\|O_EXCL` | No; existence check followed by create | No; existence check followed by create |
| Path metadata | Full native `vfs_stat` payload | Size and directory flag only | Size and directory flag only |
| No-follow path metadata | Yes, via `lstat` | Not exposed; falls back to `metadata` | Not exposed; falls back to `metadata` |
| Metadata by open descriptor | Yes, via `fstat` | Not exposed | Not exposed |
| Directory enumeration | Path-based list operation | Descriptor/index-based operation | Descriptor/index-based operation |
| File duplication | Yes | Not exposed | Not exposed |
| Positional read/write | Yes | Not exposed | Not exposed |
| File sync | Yes | Not exposed | Not exposed |
| mkdir/rmdir/unlink/rename | Yes | Not exposed by current bring-up ABI | Not exposed by current bring-up ABI |
| Path truncate | Yes | Not exposed by current bring-up ABI | Not exposed by current bring-up ABI |
| Symbolic-link operations | Yes | Not exposed by current bring-up ABI | Not exposed by current bring-up ABI |

Additional limits of the current portable AArch64/RV64 filesystem ABI:

- The user file-descriptor table has eight slots, currently numbered 3 through 10.
- A single read or write syscall processes at most 4096 bytes. RSBR limits each individual operation accordingly; callers can repeat operations for larger transfers.
- Directory enumeration returns at most 32 entries per call.
- Portable directory names are limited to 59 bytes plus a terminating NUL in the current ABI.
- Portable metadata includes only the file size and a directory indicator. Other metadata fields are zero because the kernel ABI does not provide them there.
- Relative paths are resolved from the root by the current bring-up path, not from a per-process current working directory.
- The portable open implementation does not enforce the full read/write access-mode contract.
- Portable `create_new` checks whether the path exists before requesting creation. Because the check and creation are separate syscalls, this is not an atomic exclusive-create operation.

The `x86_64` directory path uses the native path-based list operation and can return up to 256 entries per call. It uses the AuraLite VFS directory-entry layout, not Linux `getdents64` records. Linux compatibility syscalls are a separate kernel interface and must not be confused with the native RSBR ABI.

#### 7.5.7. ABI and syscall mapping

The filesystem module keeps its ABI definitions private in `abi.rs`. The layouts are target-specific:

- On `x86_64`, metadata uses the native `vfs_stat` layout (64 bytes), and directory entries use the native VFS directory-entry layout (280 bytes).
- On `aarch64` and `riscv64`, metadata uses the compact `AuraStat` layout (16 bytes), and directory entries use the compact `AuraDirent` layout (64 bytes). RSBR converts these records into the common public `Metadata` and `DirEntry` types.

The current module uses these native syscall paths:

| Operation | `x86_64` native interface | AArch64/RV64 bring-up interface |
|---|---|---|
| Open | `open` (2) | `open` (2) |
| Close | `close` (3) | `close` (3) |
| Read / write | `read` (0) / `write` (1) | `read` (0) / `write` (1) |
| Seek | `lseek` (8) | `lseek` (8) |
| Path metadata | `stat` (105) | `stat` (4) |
| Descriptor metadata | `fstat` (5) | Not exposed |
| No-follow metadata | `lstat` (6) | Not exposed |
| Directory entries | `listdir` (80) | `readdir` (78) |
| Create directory | `mkdir` (100) | Not exposed |
| Remove directory | `rmdir` (101) | Not exposed |
| Remove file | `unlink` (102) | Not exposed |
| Rename | `rename` (103) | Not exposed |
| Path truncate | `truncate` (104) | Not exposed |
| Positional I/O | `pread64` (17) / `pwrite64` (18) | Not exposed |
| Duplicate descriptor | `dup` (32) | Not exposed |
| Synchronize file | `fsync` (74) | Not exposed |
| Create/read symbolic link | `symlink` (88) / `readlink` (89) | Not exposed |

The numbers above are the syscall numbers used by the current native AuraLite interfaces. They are not a promise that the same number has identical semantics in a Linux ABI or on another kernel personality.

#### 7.5.8. Operations intentionally not exposed yet

The current first cut does not provide wrappers for every operation present in the kernel's internal VFS. In particular:

- `ftruncate` is not exposed because the kernel defines a number for it but has no active dispatcher arm in the current `x86_64` syscall handler.
- Hard-link creation is not exposed because the native `SYS_LINK` path is not implemented in the current dispatcher.
- `chmod`, `chown`, `access`, `fcntl`, `ioctl`, `readv`, `writev`, pipes, and current-working-directory operations are not part of this first public filesystem API.
- A feature must not be added merely because a similarly named `vfs_*` function exists inside the kernel. RSBR needs a real user-accessible syscall with a verified argument and data-layout contract.

#### 7.5.9. Integration notes

To add the initial filesystem module to an RSBR checkout:

1. Place the source directory at `lib/rsbr/fs/`.
2. Declare it from `lib.rs` with `pub mod fs;`.
3. Export the intended public types and functions through the crate prelude without removing existing exports.
4. Add the target-appropriate syscall numbers to `syscall/numbers.rs`.
5. Add any missing errno constants to `common/error.rs`.
6. Build and test all three targets. A successful `x86_64` build does not validate the AArch64 or RISC-V ABI layouts.

This module's initial implementation should be treated as **under integration and validation** until it has been built against the current RSBR checkout and exercised against the matching kernel on all supported targets.

---

## 8. Runtime

**Source:** `lib/rsbr/runtime/`

The runtime provides the minimal support needed by freestanding Rust applications:

- `entry.rs` — process entry-point glue. The current entry path invokes the application's `main() -> i32` and exits through the process API.
- `panic.rs` — panic handling for a `no_std` executable.
- `mem_intrinsics.rs` — low-level memory routines expected by compiler-generated code in a freestanding environment.

The runtime is not a hosted Rust standard library. It supplies only the startup and low-level facilities required by the RSBR execution model.

---

## 9. System-call layer

**Source:** `lib/rsbr/syscall/`

The syscall layer centralizes the low-level system-call interface. It provides helpers for invoking syscalls with different argument counts and converts kernel return values into the common RSBR result model.

`numbers.rs` stores the syscall identifiers used by the library. Numbers must be checked against the actual kernel dispatcher for each target before they are added or changed.

Subsystem modules should prefer the shared syscall helpers over writing inline assembly. Architecture-specific assembly belongs in `arch/`, not in individual filesystem, time, or GUI functions.

A syscall should be added to the RSBR public API only after the following have been verified:

1. The kernel has a live dispatcher arm for the syscall.
2. The argument order and widths are known.
3. Every pointer argument has a documented input/output direction and structure layout.
4. The kernel's return and errno behavior is understood.
5. The implementation is available on the target(s) claimed by the wrapper.

---

## 10. Adding a new module

New RSBR modules should follow the same general pattern:

1. **Inspect the kernel ABI first.** Find the syscall dispatcher, syscall number, argument order, pointer-copy logic, and related ABI structures.
2. **Define private ABI types.** Keep kernel-facing structures separate from the public Rust types unless the public type is intentionally the ABI type.
3. **Implement the shared API.** Put architecture-specific branches behind `#[cfg(target_arch = ...)]` where the underlying syscall contracts differ.
4. **Report missing functionality honestly.** Return `ENOSYS` for operations that the current kernel port does not expose. Do not emulate semantics that cannot be preserved.
5. **Add compile-time layout checks.** Assert the sizes of fixed ABI structures and, where useful, their alignment.
6. **Document platform differences.** Include behavior, limits, and unavailable operations in this file.
7. **Validate every target.** Build all supported architectures and run the relevant kernel/userland tests.

A module should not expose kernel internals as if they were stable userspace interfaces. For example, the filesystem API wraps VFS syscalls; it does not expose the kernel's `vnode`, `ofd`, or `fdtab` structures.

---

## 11. Current limitations and development status

RSBR is being developed alongside AuraLite-OS, so its API and the kernel ABI evolve together. The documentation should distinguish between:

- **Implemented:** source code exists in RSBR and has a corresponding kernel interface.
- **Integrated:** the module is declared and exported from the active RSBR crate.
- **Build-verified:** the crate compiles for the target architecture.
- **Runtime-verified:** the operation has been exercised against the matching kernel.

These are separate milestones. A function existing in source code does not, by itself, prove that the corresponding syscall is active or that the ABI works on every architecture.

The filesystem module's first cut has a broader interface on `x86_64` and a deliberately smaller one on `aarch64` and `riscv64`. Its integration and cross-target validation should be completed before describing the full filesystem API as production-stable.

As RSBR grows, this document should remain the single entry point for the library. Add a dedicated subsection under [Modules](#7-modules) for every new public subsystem, and update the platform support and development-status tables whenever the kernel ABI changes.
