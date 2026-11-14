//! AuraLite AArch64 syscall calling convention.
//!
//! X8 = syscall number; X0-X5 = arguments; X0 = result.

#[inline(always)]
pub(crate) unsafe fn syscall0(number: usize) -> isize {
    let result: isize;
    core::arch::asm!("svc #0", in("x8") number, lateout("x0") result, options(nostack));
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall1(number: usize, arg1: usize) -> isize {
    let result: isize;
    core::arch::asm!("svc #0", in("x8") number, inlateout("x0") arg1 => result, options(nostack));
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall2(number: usize, arg1: usize, arg2: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "svc #0",
        in("x8") number,
        inlateout("x0") arg1 => result,
        in("x1") arg2,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall3(number: usize, arg1: usize, arg2: usize, arg3: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "svc #0",
        in("x8") number,
        inlateout("x0") arg1 => result,
        in("x1") arg2,
        in("x2") arg3,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall4(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "svc #0",
        in("x8") number,
        inlateout("x0") arg1 => result,
        in("x1") arg2,
        in("x2") arg3,
        in("x3") arg4,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall5(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize, arg5: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "svc #0",
        in("x8") number,
        inlateout("x0") arg1 => result,
        in("x1") arg2,
        in("x2") arg3,
        in("x3") arg4,
        in("x4") arg5,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall6(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize, arg5: usize, arg6: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "svc #0",
        in("x8") number,
        inlateout("x0") arg1 => result,
        in("x1") arg2,
        in("x2") arg3,
        in("x3") arg4,
        in("x4") arg5,
        in("x5") arg6,
        options(nostack)
    );
    result
}
