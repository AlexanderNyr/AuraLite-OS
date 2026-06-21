; =============================================================================
; boot.asm — AuraLite OS 64-bit kernel entry point, invoked by Limine.
;
; Limine hands control in long mode, paging enabled, higher-half mapped, with a
; usable stack. We establish our own stack, defensively zero the .bss, and call
; the C entry point kmain(). We never return.
; =============================================================================

bits 64
default rel

section .bss
align 16
stack_bottom:
    resb 65536                 ; 64 KiB kernel stack
stack_top:

section .text
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli                        ; no interrupts until the IDT is up (Phase 2)

    ; Establish our own, deterministically-sized stack.
    lea rsp, [rel stack_top]

    ; Zero the .bss section. Limine does this too, but we do not depend on it.
    cld                        ; ensure STOSB advances forward (DF = 0)
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi               ; rcx = byte count
    xor eax, eax               ; al = 0 (fill byte)
    rep stosb                  ; memset(rdi, 0, rcx)

    ; Enter C. System V AMD64: no arguments expected by kmain().
    call kmain

.hang:
    cli
    hlt
    jmp .hang
