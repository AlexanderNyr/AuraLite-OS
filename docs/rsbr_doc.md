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

The `gui` module is RSBR's low-level Rust interface to the AuraLite-OS compositor. It exposes window creation and management, drawing into window client buffers, pointer and keyboard event delivery, desktop-level compositor queries, theme access, clipboard operations, and a small application event-loop abstraction.

The module is intentionally a thin wrapper over the native GUI syscalls. It does not provide a widget toolkit, layout engine, controls, or automatic hit-testing. Applications implement those policies themselves using `Window` drawing methods and the `Event` stream.

The public API is allocator-free. Text arguments are copied into bounded stack buffers for the duration of a syscall; pixel uploads use caller-owned slices; event and theme structures are fixed-size ABI types.

#### 7.4.1. Design and execution model

The GUI API is split into three levels:

1. **Compositor operations** — global queries and actions such as screen size, mouse position, active theme, cursor selection, notifications, and desktop icons.
2. **Window operations** — creation, lifetime, geometry, state, drawing, invalidation, pointer capture, event polling, and clipboard access through `Window`.
3. **Application loop** — the `App` trait and `Flow` enum separate event-driven state changes from rendering. `gui::run` owns the blocking event loop and decides when a frame is rendered.

Important design properties:

- **RAII ownership.** A `Window` owns one kernel window ID. Dropping it issues `OP_DESTROY`. The destructor ignores a destroy error because `Drop` cannot return one.
- **Thread affinity.** `Window` contains `PhantomData<*mut ()>` and is therefore neither `Send` nor `Sync`. A window handle must remain on the thread that created it unless ownership is deliberately transferred through the raw-handle API.
- **Explicit raw ownership transfer.** `into_raw()` prevents the wrapper's destructor from running and returns the ID. `unsafe Window::from_raw(wid)` recreates an owning wrapper. The caller must guarantee that exactly one live owner is responsible for destroying that window.
- **Thin syscall wrappers.** GUI calls are dispatched through `SYS_GUI_CALL` (200), event reads through `SYS_GUI_EVENT` (201), and theme reads/writes through `SYS_GUI_THEME` (202). The public methods do not implement a second userspace window manager.
- **Dirty-driven rendering.** `App::update` reports whether visible state changed. `gui::run` does not redraw after every event; it redraws after an initial frame, a resize, or a `Flow` value that reports a visual change.
- **No built-in widget/input layer.** The application is responsible for hit-testing, selection, hover state, scroll models, focus within controls, keyboard navigation, and rendering those controls.

#### 7.4.2. Source files

| File | Responsibility |
|---|---|
| `mod.rs` | Public module exports, syscall numbers and wrappers, global compositor functions, and bounded C-string conversion. |
| `abi.rs` | ABI constants and types: colors, operation codes, window flags, event kinds, key/modifier constants, `Event`, `Cursor`, `Snap`, `FontMetrics`, and `Theme`. |
| `window.rs` | `Window` ownership, lifecycle, geometry, state, capture, drawing, blitting, invalidation, event polling/waiting, and clipboard operations. |
| `app.rs` | `App`, `Flow`, `gui::run`, and the internal frame-rendering function. |

#### 7.4.3. ABI types and constants

##### Colors and packed arguments

`Color` is a `u32` containing a 24-bit RGB value in `0x00RRGGBB` format. The high byte is reserved by the drawing API.

```rust
pub type Color = u32;

pub const fn rgb(r: u8, g: u8, b: u8) -> Color {
    ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}
```

`pack2(lo, hi)` packs two signed 32-bit values into one register-sized argument: `lo` occupies the lower 32 bits and `hi` occupies the upper 32 bits. It is used by window syscalls to pass coordinate pairs and dimension pairs without allocating an argument structure.

Predefined colors are `BLACK`, `WHITE`, `RED`, `GREEN`, `BLUE`, `YELLOW`, `GRAY`, `DARK`, and `LIGHT`.

##### `Event`

`Event` is the Rust representation passed to `SYS_GUI_EVENT`. Its currently declared fields are:

