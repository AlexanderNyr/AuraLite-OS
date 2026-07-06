; =============================================================================
; setjmp.asm — x86_64 setjmp()/longjmp() for AuraLite OS user programs.
;
; System V AMD64 ABI callee-saved registers only (POSIX.1-2024 <setjmp.h>).
; No FPU/SSE state is saved: the kernel never touches x87/SSE registers for
; user threads today (see Makefile's -mno-sse notes for the *kernel*; user
; programs built without SSE codegen do not need it either).  If a future
; program relies on FP register survival across setjmp/longjmp, that support
; must be added here together with FPU/SSE enablement elsewhere.
;
; jmp_buf layout (8 x 8 = 64 bytes), matches libc/include/setjmp.h:
;   [0]  rbx   [8]  rbp   [16] r12  [24] r13
;   [32] r14   [40] r15   [48] rsp  [56] rip
; =============================================================================

bits 64
default rel

section .text
global setjmp, longjmp

; int setjmp(jmp_buf env)
; RDI = env
setjmp:
    mov  [rdi + 0],  rbx
    mov  [rdi + 8],  rbp
    mov  [rdi + 16], r12
    mov  [rdi + 24], r13
    mov  [rdi + 32], r14
    mov  [rdi + 40], r15
    lea  rax, [rsp + 8]           ; RSP as seen by our caller (post-ret)
    mov  [rdi + 48], rax
    mov  rax, [rsp]               ; caller's return address (RIP to resume at)
    mov  [rdi + 56], rax
    xor  eax, eax                 ; setjmp() returns 0 on the direct call
    ret

; void longjmp(jmp_buf env, int val)
; RDI = env, ESI = val
longjmp:
    mov  eax, esi
    test eax, eax
    jnz  .nonzero
    inc  eax                      ; longjmp(env, 0) must make setjmp() return 1
.nonzero:
    mov  rbx, [rdi + 0]
    mov  rbp, [rdi + 8]
    mov  r12, [rdi + 16]
    mov  r13, [rdi + 24]
    mov  r14, [rdi + 32]
    mov  r15, [rdi + 40]
    mov  rsp, [rdi + 48]
    jmp  qword [rdi + 56]         ; resume at the saved setjmp() call site
