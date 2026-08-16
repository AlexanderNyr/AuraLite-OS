; =============================================================================
; kernel/arch/i386/gdt_flush32.asm -- LGDT + segment reload + LTR
; (I386_PLAN I2).  Sibling of kernel/arch/x86_64/gdt_flush.asm.
;
; cdecl: the single argument arrives on the stack at [esp+4].
; =============================================================================

bits 32

section .text

global gdt_flush32
gdt_flush32:
    mov  eax, [esp + 4]
    lgdt [eax]

    ; Reload data segments with the kernel data selector.
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Far jump to reload CS.
    jmp  0x08:.flush
.flush:
    ret

global tss_flush32
tss_flush32:
    mov  eax, [esp + 4]
    ltr  ax
    ret