| Field | Rust type | Current role in the wrapper |
|---|---|---|
| `kind` | `u16` | Event kind, compared with the `EVT_*` constants. |
| `button` | `u16` | Button-related payload for pointer events. Numeric button values are not defined in the supplied RSBR ABI declarations. |
| `x` | `i32` | X coordinate carried by the event. Coordinate origin and whether it is client-relative or screen-relative must be confirmed against the kernel event producer. |
| `y` | `i32` | Y coordinate carried by the event. Coordinate origin and whether it is client-relative or screen-relative must be confirmed against the kernel event producer. |
| `key` | `u32` | Key code for keyboard events. |
| `mods` | `u16` | Modifier bitmask; `MOD_SHIFT`, `MOD_CTRL`, and `MOD_ALT` are defined. |
| `data` | `u16` | Additional event payload. Its per-event meaning is not specified by the current public ABI declarations. |
| `_reserved` | `[u32; 3]` | Reserved padding in the Rust representation. |

The structure is `#[repr(C)]` and currently has a compile-time size assertion of 32 bytes. **This size assertion verifies the Rust structure only; it does not independently verify that its field order, field meanings, or total size match the kernel's current `gui_event_t`.** The source comment in `abi.rs` explicitly identifies the seven-field kernel layout as an assumption. Until it is checked against `kernel/gui/gui.h` and the event syscall implementation, the `Event` layout must be treated as a kernel-ABI item requiring verification, not as a fully proven cross-version contract.

The `Event` helper methods have deliberately narrow definitions:

- `is_mouse()` returns `true` only for `EVT_MOUSE_DOWN`, `EVT_MOUSE_UP`, `EVT_MOUSE_MOVE`, and `EVT_MOUSE_DBLCLICK`.
- `is_key()` returns `true` for `EVT_KEY_DOWN` and `EVT_KEY_UP`.

Consequently, `is_mouse()` currently does **not** include mouse wheel, right-button, or middle-button event kinds. Applications that need those events must match their `kind` explicitly or use their own helper.

##### Event kinds

The following values are declared in `abi.rs` and must remain numerically synchronized with `kernel/gui/gui.h`:

| Constant | Value | Declared event category |
|---|---:|---|
| `EVT_NONE` | 0 | No event / empty kind. |
| `EVT_MOUSE_MOVE` | 1 | Pointer movement. |
| `EVT_MOUSE_DOWN` | 2 | Mouse-button press event (right and middle buttons have separate event kinds). |
| `EVT_MOUSE_UP` | 3 | Mouse-button release event (right and middle buttons have separate event kinds). |
| `EVT_MOUSE_DBLCLICK` | 4 | Double-click event. |
| `EVT_MOUSE_WHEEL` | 5 | Wheel event. |
| `EVT_MOUSE_RIGHT_DOWN` | 6 | Right-button press event. |
| `EVT_MOUSE_RIGHT_UP` | 7 | Right-button release event. |
| `EVT_MOUSE_MIDDLE_DOWN` | 8 | Middle-button press event. |
| `EVT_MOUSE_MIDDLE_UP` | 9 | Middle-button release event. |
| `EVT_KEY_DOWN` | 10 | Key press event. |
| `EVT_KEY_UP` | 11 | Key release event. |
| `EVT_FOCUS` | 12 | Window receives focus. |
| `EVT_BLUR` | 13 | Window loses focus. |
| `EVT_RESIZE` | 14 | Window resize notification. |
| `EVT_CLOSE_REQ` | 15 | Close request from the window decoration or compositor. |
| `EVT_TIMER` | 16 | Timer-related event kind. |
| `EVT_PAINT` | 17 | Paint-related event kind. |
| `EVT_CONTEXT_MENU` | 18 | Context-menu-related event kind. |
| `EVT_SNAP_CHANGED` | 19 | Window snap-state change. |
| `EVT_DROP` | 20 | Drop-related event kind. |
| `EVT_ICON_CLICK` | 21 | Desktop-icon click event kind. |

The names and numeric values above are what RSBR currently declares. They do not, by themselves, define every payload field for each event. For example, the public ABI declarations do not currently specify the wheel delta encoding, drag/drop payload format, timer identifier, or context-menu payload.

##### Keyboard constants and modifiers

`Event::key` is a `u32`. The constants currently exported for special keys are:

| Constant | Value |
|---|---:|
| `KEY_LEFT` | `0x100` |
| `KEY_RIGHT` | `0x101` |
| `KEY_UP` | `0x102` |
| `KEY_DOWN` | `0x103` |
| `KEY_HOME` | `0x104` |
| `KEY_END` | `0x105` |
| `KEY_DELETE` | `0x109` |

