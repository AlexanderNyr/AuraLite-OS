; =============================================================================
; context.asm — context switch for preemptive multitasking.
;
; C prototype: void context_switch(tcb_t *old, tcb_t *new);
;   rdi = pointer to old thread's TCB (rsp field is at offset 0)
;   rsi = pointer to new thread's TCB (rsp field is at offset 0)
;
; Only the callee-saved registers (rbx, rbp, r12-r15) are saved/restored —
; caller-saved registers are the responsibility of the callers.  RSP is saved
; into old->rsp and loaded from new->rsp.  The `ret` instruction pops the new
; thread's saved RIP (or, for a freshly-created thread, the thread_entry
; trampoline address), effectively "returning" into the new thread.
;
; Called with interrupts disabled (the caller manages the interrupt state).
; =============================================================================

bits 64
default rel

section .text
global context_switch

context_switch:
    ; ---- Save callee-saved registers onto the current (old) stack ----
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; ---- Save old RSP into old_tcb->rsp (field at offset 0) ----
    mov [rdi], rsp

    ; ---- Load new RSP from new_tcb->rsp (field at offset 0) ----
    mov rsp, [rsi]

    ; ---- Restore callee-saved registers from the new stack ----
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; ---- "Return" into the new thread (pops saved RIP / trampoline addr) ----
    ret
