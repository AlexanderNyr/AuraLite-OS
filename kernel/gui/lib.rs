// kernel/gui/lib.rs

#![no_std]

use core::panic::PanicInfo;

mod drawing;
mod dirty;
mod render;
mod desktop;

#[path = "sdf/sdf.rs"]
mod sdf;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}