The existing `App` example also compares `ev.key` with `0x1B` to detect Escape. The constants above are only the special-key constants present in the current `abi.rs`; they are not a complete keyboard map. The exact encoding of printable keys, function keys, keypad keys, and key-repeat events must be documented from the kernel input/event implementation.

Modifier flags are bit values in `Event::mods`:

| Constant | Bit/value | Meaning |
|---|---:|---|
| `MOD_SHIFT` | `1 << 0` (`0x0001`) | Shift modifier. |
| `MOD_CTRL` | `1 << 1` (`0x0002`) | Control modifier. |
| `MOD_ALT` | `1 << 2` (`0x0004`) | Alt modifier. |

Applications should test modifier bits with a bitwise operation, for example `ev.mods & MOD_CTRL != 0`, rather than compare the entire field to one modifier value. Combined modifiers can therefore be represented in the same mask.

##### Cursor and snap state

`Cursor` is a `#[repr(u32)]` enum used by `gui::set_cursor`:

| Variant | Value | Intended appearance |
|---|---:|---|
| `Arrow` | 0 | Default pointer. |
| `IBeam` | 1 | Text insertion pointer. |
| `Hand` | 2 | Link/interactive pointer. |
| `HResize` | 3 | Horizontal resize pointer. |
| `VResize` | 4 | Vertical resize pointer. |
| `DResize` | 5 | Diagonal resize pointer. |
| `Wait` | 6 | Busy pointer. |
| `Move` | 7 | Move pointer. |
| `Crosshair` | 8 | Crosshair pointer. |
| `NotAllowed` | 9 | Operation-not-allowed pointer. |

`Snap` is a `#[repr(u32)]` enum used by `Window::snap`:

| Variant | Value |
|---|---:|
| `None` | 0 |
| `Left` | 1 |
| `Right` | 2 |
| `Top` | 3 |
| `Bottom` | 4 |
| `Maximized` | 5 |

##### Font metrics and theme structure

`FontMetrics` contains five `u32` values returned by `font_metrics()`:

- `width` and `height` — reported bitmap-font dimensions.
- `ascent` and `descent` — reported vertical font metrics.
- `num_glyphs` — number of glyphs reported by the built-in font implementation.

`Theme` mirrors `gui_theme_t` and contains 33 `u32` fields (132 bytes). Color-like fields describe desktop, window, title bar, border, taskbar, start button, control, icon, and notification colors. Dimension/behavior fields include `shadow_offset`, `taskbar_h`, `titlebar_h`, `border_w`, `resize_grip`, `icon_size`, `icon_pad`, `win_round`, and `dpi`.

The exact field order is part of the ABI and is preserved in `abi.rs`:

```rust
#[repr(C)]
pub struct Theme {
    pub desktop_top: u32,
    pub desktop_bot: u32,
    pub win_bg: u32,
    pub win_content: u32,
    pub title_active: u32,
    pub title_inactive: u32,
    pub title_text: u32,
    pub border: u32,
    pub border_active: u32,
    pub taskbar_bg: u32,
    pub taskbar_border: u32,
    pub taskbar_text: u32,
    pub start_btn_bg: u32,
    pub start_btn_text: u32,
    pub close_bg: u32,
    pub close_bg_hover: u32,
    pub max_bg: u32,
    pub min_bg: u32,
    pub icon_text: u32,
    pub icon_selected: u32,
    pub notif_bg: u32,
    pub notif_border: u32,
    pub notif_text: u32,
    pub shadow_color: u32,
    pub shadow_offset: u32,
    pub taskbar_h: u32,
    pub titlebar_h: u32,
    pub border_w: u32,
    pub resize_grip: u32,
    pub icon_size: u32,
    pub icon_pad: u32,
    pub win_round: u32,
    pub dpi: u32,
}
```

The compile-time assertion checks `size_of::<Theme>() == 33 * 4`. `theme()` initializes a zeroed/default structure, asks the kernel to fill it, and returns the populated value. `set_theme()` passes the supplied structure to the kernel; it is a global compositor operation rather than a per-window style setter.

#### 7.4.4. Window creation, flags, ownership, and lifecycle

##### Creating a window

```rust
let window = Window::create(
    100, 100,
    640, 480,
    Some("My application"),
    FLAG_HAS_TITLE | FLAG_HAS_CLOSE | FLAG_RESIZABLE,
)?;
window.show()?;
```

