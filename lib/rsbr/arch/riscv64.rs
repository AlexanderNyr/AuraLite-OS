//! AuraLite RV64 syscall calling convention.
//!
//! A7 = syscall number; A0-A5 = arguments; A0 = result.

#[inline(always)]
pub(crate) unsafe fn syscall0(number: usize) -> isize {
    let result: isize;
    core::arch::asm!("ecall", in("a7") number, lateout("a0") result, options(nostack));
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall1(number: usize, arg1: usize) -> isize {
    let result: isize;
    core::arch::asm!("ecall", in("a7") number, inlateout("a0") arg1 => result, options(nostack));
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall2(number: usize, arg1: usize, arg2: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "ecall",
        in("a7") number,
        inlateout("a0") arg1 => result,
        in("a1") arg2,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall3(number: usize, arg1: usize, arg2: usize, arg3: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "ecall",
        in("a7") number,
        inlateout("a0") arg1 => result,
        in("a1") arg2,
        in("a2") arg3,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall4(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "ecall",
        in("a7") number,
        inlateout("a0") arg1 => result,
        in("a1") arg2,
        in("a2") arg3,
        in("a3") arg4,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall5(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize, arg5: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "ecall",
        in("a7") number,
        inlateout("a0") arg1 => result,
        in("a1") arg2,
        in("a2") arg3,
        in("a3") arg4,
        in("a4") arg5,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall6(number: usize, arg1: usize, arg2: usize, arg3: usize, arg4: usize, arg5: usize, arg6: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "ecall",
        in("a7") number,
        inlateout("a0") arg1 => result,
        in("a1") arg2,
        in("a2") arg3,
        in("a3") arg4,
        in("a4") arg5,
        in("a5") arg6,
        options(nostack)
    );
    result
}
