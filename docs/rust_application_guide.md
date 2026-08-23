# AuraLite OS Rust Application Guide

This guide explains how to write, build, and troubleshoot freestanding Rust
applications (`#![no_std]`) for AuraLite OS, including console utilities and
graphical AuraGUI v2.0 applications.

See also [`build_and_run.md`](build_and_run.md) for toolchain setup and
[`syscall_abi.md`](syscall_abi.md) for the low-level system call interface.

---

## 1. Architecture and Freestanding Runtime

AuraLite OS applications are statically linked ELF64 binaries loaded at a fixed
virtual address (`0x40000000`). User programs do not rely on dynamic linkers or
host operating system runtimes.

Rust applications in AuraLite OS are compiled as freestanding binaries using:

```rust
#![no_std]
#![no_main]
```

### Why a full `std` port is unnecessary

The standard Rust library (`std`) assumes a full POSIX-compliant host operating
system with POSIX threads (`pthread_*`), advanced signal handling, complex file
locking (`fcntl`), and dynamic linking. Porting `std` to a bare-metal kernel
requires extensive kernel modification and adds substantial binary bloat.

In AuraLite OS, **`#![no_std]` is fully sufficient** for rich user-space
applications, including graphical interfaces and complex data structures. By
linking against AuraLite's freestanding C library (`libaurac.a`) and GUI
toolkit (`libauragui.a`), Rust programs gain access to heap allocation, system
calls, and windowing primitives without the overhead of `std`.

---

## 2. Dynamic Memory Allocation (`alloc`) Without `std`

To use heap-allocated collections such as `Vec<T>`, `String`, `Box<T>`, and
`BTreeMap<K, V>` in a `#![no_std]` application, you only need the standard
`alloc` crate and a global memory allocator.

### Implementing `#[global_allocator]`

Instead of writing a custom heap allocator from scratch or modifying the
kernel, wrap AuraLite's existing C runtime allocator (`malloc` and `free`
from `libaurac.a`).

