; =============================================================================
; user_entry.asm — transition from Ring 0 to Ring 3 via iretq.
;
; C prototype: void jump_to_user(uint64_t entry, uint64_t stack_top,
;                                 uint64_t stack_bottom);
;   rdi = user RIP (entry point)
;   rsi = user RSP (stack top - 16, pre-decremented for ABI alignment)
;   rdx = (unused — stack_bottom)
;
; We push the iretq frame (SS, RSP, RFLAGS, CS, RIP) on the KERNEL stack and
; execute iretq. The CPU atomically loads these and drops to Ring 3.
; =============================================================================

bits 64
default rel

section .text
global jump_to_user_asm

%define USER_CS 0x1B
%define USER_SS 0x23

jump_to_user_asm:
    ; Build RFLAGS with IF set (so interrupts work in Ring 3).
    pushfq
    pop rax
    or rax, 0x200              ; set IF
    and rax, ~0x100            ; clear TF (trap flag)

    ; Push the iretq frame in order (low address first):
    ;   SS, RSP, RFLAGS, CS, RIP
    push USER_SS               ; SS
    push rsi                   ; RSP (user stack top)
    push rax                   ; RFLAGS
    push USER_CS               ; CS
    push rdi                   ; RIP (entry point)

    iretq                      ; -> Ring 3, RIP <- rdi, RSP <- rsi
