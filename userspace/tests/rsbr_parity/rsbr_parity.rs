#![no_std]

use core::{arch::asm, hint::black_box};

// Exercise the user-mode timer gate on both DTB tenants. The portable
// clock_gettime syscall is not implemented by these kernels yet.
fn counter() -> u64 {
    let value: u64;
    #[cfg(target_arch = "riscv64")]
    unsafe {
        asm!("rdtime {value}", value = out(reg) value, options(nostack));
    }
    #[cfg(target_arch = "aarch64")]
    unsafe {
        asm!("mrs {value}, cntvct_el0", value = out(reg) value, options(nostack));
    }
    value
}

#[no_mangle]
pub fn main() -> i32 {
    rsbr::println!("=== Rust Benchmark ===");
    let start = counter();
    let mut sum = 0u64;
    for n in 0..1_000_000u64 {
        sum += black_box(n);
    }
    let end = counter();
    rsbr::println!("Sum: {}", sum);
    if sum != 499_999_500_000 || end < start {
        return 1;
    }
    rsbr::println!("Benchmark complete!");
    0
}