Add the following 15-line allocator implementation to your Rust runtime module
(e.g., `lib/rsbr/common.rs` or your application's entry module):

```rust
extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};

struct LibcAllocator;

unsafe impl GlobalAlloc for LibcAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        extern "C" {
            fn malloc(size: usize) -> *mut u8;
        }
        malloc(layout.size())
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        extern "C" {
            fn free(ptr: *mut u8);
        }
        free(ptr);
    }
}

#[global_allocator]
static ALLOCATOR: LibcAllocator = LibcAllocator;
```

### Using standard collections

Once `#[global_allocator]` is defined, you can import and use `alloc` types
normally:

```rust
extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;
use alloc::format;

pub fn example_usage() {
    let mut names: Vec<String> = Vec::new();
    names.push(String::from("AuraLite"));
    let greeting = format!("Hello, {}!", names[0]);
}
```

---

## 3. FFI Rules: Strings and Null-Termination

A common source of bugs when calling C APIs (such as `ag_window_create`) from
Rust is the memory layout of strings.

### The Null-Termination Trap

In Rust, ordinary string literals (`&str`) consist of a pointer and a length.
**They are not null-terminated (`\0`)** by default.

If you pass an ordinary string literal pointer to a C function expecting a
null-terminated C string (`const char *`):

```rust
// WRONG: will read past the end of the string in memory!
let title = "Rust GUI".as_ptr() as *const i8;
```

The C function reads memory sequentially until it encounters a `0x00` byte.
In ELF binaries, string literals are packed closely in the `.rodata` section.
If a panic handler string such as `"panic occurred\n"` sits adjacent to
`"Rust GUI"` in memory, the C API will read both strings concatenated together,
producing erroneous window titles like:

```
Rust GUIpanic occurred
```

### Correct C-String Literals

To safely pass strings from Rust to C functions, **always use null-terminated
C-string literals** (`c"..."`) or explicitly append a `\0` byte:

```rust
// Preferred (Rust 2021 / 1.77+): C-string literal
let title = c"AuraLite Rust GUI".as_ptr();

// Alternative: explicitly null-terminated byte array
let title = "AuraLite Rust GUI\0".as_ptr() as *const i8;
```

---

## 4. AuraGUI v2.0 Applications in Rust

AuraGUI v2.0 is an immediate/retained hybrid toolkit. A graphical program must
perform three distinct steps to render a window:

1. **Create the window frame:** `ag_window_create(...)` allocates the window on
   the compositor desktop.
2. **Make the window visible:** `ag_window_show(...)` displays the window border
   and title bar.
3. **Run the GUI event loop:** Without an event loop, the interior of the window
   remains unrendered (appearing as a blank white rectangle) and does not respond
   to user input.

### Why an empty window occurs

Calling `ag_window_create` and `ag_window_show` without initializing a view and
running `ag_view_run` causes the window manager to display the undecorated
client area without drawing child widgets or handling repaint events.

### The GUI Event Loop

To initialize widgets and start rendering, bind an `ag_view_t` structure to the
window ID and execute the toolkit event loop:

```rust
// 1. Create and show window
let wid = ag_window_create(100, 100, 320, 200, c"Rust App".as_ptr(), AG_WIN_DEFAULT);
ag_window_show(wid);

// 2. Initialize the widget view
let mut view = core::mem::zeroed::<ag_view_t>();
let mut widgets = [core::mem::zeroed::<ag_widget_t>(); 16];
ag_view_init(&mut view, wid, widgets.as_mut_ptr(), widgets.len() as i32, AG_PANEL);

// 3. Populate widgets and enter event loop
ag_add_label(&mut view, 20, 20, c"Hello from Rust".as_ptr(), AG_ACCENT);
ag_view_run(&mut view, 0, 0); // Blocks until window is closed
```

---

## 5. Complete Worked Example: A GUI Application in Rust

Below is a complete, freestanding Rust application that creates a window with
a button, a text readout, and an event handler.

```rust
#![no_std]
#![no_main]

extern crate alloc;

use core::panic::PanicInfo;
use core::ptr;

// ---- Panic Handler ---------------------------------------------------------
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

// ---- C API Bindings for AuraGUI --------------------------------------------
const AG_WIN_HAS_TITLE: u32 = 0x01;
const AG_WIN_HAS_CLOSE: u32 = 0x02;
const AG_WIN_MOVABLE:   u32 = 0x04;
const AG_WIN_DEFAULT:   u32 = AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_MOVABLE;

const AG_PANEL:  u32 = 0;
const AG_ACCENT: u32 = 1;

#[repr(C)]
struct ag_view_t {
    wid: i32,
    widgets: *mut u8, // Opaque widget array pointer
    count: i32,
    capacity: i32,
    style: u32,
}

extern "C" {
    fn ag_window_create(x: i32, y: i32, w: i32, h: i32, title: *const i8, flags: u32) -> i32;
    fn ag_window_show(wid: i32);
    fn ag_window_invalidate(wid: i32);
    fn ag_view_init(view: *mut ag_view_t, wid: i32, buf: *mut u8, cap: i32, style: u32);
    fn ag_add_label(view: *mut ag_view_t, x: i32, y: i32, text: *const i8, style: u32) -> *mut u8;
    fn ag_add_button(
        view: *mut ag_view_t,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        text: *const i8,
        cb: extern "C" fn(*mut u8, *mut u8),
        user: *mut u8,
    ) -> *mut u8;
    fn ag_view_run(view: *mut ag_view_t, idle_cb: *const u8, idle_user: *const u8);
}

// ---- Event Callback --------------------------------------------------------
extern "C" fn on_button_click(_widget: *mut u8, _user: *mut u8) {
    // Handle button click event
}

// ---- Application Entry Point -----------------------------------------------
#[no_mangle]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8) -> i32 {
    unsafe {
        let wid = ag_window_create(
            150, 150, 300, 180,
            c"Rust GUI App".as_ptr(),
            AG_WIN_DEFAULT,
        );
        if wid < 0 {
            return 1;
        }
        ag_window_show(wid);

        let mut view = core::mem::zeroed::<ag_view_t>();
        let mut widget_buf = [0u8; 1024];
        ag_view_init(
            &mut view,
            wid,
            widget_buf.as_mut_ptr(),
            8,
            AG_PANEL,
        );

        ag_add_label(&mut view, 20, 20, c"Built with Rust & AuraGUI".as_ptr(), AG_ACCENT);
        ag_add_button(&mut view, 20, 60, 120, 30, c"Click Me".as_ptr(), on_button_click, ptr::null_mut());

        // Process GUI events until the window is closed
        ag_view_run(&mut view, ptr::null(), ptr::null());
    }
    0
}
```

---

## 6. Building and Packaging Checklist

When adding a new Rust application to the repository:

1. **Source directory:** Place GUI applications under `userspace/apps/gui-<app_name>/`
   and terminal programs under `userspace/apps/<app_name>/`.
2. **Build rules:** In `Makefile`, add a compilation rule for your `.rs` source
   using `$(RUSTC) $(RUSTFLAGS) -o $@ $<` and link the resulting object against
   `$(USER_COMMON)` and `libauragui.a`.
3. **Initrd inclusion:** Add the executable target `.elf` name to `INITRD_APPS`
   in `Makefile` so `make initrd` packages it into `/apps/` in the boot image.
4. **Verification:** Test your build locally using:
   ```bash
   make -j$(nproc) test-unit
   make iso
   ```

---

## 7. Rust on the riscv64 / aarch64 tenants (RESIDUE R8)

The x86_64 guide above transfers to both DTB tenants: `rustes` builds
for `riscv64gc-unknown-none-elf` and `aarch64-unknown-none` through
the same one-source recipe (`rustc --emit obj` + the tenant's own
layout script; `_start` comes from the rlib) and runs from
`/binrv/rustes` and `/bina64/rustes` in the one four-tenant initrd.
`lib/rsbr/common.rs` carries `cfg`-gated syscall shims for all three
ISAs (`syscall`/`ecall a7`/`svc #0 x8` — the same D4 numbers the C
shims share), and the cycle counter is `rdtsc`/`rdtime`/`cntvct_el0`
per arch.  The receipt strings are shared text: the benchmark output
is byte-identical on all three, asserted in the rv_fs/a64_fs smokes.
Install the targets with
`rustup target add riscv64gc-unknown-none-elf aarch64-unknown-none`.
