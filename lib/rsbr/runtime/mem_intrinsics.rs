// RV64 and AArch64 Rust tenants do not link a C archive. Keep the compiler's
// basic C memory intrinsics available from the Rust runtime on those targets.
// x86-64 retains its existing libc/compiler-builtins link path.

#[cfg(any(target_arch = "riscv64", target_arch = "aarch64"))]
// These redefine C memory intrinsics that the Rust runtime expects
// but that the AuraLite tenant does not link (no libc, no
// compiler-builtins archive on RV64/AArch64). `#[no_mangle]` is the
// whole point: the compiler emits calls to bare `memcpy`/`memset`
// and we satisfy them here.
mod intrinsics {
    #[no_mangle]
    pub unsafe extern "C" fn memset(destination: *mut u8, value: i32, length: usize) -> *mut u8 {
        let mut index = 0;
        while index < length {
            unsafe {
                *destination.add(index) = value as u8;
            }
            index += 1;
        }
        destination
    }

    #[no_mangle]
    pub unsafe extern "C" fn memcpy(destination: *mut u8, source: *const u8, length: usize) -> *mut u8 {
        let mut index = 0;
        while index < length {
            unsafe {
                *destination.add(index) = *source.add(index);
            }
            index += 1;
        }
        destination
    }

    #[no_mangle]
    pub unsafe extern "C" fn memcmp(left: *const u8, right: *const u8, length: usize) -> i32 {
        let mut index = 0;
        while index < length {
            let left_byte;
            let right_byte;
            unsafe {
                left_byte = *left.add(index);
                right_byte = *right.add(index);
            }

            if left_byte != right_byte {
                return left_byte as i32 - right_byte as i32;
            }
            index += 1;
        }
        0
    }
}
