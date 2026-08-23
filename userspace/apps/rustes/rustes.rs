#![no_std]
#![no_main]

// RESIDUE R8: the same benchmark on three ISAs -- the receipt lines
// are IDENTICAL by construction (the plan's exit is "the same
// receipt line the x86_64 edition prints"); only the cycle counter
// is per-arch: rdtsc / rdtime / cntvct_el0.  The tenant kernels
// open the U-mode counter gates (scounteren, CNTKCTL_EL1) in the
// same R8 commit -- measured by this program running, not assumed.

extern crate rsbr;

use rsbr::{print, println, getpid};

#[cfg(target_arch = "x86_64")]
fn read_tsc() -> u64 {
    unsafe {
        let low: u32;
        let high: u32;
        core::arch::asm!(
            "rdtsc",
            out("eax") low,
            out("edx") high,
            options(nostack)
        );
        ((high as u64) << 32) | (low as u64)
    }
}

#[cfg(target_arch = "riscv64")]
fn read_tsc() -> u64 {
    unsafe {
        let t: u64;
        core::arch::asm!("rdtime {}", out(reg) t, options(nostack));
        t
    }
}

#[cfg(target_arch = "aarch64")]
fn read_tsc() -> u64 {
    unsafe {
        let t: u64;
        core::arch::asm!("mrs {}, cntvct_el0", out(reg) t,
                         options(nostack));
        t
    }
}

fn print_u64(mut n: u64) {
    if n == 0 {
        print("0");
        return;
    }
    let mut buf = [0u8; 20];
    let mut i = 0;
    while n > 0 && i < 20 {
        buf[i] = (b'0' + (n % 10) as u8) as u8;
        n /= 10;
        i += 1;
    }
    let mut s = [0u8; 20];
    let mut j = 0;
    while i > 0 {
        i -= 1;
        s[j] = buf[i];
        j += 1;
    }
    let slice = &s[0..j];
    unsafe {
        print(core::str::from_utf8_unchecked(slice));
    }
}

#[no_mangle]
pub fn main() -> i32 {
    println("=== Rust Benchmark ===");

    let _pid = getpid();

    let iterations = 1_000_000;
    print("Running ");
    print_u64(iterations);
    println(" iterations...");

    let start = read_tsc();

    let mut sum: u64 = 0;
    let mut i: u64 = 0;
    while i < iterations {
        sum += i;
        i += 1;
    }

    let end = read_tsc();
    let cycles = end - start;

    print("Sum: ");
    print_u64(sum);
    print("\n");

    print("TSC cycles: ");
    print_u64(cycles);
    print("\n");

    print("Per iteration: ");
    print_u64(cycles / iterations);
    println(" cycles");

    println("\nBenchmark complete!");
    0
}
