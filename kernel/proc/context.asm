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

; TCB_TLS_BASE et al. are auto-generated from tcb_t via tools/gen_asm_offsets.c
; so this never drifts out of sync with the C struct layout.
%include "asm_offsets.inc"

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

    ; SMP step 3.2: the old thread's saved frame is now COMPLETE.  Publish
    ; that to any cpu that may have just dequeued it (schedule() cleared
    ; switch_parked before enqueueing a still-running thread; pickers spin
    ; on this byte).  Plain dword store, x86 TSO keeps it after the saves.
    mov dword [rdi + TCB_SWITCH_PARKED], 1

    mov rsp, [rsi]             ; load new RSP from new_tcb->rsp

    ; P9: Restore FS.base for TLS (pthread).  Offset comes from asm_offsets.inc.
    ; FIX_R3: unconditional, via the IA32_FS_BASE MSR (0xC0000100) rather
    ; than wrfsbase -- CR4.FSGSBASE is never enabled on any CPU in this
    ; tree, so wrfsbase raised #UD here and killed the kernel the first
    ; time a pthread child got scheduled.  Unconditional also means a
    ; thread with tls_base == 0 does NOT keep the previous tenant's FS:
    ; FS.base is always exactly the incoming thread's value.
    mov rax, [rsi + TCB_TLS_BASE]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 0xC0000100        ; IA32_FS_BASE
    wrmsr                      ; FS.base <- new_tcb->tls_base

    popfq                      ; restore RFLAGS
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret                        ; "return" into the new thread