`Window::create(x, y, w, h, title, flags)` creates a window and returns its RAII wrapper. The new window starts hidden; `show()` makes it visible. `title` is optional: `None` passes a null pointer to the kernel. `x` and `y` are the requested window position, while `w` and `h` are the requested dimensions passed to the create operation.

The flags currently defined in `abi.rs` are:

| Flag | Value | Meaning indicated by the flag name |
|---|---:|---|
| `FLAG_RESIZABLE` | `0x001` | Window can be resized. |
| `FLAG_MOVABLE` | `0x002` | Window can be moved. |
| `FLAG_HAS_TITLE` | `0x004` | Show a title bar. |
| `FLAG_HAS_CLOSE` | `0x008` | Show a close control. |
| `FLAG_HAS_MINMAX` | `0x010` | Show minimize/maximize controls. |
| `FLAG_MODAL` | `0x020` | Modal window flag. |
| `FLAG_NO_DECOR` | `0x040` | Suppress standard decorations. |
| `FLAG_ALWAYS_TOP` | `0x080` | Keep the window above ordinary windows. |
| `FLAG_TOOL_WINDOW` | `0x100` | Tool-window style flag. |
| `FLAG_BORDERLESS` | `0x200` | Borderless window flag. |
| `FLAG_PRESENTATION` | `0x800` | Presentation-mode flag. |

The numeric values must match `AG_WIN_*` / `GUI_WIN_*` definitions in `kernel/gui/gui.h`. Flags may be combined with bitwise OR. The precise interactions between combinations (for example, `FLAG_NO_DECOR` and `FLAG_HAS_TITLE`) are determined by the kernel window implementation.

##### Window lifetime and ownership

| API | Behavior |
|---|---|
| `Window::wid()` | Returns the kernel window ID without transferring ownership. |
| `Window::into_raw(self)` | Consumes the wrapper, suppresses its destructor, and returns the raw ID. The caller becomes responsible for eventual destruction. |
| `unsafe Window::from_raw(wid)` | Creates an owning wrapper for an existing live window. The caller must ensure the ID belongs to the process and has no other RAII owner. |
| `Drop for Window` | Issues `OP_DESTROY` on a best-effort basis. Any destroy error is ignored. |

Do not create two owning `Window` wrappers for the same ID. Doing so can cause one wrapper to destroy the window while the other still appears valid.

##### Window state and geometry

| API | Behavior |
|---|---|
| `show()` / `hide()` | Show or hide the window. |
| `focus()` | Request keyboard/window focus for this window. |
| `lower()` | Lower the window in the Z-order. |
| `minimize()` / `maximize()` / `restore()` | Request the corresponding window-state transition. |
| `snap(Snap)` | Request a snap state (`Left`, `Right`, `Top`, `Bottom`, or `Maximized`). |
| `move_to(x, y)` | Request a new window position. |
| `resize(w, h)` | Request a new window size. |
| `size()` | Return the current dimensions reported by the compositor. |
| `pos()` | Return the current position reported by the compositor. |
| `rect()` | Return `(x, y, w, h)` from the compositor. |
| `set_title(title)` | Change the title; `None` passes a null pointer. |
| `flags()` / `set_flags(flags)` | Read or replace the window flag mask. |
| `z()` | Read the current Z-order value. |
| `set_parent(Some(parent))` | Set another window as parent. |
| `set_parent(None)` | Pass `-1` as the parent ID to the kernel. |
| `set_hole(l, t, r, b)` | Pass a rectangular hole/cutout to the compositor. The exact coordinate and hit-testing semantics are kernel-defined. |

The geometry methods forward their arguments to the compositor. The public Rust wrapper does not clamp coordinates or dimensions before issuing the syscall. Applications should query `size()` after a resize event when they need the current dimensions rather than relying only on the original create arguments.

#### 7.4.5. Pointer and keyboard input

The GUI input interface is event-based. Applications receive an `Event` from the window event queue and inspect `kind`, `button`, `x`, `y`, `key`, `mods`, and `data` as appropriate for that event kind.

The RSBR layer declares event kinds and basic key/modifier constants, but it does not translate raw events into higher-level widgets or control actions. A file manager, for example, must implement its own item hit-testing, selection, double-click behavior, hover tracking, scrolling, and keyboard navigation.

