//! AuraLite x86-64 syscall calling convention.
//!
//! RAX = syscall number; RDI, RSI, RDX, R10, R8, R9 = arguments.
//! RAX = result. SYSCALL always clobbers RCX and R11.

#[inline(always)]
pub(crate) unsafe fn syscall0(number: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "syscall",
        inlateout("rax") number as isize => result,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall1(number: usize, arg1: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "syscall",
        inlateout("rax") number as isize => result,
        in("rdi") arg1,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall2(number: usize, arg1: usize, arg2: usize) -> isize {
    let result: isize;
    core::arch::asm!(
        "syscall",
        inlateout("rax") number as isize => result,
        in("rdi") arg1,
        in("rsi") arg2,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall3(
    number: usize,
    arg1: usize,
    arg2: usize,
    arg3: usize,
) -> isize {
    let result: isize;
    core::arch::asm!(
        "syscall",
        inlateout("rax") number as isize => result,
        in("rdi") arg1,
        in("rsi") arg2,
        in("rdx") arg3,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall4(
    number: usize,
    arg1: usize,
    arg2: usize,
    arg3: usize,
    arg4: usize,
) -> isize {
    let result: isize;
    core::arch::asm!(
        "syscall",
        inlateout("rax") number as isize => result,
        in("rdi") arg1,
        in("rsi") arg2,
        in("rdx") arg3,
        in("r10") arg4,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall5(
    number: usize,
    arg1: usize,
    arg2: usize,
    arg3: usize,
    arg4: usize,
    arg5: usize,
) -> isize {
    let result: isize;
    core::arch::asm!(
        "syscall",
        inlateout("rax") number as isize => result,
        in("rdi") arg1,
        in("rsi") arg2,
        in("rdx") arg3,
        in("r10") arg4,
        in("r8") arg5,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    result
}

#[inline(always)]
pub(crate) unsafe fn syscall6(
    number: usize,
    arg1: usize,
    arg2: usize,
    arg3: usize,
    arg4: usize,
    arg5: usize,
    arg6: usize,
) -> isize {
    let result: isize;
    core::arch::asm!(
        "syscall",
        inlateout("rax") number as isize => result,
        in("rdi") arg1,
        in("rsi") arg2,
        in("rdx") arg3,
        in("r10") arg4,
        in("r8") arg5,
        in("r9") arg6,
        lateout("rcx") _,
        lateout("r11") _,
        options(nostack)
    );
    result
}
