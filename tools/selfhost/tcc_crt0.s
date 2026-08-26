/* tools/selfhost/tcc_crt0.s -- AuraLite guest crt0/syscall glue for tcc.
 *
 * SELFHOST_PLAN.md SH2.  GNU as (AT&T) syntax, assembled by the guest
 * tcc itself (`tcc -c tcc_crt0.s -o crt0.o`) -- tcc ships an assembler,
 * so the guest toolchain needs no external asm tool.
 *
 * Why assembly and not C+inline-asm: tcc ignores __attribute__((naked))
 * and emits a prologue (push rbp; mov rsp,rbp; sub $0,rsp) BEFORE the
 * inline-asm body, which shifts RSP and turns the stack decode of
 * _start into garbage (observed: argc read from the saved rbp slot).
 * A .s file has no prologue by construction.
 *
 * These are AuraLite's own replacements for the NASM trio
 * (crt/crt0.asm, src/syscall.asm, crt/sigreturn.asm) -- same behaviour,
 * same ABI, no assembler dependency.
 */

/* ---- _start: decode the System V initial process stack and call
 * __libc_start_main(argc, argv, envp).  RSP at entry:
 *   [rsp+0]=argc  [rsp+8]=argv[0..]  NULL  envp[0..]  NULL  auxv   */
    .text
    .globl _start
_start:
    xor  %rbp, %rbp
    mov  (%rsp), %rdi          /* argc */
    lea  8(%rsp), %rsi         /* argv */
    mov  %rsi, %rdx
    lea  (%rdx,%rdi,8), %rdx   /* &argv[argc] (the NULL slot) */
    add  $8, %rdx              /* envp */
    call __libc_start_main
1:  jmp  1b                    /* __libc_start_main never returns */

/* ---- syscall: the generic wrapper (src/syscall.asm) --------------------
 * C: long syscall(long num, long a1, long a2, long a3,
 *                 long a4, long a5, long a6);
 * SysV: rdi rsi rdx rcx r8 r9 [rsp+8] -> SYSCALL: rax rdi rsi rdx r10 r8 r9 */
    .globl syscall
syscall:
    mov  %rdi, %rax
    mov  %rsi, %rdi
    mov  %rdx, %rsi
    mov  %rcx, %rdx
    mov  %r8,  %r10
    mov  %r9,  %r8
    mov  8(%rsp), %r9
    syscall
    ret

/* ---- __sigreturn: the signal-return trampoline (crt/sigreturn.asm) ---- */
    .globl __sigreturn
__sigreturn:
    mov  $15, %rax             /* SYS_SIGRETURN */
    syscall
    ud2                        /* unreachable: kernel iretq's instead */
