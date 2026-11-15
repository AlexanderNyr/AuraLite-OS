#![no_std]

use core::panic::PanicInfo;

mod gui_drawing;
mod gui_dirty;

#[path = "sdf/gui_sdf.rs"]
mod gui_sdf;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
