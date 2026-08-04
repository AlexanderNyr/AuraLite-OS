#![no_std]

const SYS_WRITE: u64 = 1;
const SYS_EXIT: u64 = 60;
const SYS_GETPID: u64 = 39;

#[inline(always)]
unsafe fn syscall1(n: u64, a1: u64) -> i64 {
    let ret: i64;
    core::arch::asm!(
        "syscall",
        in("rax") n,
        in("rdi") a1,
        lateout("rax") ret,
        options(nostack)
    );
    ret
}

#[inline(always)]
unsafe fn syscall3(n: u64, a1: u64, a2: u64, a3: u64) -> i64 {
    let ret: i64;
    core::arch::asm!(
        "syscall",
        in("rax") n,
        in("rdi") a1,
        in("rsi") a2,
        in("rdx") a3,
        lateout("rax") ret,
        options(nostack)
    );
    ret
}

pub fn print(s: &str) {
    unsafe {
        syscall3(SYS_WRITE, 1, s.as_ptr() as u64, s.len() as u64);
    }
}

pub fn println(s: &str) {
    print(s);
    print("\n");
}

pub fn getpid() -> i32 {
    unsafe { syscall1(SYS_GETPID, 0) as i32 }
}

pub fn exit(code: i32) -> ! {
    unsafe {
        syscall1(SYS_EXIT, code as u64);
    }
    loop {}
}

#[no_mangle]
pub extern "C" fn rsbr_print(msg: *const u8, len: usize) {
    if !msg.is_null() && len > 0 {
        unsafe {
            syscall3(SYS_WRITE, 1, msg as u64, len as u64);
        }
    }
}

#[no_mangle]
pub extern "C" fn rsbr_getpid() -> i32 {
    getpid()
}

#[no_mangle]
pub extern "C" fn rsbr_exit(code: i32) -> ! {
    exit(code)
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    println("panic occurred");
    exit(1)
}

extern "Rust" {
    fn main() -> i32;
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let code = unsafe { main() };
    exit(code);
}