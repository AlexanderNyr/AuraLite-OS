#![no_std]

// RESIDUE R8: one bridge, three ISAs.  The x86_64 blocks are the
// original row untouched; riscv64/aarch64 arrive as cfg siblings --
// the SAME D4 syscall numbers (the convention the C shims already
// share), only the trap instruction and register names differ:
//   x86_64:  syscall  rax=n  rdi/rsi/rdx
//   riscv64: ecall    a7=n   a0/a1/a2
//   aarch64: svc #0   x8=n   x0/x1/x2

const SYS_WRITE: u64 = 1;
const SYS_EXIT: u64 = 60;
const SYS_GETPID: u64 = 39;

#[cfg(target_arch = "x86_64")]
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

#[cfg(target_arch = "x86_64")]
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

#[cfg(target_arch = "riscv64")]
#[inline(always)]
unsafe fn syscall1(n: u64, a1: u64) -> i64 {
    let ret: i64;
    core::arch::asm!(
        "ecall",
        in("a7") n,
        inlateout("a0") a1 => ret,
        options(nostack)
    );
    ret
}

#[cfg(target_arch = "riscv64")]
#[inline(always)]
unsafe fn syscall3(n: u64, a1: u64, a2: u64, a3: u64) -> i64 {
    let ret: i64;
    core::arch::asm!(
        "ecall",
        in("a7") n,
        inlateout("a0") a1 => ret,
        in("a1") a2,
        in("a2") a3,
        options(nostack)
    );
    ret
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
unsafe fn syscall1(n: u64, a1: u64) -> i64 {
    let ret: i64;
    core::arch::asm!(
        "svc #0",
        in("x8") n,
        inlateout("x0") a1 => ret,
        options(nostack)
    );
    ret
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
unsafe fn syscall3(n: u64, a1: u64, a2: u64, a3: u64) -> i64 {
    let ret: i64;
    core::arch::asm!(
        "svc #0",
        in("x8") n,
        inlateout("x0") a1 => ret,
        in("x1") a2,
        in("x2") a3,
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

// The tenants link no C archive (libcrv/libca64 are header-only
// static inline), so the compiler's implicit mem-intrinsic calls
// land here.  x86_64 keeps taking them from the user libc at link
// time -- these stay cfg'd OFF there to avoid a duplicate symbol.
#[cfg(any(target_arch = "riscv64", target_arch = "aarch64"))]
#[allow(suspicious_runtime_symbol_definitions)]
mod mem_intrinsics {
    #[no_mangle]
    pub unsafe extern "C" fn memset(s: *mut u8, c: i32, n: usize)
        -> *mut u8 {
        let mut i = 0;
        while i < n {
            *s.add(i) = c as u8;
            i += 1;
        }
        s
    }
    #[no_mangle]
    pub unsafe extern "C" fn memcpy(d: *mut u8, s: *const u8, n: usize)
        -> *mut u8 {
        let mut i = 0;
        while i < n {
            *d.add(i) = *s.add(i);
            i += 1;
        }
        d
    }
    #[no_mangle]
    pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize)
        -> i32 {
        let mut i = 0;
        while i < n {
            let x = *a.add(i);
            let y = *b.add(i);
            if x != y {
                return x as i32 - y as i32;
            }
            i += 1;
        }
        0
    }
}

extern "Rust" {
    fn main() -> i32;
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let code = unsafe { main() };
    exit(code);
}