##### Pointer events

The public event kinds include:

- `EVT_MOUSE_MOVE` — pointer movement.
- `EVT_MOUSE_DOWN` and `EVT_MOUSE_UP` — primary-button press/release.
- `EVT_MOUSE_DBLCLICK` — double-click notification.
- `EVT_MOUSE_WHEEL` — wheel input.
- `EVT_MOUSE_RIGHT_DOWN` / `EVT_MOUSE_RIGHT_UP` — right-button press/release.
- `EVT_MOUSE_MIDDLE_DOWN` / `EVT_MOUSE_MIDDLE_UP` — middle-button press/release.

For pointer events, `x` and `y` carry coordinates and `button` carries a button-related value according to the declared structure. The current RSBR ABI file does not define the numeric button mapping, wheel delta encoding, or whether event coordinates are client-relative or screen-relative. These details must be taken from the kernel's event producer and documented here once verified. Do not hard-code assumptions about them in reusable widgets before that verification.

A `EVT_MOUSE_MOVE` stream can be frequent. In an `App::update` implementation, compare the new pointer position/hover target with the previous state and return `Flow::Changed` only when the visible hover state actually changes. Returning `Flow::Changed` for every movement causes a full application repaint for every move event.

##### Pointer capture

`Window::set_capture()` asks the compositor to route pointer events to this window. It returns the ID of the window that previously owned capture, or `-1` if no previous capturing window was reported. `Window::get_capture()` returns the current capture owner ID.

Capture is useful for interactions that continue after the pointer leaves a control or window area, such as dragging a selection rectangle or moving a custom slider. The current safe wrapper exposes capture acquisition and querying, but no dedicated `release_capture()` method. The release operation and whether capture is automatically relinquished on button release, focus loss, or window destruction must be verified against the kernel implementation before being described as a guaranteed behavior.

##### Global pointer position and cursor

`gui::mouse_position()` queries the global pointer position and returns `Option<(i32, i32)>`:

- `Some((x, y))` when the wrapper considers a pointer position available.
- `None` when no pointer is reported.

The current implementation accepts a syscall return value of `1` as present. It also accepts a return value of `0` when at least one output coordinate is non-zero. This compatibility heuristic means that the `(0, 0)` position combined with a zero syscall return is ambiguous and can be reported as `None`.

`gui::set_cursor(cursor)` changes the active cursor shape using the `Cursor` enum. It is a global GUI operation; the current wrapper does not provide a per-window cursor property.

##### Keyboard events

`EVT_KEY_DOWN` and `EVT_KEY_UP` report key transitions. `Event::key` contains the key code and `Event::mods` contains the modifier mask. Applications should generally handle both transitions when they need to track held keys; a one-shot command can normally be triggered on key-down.

The current `abi.rs` provides only a small set of special-key constants plus the Escape value used in the example. It does not define a complete key-code table, text-input/Unicode event, keyboard layout translation, or key-repeat contract. Those behaviors must not be inferred from the presence of `EVT_KEY_DOWN` alone.

`EVT_FOCUS` and `EVT_BLUR` allow an application to react to focus changes. A robust input handler should clear any locally tracked pressed-key state on blur if the kernel does not guarantee matching key-up events after focus is lost; that guarantee requires kernel-side verification.

##### Event queue access

| API | Behavior |
|---|---|
| `Window::poll()` | Non-blocking read. Returns `Ok(Some(event))` when an event is available and `Ok(None)` when the event ring is empty. |
| `Window::wait()` | Blocking read. Waits until an event is delivered or the window/event syscall returns an error. |

`poll()` is appropriate for an application that owns its own loop or wants to drain currently pending events. `wait()` is appropriate for an event-driven application that should sleep while idle. The wrapper does not document the kernel queue capacity, overflow policy, event coalescing rules, or ordering guarantees beyond the one-event-per-call interface.

#### 7.4.6. Drawing, pixel buffers, and invalidation

Window drawing methods operate on the window's client drawing buffer. Each public drawing primitive issues one GUI call; the wrapper does not construct a higher-level display list.

The color format for ordinary drawing methods is `0x00RRGGBB`.

