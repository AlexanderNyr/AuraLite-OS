; =============================================================================
; boot.asm -- AuraLite OS 64-bit kernel entry point.
;
; Entered in long mode with paging enabled and the higher half already
; mapped by the bootloader (BIOS Stage 2, UEFI BOOTX64.EFI, or the
; optional Limine fallback).  Contract:
;
;   * RDI  = physical (identity-mapped) pointer to boot_info_t.
;   * RSP  = any temporary stack the loader chose; we replace it below.
;   * IF   = 0 (bootloader is responsible for masking interrupts).
;
; We enable SSE (needed for the software 3D renderer), install our own
; deterministic stack, defensively zero the .bss section, restore the
; boot_info pointer into RDI, and tail-call kmain(boot_info_t *).
; kmain never returns; if it does, we halt permanently.
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

    ; ---- Save the bootloader-provided boot_info pointer.
    ; We stash it in R15 (a callee-saved GPR that the SysV AMD64 ABI
    ; guarantees we own) *before* touching any memory, so that the
    ; .bss zero-fill below cannot corrupt it.  We cannot push it onto
    ; a stack yet because our stack sits inside .bss -- pushing then
    ; zeroing would erase the saved value.
    mov r15, rdi

    ; ---- Enable SSE (needed for float in the 3D renderer) ----
    mov rax, cr0
    and ax, 0xFFFB             ; clear EM (Emulation) bit
    or  ax, 0x2                ; set   MP (Monitor coProcessor) bit
    mov cr0, rax
    mov rax, cr4
    or  ax, 3 << 9             ; set OSFXSR (bit 9) + OSXMMEXCPT (bit 10)
    mov cr4, rax

    ; Zero the .bss section.  The bootloader is not required to.
    ; Do this BEFORE we start using the .bss-resident stack.
    cld                        ; STOSB advances forward (DF = 0)
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi               ; rcx = byte count
    xor eax, eax               ; al = 0 (fill byte)
    rep stosb                  ; memset(rdi, 0, rcx)

    ; Now that .bss is zeroed, install our own deterministic stack.
    lea rsp, [rel stack_top]

    ; Restore boot_info pointer into RDI -- first argument to kmain per
    ; the System V AMD64 ABI.
    mov rdi, r15

    ; Enter C.
    call kmain

.hang:
    cli
    hlt
    jmp .hang
