; =============================================================================
; context.asm — context switch for preemptive multitasking.
;
; C prototype: void context_switch(tcb_t *old, tcb_t *new);
;   rdi = pointer to old thread's TCB (rsp field is at offset 0)
;   rsi = pointer to new thread's TCB (rsp field is at offset 0)
;
; Saves/restores callee-saved registers (rbx, rbp, r12-r15) + RSP + RFLAGS.
; RFLAGS is saved so the interrupt-enable flag (IF) doesn't leak between
; threads (critical: a thread running with IF=0 in a SYSCALL handler must
; not inherit IF=1 from a thread it was switched from).
; =============================================================================

bits 64
default rel

section .text
global context_switch

context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq                     ; save RFLAGS (including IF)

    mov [rdi], rsp             ; save old RSP into old_tcb->rsp

    mov rsp, [rsi]             ; load new RSP from new_tcb->rsp

    popfq                      ; restore RFLAGS
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret                        ; "return" into the new thread
