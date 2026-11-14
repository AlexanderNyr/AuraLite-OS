#![no_std]

use rsbr::prelude::*;

#[no_mangle]
pub extern "Rust" fn main() -> i32 {
    println!("Hello from AuraLite!");

    match getpid() {
        Ok(pid) => println!("Process ID: {}", pid),
        Err(error) => eprintln!("getpid failed: {}", error),
    }

    0
}