| API | Behavior |
|---|---|
| `clear(color)` | Clear the entire window client buffer to one color. |
| `fill_rect(x, y, w, h, color)` | Fill a rectangle. |
| `draw_rect(x, y, w, h, color)` | Draw a rectangle outline. |
| `draw_line(x0, y0, x1, y1, color)` | Draw a line segment. |
| `draw_pixel(x, y, color)` | Draw one pixel. |
| `draw_text(x, y, text, color)` | Draw text using the compositor's built-in bitmap font. |
| `get_pixel(x, y)` | Read one pixel and return its lower 24 RGB bits; alpha is stripped. |

The Rust wrapper forwards drawing coordinates to the kernel. The exact origin, clipping behavior, treatment of negative coordinates, text baseline convention, and DPI scaling are kernel-side rendering contracts and are not fully specified by the current Rust declarations.

##### Bulk pixel upload

`blit` and `blit_alpha` copy a caller-owned `&[u32]` pixel buffer into a rectangular destination area:

```rust
window.blit(x, y, width, height, &pixels, stride_pixels)?;
window.blit_alpha(x, y, width, height, &pixels, stride_pixels)?;
```

Arguments:

- `x`, `y` — destination position passed to the kernel.
- `w`, `h` — destination width and height in pixels.
- `src` — source pixels in `u32` format.
- `stride` — source row stride in pixels, not bytes. A value of `0` is converted to `w` (tightly packed rows).

Before issuing the syscall, the wrapper computes `h * stride` with checked arithmetic and rejects a source slice shorter than that number of pixels. The caller must provide a valid row layout; in ordinary images, use a stride at least as large as the copied row width. The entire rectangle is transferred through one GUI syscall.

`blit_alpha` interprets the high byte of each source pixel as alpha (`0` transparent, `255` opaque). `blit` is the non-alpha variant. The source slice must remain valid for the duration of the synchronous syscall; the wrapper does not retain the pointer.

##### Frame brackets and dirty regions

`frame_begin()` and `frame_end()` bracket a drawing frame. They are used by `gui::run` to group the app's drawing calls before invalidation/presentation.

`invalidate()` marks the whole window for presentation. `invalidate_rect(x, y, w, h)` marks a rectangular region. Invalidation is separate from drawing: drawing changes the window buffer, while invalidation tells the compositor that the changed area should be presented.

`gui::render_now()` is a global immediate-present request. It is not a substitute for updating the window buffer or marking the correct dirty area.

#### 7.4.7. Clipboard and desktop integration

##### Clipboard

- `Window::set_clipboard(text)` writes text to the system clipboard.
- `Window::get_clipboard(buf)` asks the kernel to copy clipboard data into a caller-provided byte buffer.

`get_clipboard` returns `Result<()>`, not the number of bytes copied. The current wrapper therefore does not expose an explicit copied-length value or a truncation indicator. The exact NUL-termination and truncation contract must be checked against the kernel clipboard implementation before callers rely on a particular string-decoding strategy.

##### Notifications and desktop icons

- `gui::notify(text, color, duration_ms)` requests a desktop notification.
- `gui::add_icon(x, y, label, icon_id)` registers a desktop shortcut and returns the resulting integer index/ID reported by the kernel.
- `gui::remove_icon(idx)` requests removal of a previously registered desktop icon.

The Rust wrapper passes `icon_id` through unchanged. The available icon-resource mapping and the notification duration/time-unit behavior are kernel/UI contracts; this section does not define additional icon IDs or resource formats.

##### Theme and font queries

- `gui::theme()` reads the current global theme.
- `gui::set_theme(&theme)` applies a global theme. The kernel-side access check requires the calling process to own at least one active window (`gui_pid_has_windows`).
- `gui::font_metrics()` reads the built-in font metrics into a fixed five-element output buffer.

#### 7.4.8. Application interface and managed event loop

##### `App`

An application implementing `App` provides two methods:

```rust
pub trait App {
    fn update(&mut self, ev: &Event) -> Flow;
    fn render(&self, w: &Window) -> crate::Result<()>;
}
```

`update` receives one event and may mutate application state. `render` receives an immutable reference to the application and draws the current state. The immutable `&self` is intentional: rendering should observe state prepared by event handling rather than mutate application behavior as a side effect of painting.

##### `Flow`

| Variant | Meaning | Repaint requested? | Stop loop? |
|---|---|---:|---:|
| `Flow::Ignore` | Event handled or intentionally ignored; no visible state change. | No | No |
| `Flow::Changed` | Visible application state changed. | Yes | No |
| `Flow::Quit` | Visible state changed and the app requests termination. | Yes | Yes |

