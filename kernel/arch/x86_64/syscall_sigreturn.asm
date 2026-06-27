; =============================================================================
; syscall_sigreturn.asm — iretq slow path for delivering a signal at syscall
; exit.
;
; void syscall_iret_to_user(struct registers *frame) __attribute__((noreturn));
;
; Loads the user register state from a kernel-resident `struct registers` and
; returns to Ring 3 via IRETQ.  Used when a signal handler frame was set up on
; the syscall-return path (where the normal fast path would SYSRET).  Using
; IRETQ avoids the SYSRET non-canonical-RIP #GP-in-Ring0 hazard and fully
; reloads CS/SS/RSP/RFLAGS/RIP.
;
; struct registers layout (kernel/arch/x86_64/isr.h), offsets from frame base:
;   0:r15 8:r14 16:r13 24:r12 32:r11 40:r10 48:r9 56:r8
;   64:rdi 72:rsi 80:rbp 88:rdx 96:rcx 104:rbx 112:rax
;   120:int_no 128:err_code
;   136:rip 144:cs 152:rflags 160:rsp 168:ss
; =============================================================================

bits 64
default rel

section .text
global syscall_iret_to_user

syscall_iret_to_user:
    ; rdi = frame pointer.  Build the IRETQ frame on the current kernel stack
    ; from the saved fields, then restore GPRs and iretq.
    mov rax, rdi                 ; rax = &frame (rdi will be overwritten)

    ; Push the 5-word IRETQ frame: SS, RSP, RFLAGS, CS, RIP (top-down).
    mov rbx, [rax + 168]         ; ss
    push rbx
    mov rbx, [rax + 160]         ; rsp
    push rbx
    mov rbx, [rax + 152]         ; rflags
    push rbx
    mov rbx, [rax + 144]         ; cs
    push rbx
    mov rbx, [rax + 136]         ; rip
    push rbx

    ; Restore GPRs from the frame (rax/rdi last since we use rax as base).
    mov r15, [rax + 0]
    mov r14, [rax + 8]
    mov r13, [rax + 16]
    mov r12, [rax + 24]
    mov r11, [rax + 32]
    mov r10, [rax + 40]
    mov r9,  [rax + 48]
    mov r8,  [rax + 56]
    mov rsi, [rax + 72]
    mov rbp, [rax + 80]
    mov rdx, [rax + 88]
    mov rcx, [rax + 96]
    mov rbx, [rax + 104]
    mov rdi, [rax + 64]
    mov rax, [rax + 112]         ; rax last

    iretq
