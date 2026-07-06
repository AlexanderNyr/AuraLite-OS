#ifndef AURALITE_LIBC_SETJMP_H
#define AURALITE_LIBC_SETJMP_H

/*
 * setjmp.h — POSIX.1-2024 <setjmp.h> for AuraLite user programs.
 *
 * setjmp()/longjmp() are implemented in libc/crt/setjmp.asm (System V AMD64
 * ABI, callee-saved registers only — see that file for the exact jmp_buf
 * layout).  sigsetjmp()/siglongjmp() additionally save/restore the caller's
 * signal mask via sigprocmask() and are implemented in libc/src/compat.c.
 */

/* jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp, rip (8 slots x 8 bytes). */
typedef long jmp_buf[8];

/* sigjmp_buf: jmp_buf plus a saved sigset_t (padded to a long for
 * alignment) plus a "mask was saved" flag. */
typedef long sigjmp_buf[10];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

int  sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((__noreturn__));

#endif /* AURALITE_LIBC_SETJMP_H */
