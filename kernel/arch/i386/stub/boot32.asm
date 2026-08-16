; =============================================================================
; kernel/arch/i386/stub/boot32.asm -- i386 kernel entry point (I386_PLAN I1).
;
; Sibling of kernel/arch/x86_64/boot.asm, one width down.  Stage 2's
; enter_prot32 (boot/bios/stage2/pmode32.inc) jumps here in 32-bit
; protected mode, paging OFF, with:
;   * ESI = physical address of boot_info_t
;   * flat CS/DS/ES/FS/GS/SS (base 0, limit 4 GiB)
;   * a scratch ESP we do not trust
;
; Responsibilities, in order:
;   1. cli -- no IDT exists yet.
;   2. Zero .bss (the loader copied p_filesz and zero-filled to p_memsz,
;      but the stub does it again for the same reason boot.asm does:
;      the entry code must not depend on which loader ran).
;   3. ESP := stack_top (16 KiB in .bss -- the stub needs no more; the
;      real i386 kernel in I2 grows this to the 64 KiB boot.asm uses).
;   4. Push ESI and call kmain32 (cdecl: one stack argument).
;   5. If kmain32 ever returns: hlt loop.
; =============================================================================

bits 32

STACK_SIZE equ 16*1024

section .bss
align 16
stack_bottom:
    resb STACK_SIZE
stack_top:

section .text
global _start
extern kmain32

_start:
    cli

    ; Zero .bss.  __bss_start/__bss_end come from kernel32.ld, both
    ; 4-aligned by the script, so dword stores suffice.
    extern __bss_start
    extern __bss_end
    mov  edi, __bss_start
    mov  ecx, __bss_end
    sub  ecx, edi
    shr  ecx, 2
    xor  eax, eax
    rep  stosd

    ; A real stack.  NOTE: the rep stosd above just zeroed it -- that is
    ; fine, nothing lives there yet.  ESI survived (rep stosd touches
    ; EDI/ECX/EAX only).
    mov  esp, stack_top

    ; cdecl hand-off: kmain32(boot_info_phys).
    push esi
    call kmain32

.hang:
    hlt
    jmp  .hang
