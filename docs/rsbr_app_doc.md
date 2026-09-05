# Building Rust Applications with RSBR

This document guides you through developing native AuraLite-OS applications using pure Rust without relying on C dependencies.

## What is RSBR?

RSBR is a lightweight integration bridge developed by **Van009**. It is **not** a full-fledged standard library; instead, it serves as a minimalist hardware and kernel abstraction layer, providing the essential fundamentals required to build functional bare-metal Rust utilities.

### Application Boilerplate

Every standalone RSBR application must comply with `no_std` environments. Because RSBR internally implements and handles the global `_start` entry point and the default `#[panic_handler]`, your application code stays incredibly clean. 

Below is the absolute minimum required code structure:

```rust
#![no_std]
#![no_main]

extern crate rsbr;

// Import required symbols from the bridge
use rsbr::println;

#[no_mangle]
pub fn main() -> i32 {
    println("Hello, world!");
    0 // Return success status code to the OS
}
```

---

## RSBR API Reference

RSBR communicates directly with the AuraLite-OS kernel via each target's
native system-call instruction — x86_64 `syscall`, riscv64 `ecall` (a7),
aarch64 `svc #0` (x8); see “Three ISAs, one bridge” below and
`docs/rust_application_guide.md` §7 for the per-tenant details.

### Core App Functions

These functions provide a clean, idiomatic Rust interface for userspace applications.

#### `print`
```rust
pub fn print(s: &str)
```
* **Description:** Prints a string slice directly to the standard output (`stdout`, file descriptor `1`).
* **Under the hood:** Triggers `SYS_WRITE` (1).

#### `println`
```rust
pub fn println(s: &str)
```
* **Description:** Prints a string slice followed by a newline character (`\n`).

#### `getpid`
```rust
pub fn getpid() -> i32
```
* **Description:** Retrieves the unique Process ID of the currently executing application.
* **Under the hood:** Triggers `SYS_GETPID` (39).

#### `exit`
```rust
pub fn exit(code: i32) -> !
```
* **Description:** Terminates the current process immediately with the specified status code. This function never returns (`-> !`).
* **Under the hood:** Triggers `SYS_EXIT` (60).

---

### FFI / C-Compatible Exports

RSBR exposes C-compatible symbols using the `extern "C"` calling convention and `#[no_mangle]` attributes. These primitives allow foreign C modules to interface with Rust space if required.

| C Function Signature | Rust Equivalent | Description |
| :--- | :--- | :--- |
| `rsbr_print(msg: *const u8, len: usize)` | `print()` | Safe pointer-based console printing (includes explicit `null` checks). |
| `rsbr_getpid() -> i32` | `getpid()` | Exposes the current process ID directly to foreign C contexts. |
| `rsbr_exit(code: i32) -> !` | `exit()` | Triggers immediate process termination from foreign C contexts. |

---

## Internal Execution Flow

Developers do not need to implement low-level runtime symbols. RSBR abstractly manages the entire application lifecycle under the hood:

1. **Runtime Definition:** RSBR acts as the root crate, defining the global `_start` entry point and the mandatory `#[panic_handler]`.
2. **Main Invocation:** It securely transfers execution control to your defined `pub fn main() -> i32`.
3. **Implicit Cleanup:** Upon execution completion, RSBR traps the returned `i32` register value and automatically routes it through `exit(code)` to prevent core hangs.

---

## Three ISAs, one bridge (RESIDUE R8)

RSBR is not x86-only: `common.rs` carries `cfg(target_arch)` siblings
of the syscall shims (x86_64 `syscall`, riscv64 `ecall`/a7, aarch64
`svc #0`/x8) and a per-arch cycle counter.  The tenant builds link no
C archive — implicit `memset`/`memcpy`/`memcmp` land in a `cfg`-gated
intrinsics module that is OFF on x86_64.  The same application source
produces byte-identical receipts on all three architectures.