`Flow::changed()` returns `true` for `Changed` and `Quit`. `Flow::quit()` returns `true` only for `Quit`.

**Important:** `EVT_MOUSE_MOVE` should normally return `Flow::Ignore` unless the hover target, drag state, selection, or another visible value actually changed. A repaint on every mouse movement defeats the dirty-driven loop and can create unnecessary CPU/GPU work.

##### Behavior of `gui::run`

The managed loop performs the following steps:

1. Render the initial frame before waiting for input.
2. Block in `Window::wait()` for one event.
3. Call `App::update(&event)`.
4. Render a new frame if the event is `EVT_RESIZE` or the returned `Flow` is `Changed`/`Quit`.
5. Stop after processing a `Flow::Quit` event.

A resize always schedules a repaint even if the app returns `Flow::Ignore`, because the framework knows the client-area geometry changed. The resize event is still passed to `update`, so the app can update layout state or query `Window::size()`.

For each rendered frame, the framework calls `frame_begin()`, invokes `App::render()`, calls `frame_end()`, and then calls `invalidate()`. Drawing errors are propagated by `gui::run`. In the current implementation, any error returned by `Window::wait()` ends the loop; the loop then returns `Ok(())` rather than propagating that wait error. This behavior should be reconsidered if callers need to distinguish window destruction from other event-syscall failures.

##### Basic application example

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
            EVT_KEY_DOWN if ev.key == 0x1B => Flow::Quit,
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

##### Manual event loop

Applications do not have to use `App` or `gui::run`. They can call `poll()` or `wait()` directly and manage rendering themselves. A manual loop must still define its own redraw policy and should avoid continuously repainting when no visible state has changed.

#### 7.4.9. Global compositor API reference

| API | Description |
|---|---|
| `screen_size()` | Returns desktop resolution as `(width, height)`. |
| `mouse_position()` | Returns the wrapper's optional global pointer position. |
| `focused_window()` | Returns the focused window ID. |
| `top_window()` | Returns the top-most window ID. |
| `set_cursor(cursor)` | Changes the global pointer cursor. |
| `render_now()` | Requests immediate compositor presentation. |
| `theme()` | Reads the current global `Theme`. |
| `set_theme(theme)` | Applies a global theme; requires the process to own an active window. |
| `font_metrics()` | Returns built-in font metrics. |
| `notify(text, color, duration_ms)` | Displays a desktop notification. |
| `add_icon(x, y, label, icon_id)` | Adds a desktop icon and returns its kernel-reported ID/index. |
| `remove_icon(idx)` | Removes a desktop icon by ID/index. |

#### 7.4.10. Window API reference

| Area | API | Description |
|---|---|---|
| Lifecycle | `Window::create(...)` | Creates a hidden window. |
| Lifecycle | `show()`, `hide()`, `Drop` | Show, hide, and destroy the window. |
| Focus/Z-order | `focus()`, `lower()`, `z()` | Focus, lower, and query Z-order. |
| State | `minimize()`, `maximize()`, `restore()`, `snap(...)` | Request window state transitions. |
| Geometry | `move_to(...)`, `resize(...)` | Change window geometry. |
| Geometry | `size()`, `pos()`, `rect()` | Query current geometry. |
| Properties | `set_title(...)`, `flags()`, `set_flags(...)` | Read or change title and flags. |
| Parent/cutout | `set_parent(...)`, `set_hole(...)` | Set parent relationship or compositor hole. |
| Pointer | `set_capture()`, `Window::get_capture()` | Acquire or query pointer capture. |
| Drawing | `clear(...)`, `fill_rect(...)`, `draw_rect(...)` | Clear or draw rectangles. |
| Drawing | `draw_line(...)`, `draw_pixel(...)`, `draw_text(...)` | Draw primitives or bitmap text. |
| Pixel access | `get_pixel(...)` | Read one pixel. |
| Pixel upload | `blit(...)`, `blit_alpha(...)` | Copy a pixel buffer. |
| Frame/invalidation | `frame_begin()`, `frame_end()` | Bracket drawing. |
| Frame/invalidation | `invalidate()`, `invalidate_rect(...)` | Mark content dirty for presentation. |
| Events | `poll()`, `wait()` | Read one event without blocking or block for one event. |
| Clipboard | `set_clipboard(...)`, `get_clipboard(...)` | Write or read clipboard bytes. |
| Raw ownership | `wid()`, `into_raw()`, `Window::from_raw(...)` | Inspect or transfer the raw window ID. |

