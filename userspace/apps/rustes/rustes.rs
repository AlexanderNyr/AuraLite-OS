#![no_std]
#![no_main]

use core::panic::PanicInfo;
use core::arch::asm;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn rust_main(_argc: i32, _argv: *mut *mut u8, _envp: *mut *mut u8) {
    let msg = b"Default Rust Program\n";
    
    unsafe {
        let fd: i64 = 1;
        let buf: *const u8 = msg.as_ptr();
        let count: i64 = msg.len() as i64;
        let syscall_num: i64 = 1;
        
        let result: i64;
        asm!(
            "syscall",
            inout("rax") syscall_num => result,
            in("rdi") fd,
            in("rsi") buf,
            in("rdx") count,
            out("rcx") _,
            out("r11") _,
            options(nostack)
        );
    }
}