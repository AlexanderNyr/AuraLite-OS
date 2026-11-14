mod numbers;

pub(crate) use self::numbers::*;

use crate::arch;
use crate::common::error::{Error, Result};

const MAX_ERRNO: isize = 4095;

#[inline(always)]
fn decode(raw: isize) -> Result<usize> {
    // AuraLite returns errors in the reserved Linux-style -1..=-4095 band.
    if raw < 0 && raw >= -MAX_ERRNO {
        Err(Error::Syscall((-raw) as i32))
    } else {
        Ok(raw as usize)
    }
}

#[inline(always)]
pub(crate) fn call0(number: usize) -> Result<usize> {
    decode(unsafe { arch::syscall0(number) })
}

#[inline(always)]
pub(crate) fn call1(number: usize, arg1: usize) -> Result<usize> {
    decode(unsafe { arch::syscall1(number, arg1) })
}

#[inline(always)]
pub(crate) fn call2(number: usize, arg1: usize, arg2: usize) -> Result<usize> {
    decode(unsafe { arch::syscall2(number, arg1, arg2) })
}

#[inline(always)]
pub(crate) fn call3(number: usize, arg1: usize, arg2: usize, arg3: usize) -> Result<usize> {
    decode(unsafe { arch::syscall3(number, arg1, arg2, arg3) })
}

#[inline(always)]
pub(crate) fn call4(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize) -> Result<usize> {
    decode(unsafe { arch::syscall4(number, arg1, arg2, arg3, arg4) })
}

#[inline(always)]
pub(crate) fn call5(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize, arg5: usize) -> Result<usize> {
    decode(unsafe { arch::syscall5(number, arg1, arg2, arg3, arg4, arg5) })
}

#[inline(always)]
pub(crate) fn call6(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize, arg5: usize, arg6: usize) -> Result<usize> {
    decode(unsafe { arch::syscall6(number, arg1, arg2, arg3, arg4, arg5, arg6) })
}
