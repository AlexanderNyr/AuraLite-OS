; =============================================================================
; sigreturn.asm — signal return trampoline for AuraLite user programs.
;
; The kernel pushes the address of __sigreturn as the signal handler's return
; address.  When the handler executes `ret`, control lands here with RSP
; pointing exactly at the kernel's `struct signal_frame`.  We must NOT modify
; the stack before the syscall — the kernel reads the frame at the current RSP.
;
; SYS_SIGRETURN = 15.  This syscall does not return (the kernel iretq's to the
; interrupted context), so no cleanup is needed after `syscall`.
; =============================================================================

bits 64
default rel

section .text
global __sigreturn

__sigreturn:
    mov rax, 15            ; SYS_SIGRETURN
    syscall
    ; Unreachable: the kernel restores the interrupted context via IRETQ.
    ud2
