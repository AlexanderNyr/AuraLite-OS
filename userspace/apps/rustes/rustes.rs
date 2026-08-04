#![no_std]
#![no_main]

extern crate rsbr;

use rsbr::{print, println, getpid, exit};

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