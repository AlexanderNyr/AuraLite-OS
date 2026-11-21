// kernel/gui/lib.rs

#![no_std]

use core::panic::PanicInfo;

mod drawing;
mod dirty;
mod framebuffer;
mod render;
mod desktop;
mod compositor;
mod kvec;

#[path = "sdf/sdf.rs"]
mod sdf;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}