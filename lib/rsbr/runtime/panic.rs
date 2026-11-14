use core::panic::PanicInfo;

#[panic_handler]
fn panic(info: &PanicInfo<'_>) -> ! {
    let _ = crate::io::eprintln_fmt(core::format_args!("RSBR panic: {}", info));
    crate::process::exit(101)
}