#### 7.4.11. Limits, ABI verification, and syscall mapping

##### String and buffer limits

- **C-string buffer:** `with_cstr` uses a 256-byte stack buffer. It copies at most 255 input bytes and appends a terminating NUL. Longer values are silently truncated by bytes, not Unicode characters; truncation can split a UTF-8 sequence. The helper does not reject an embedded NUL byte, so a kernel C-string consumer will observe only the prefix before that NUL.
- **Pixel buffers:** `blit` and `blit_alpha` do not allocate. The source slice must contain at least `height * effective_stride` pixels, where an input stride of zero is treated as the width. The wrapper checks multiplication overflow and slice length before the syscall.
- **Event ABI:** `Event` is currently declared as 32 bytes, but its source comment marks the kernel field layout as an assumption. Verify it against the matching kernel header and event-copy code before relying on it as a stable binary contract.
- **Event payload semantics:** button IDs, pointer-coordinate origin, wheel delta, `data` meanings, drag/drop payloads, timer payloads, and key-repeat semantics are not fully defined by the current public RSBR declarations.
- **Capture release:** the safe API exposes acquisition and owner query but no explicit release method. Verify the kernel's release rules before relying on implicit release behavior.
- **Clipboard result length:** `get_clipboard` currently returns `Result<()>`; it does not report the number of bytes copied or an explicit truncation status.
- **Wait errors:** `gui::run` currently treats any `Window::wait()` error as loop termination and returns success after leaving the loop.
- **Platform verification:** the module status below is the status recorded for the current implementation. A syscall backend existing for another architecture does not itself prove the GUI kernel implementation is available there.

##### Kernel ABI facts that should be added after source verification

The following details should be confirmed from `kernel/gui/gui.h`, `kernel/gui/gui_syscalls.h`, the GUI event producer, and the `SYS_GUI_EVENT` dispatcher before this section makes stronger claims:

1. Exact `gui_event_t` field order, field widths, alignment, and copy size.
2. Coordinate space for `Event::x` / `Event::y`, including whether capture changes the coordinate space.
3. Numeric pointer-button values and the exact payload used for wheel events.
4. Key-code mapping, printable-key behavior, modifiers, and repeat delivery.
5. Event queue capacity, overflow policy, event coalescing, and ordering guarantees.
6. Whether pointer capture is automatically released and how an application explicitly releases it.
7. Payload formats for `EVT_TIMER`, `EVT_PAINT`, `EVT_CONTEXT_MENU`, `EVT_DROP`, `EVT_SNAP_CHANGED`, and `EVT_ICON_CLICK`.
8. Drawing coordinate origin, clipping rules, text baseline, DPI scaling, and the semantics of `set_hole`.
9. Clipboard buffer termination, length reporting, and truncation behavior.
10. Exact time unit and scheduling behavior for `notify(..., duration_ms)`.

##### Syscall mapping

| Operation | Native syscall wrapper | Number |
|---|---|---:|
| Window lifecycle, properties, drawing, capture, and compositor commands (`OP_*`) | `gui_call` | `200` |
| Window event polling/waiting | `gui_event` | `201` |
| Get/set global compositor theme | `gui_theme` | `202` |

The operation constants (`OP_CREATE` through `OP_FRAME_END`) and event/flag constants are low-level ABI values. Application code should normally call the safe public methods rather than invoke `gui_call` directly. These numeric values must be kept synchronized with `kernel/gui/gui_syscalls.h` and `kernel/gui/gui.h` in the same change that updates the kernel ABI.

**Status:** implemented and runtime-verified on `x86_64` according to the current RSBR documentation. The `Event` layout and the input details listed above still require direct verification against the matching kernel sources before they can be considered fully specified.


**### 7.5. \`fs\`**



**Source:*** `lib/rsbr/fs/\



The `fs` module is the Rust-facing filesystem API for AuraLite applications. It wraps the kernel's file-descriptor and path-based filesystem interfaces and provides an owned `File` type, metadata structures, directory enumeration, and path operations.



The module is allocator-free: callers provide the buffers used for file contents and directory entries.



**

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
