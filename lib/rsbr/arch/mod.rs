#[cfg(target_arch = "x86_64")]
mod x86_64;
#[cfg(target_arch = "riscv64")]
mod riscv64;
#[cfg(target_arch = "aarch64")]
mod aarch64;

#[cfg(target_arch = "x86_64")]
pub(crate) use self::x86_64::*;
#[cfg(target_arch = "riscv64")]
pub(crate) use self::riscv64::*;
#[cfg(target_arch = "aarch64")]
pub(crate) use self::aarch64::*;

#[cfg(not(any(
    target_arch = "x86_64",
    target_arch = "riscv64",
    target_arch = "aarch64"
)))]
compile_error!("RSBR currently supports x86_64, riscv64 and aarch64 targets");
