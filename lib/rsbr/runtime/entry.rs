unsafe extern "Rust" {
    fn main() -> i32;
}

/// ELF entry point for RSBR applications.
#[no_mangle]
pub extern "C" fn _start() -> ! {
    let exit_code = unsafe { main() };
    crate::process::exit(exit_code)
}
