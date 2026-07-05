; =============================================================================
; boot/bios/stage2/unreal.inc -- switch the CPU into "unreal mode".
;
; Unreal mode = real-mode CS/IP semantics but with the *hidden* base/
; limit of one or more data segment registers set to a flat 4 GiB
; view.  The trick relies on the fact that a segment register's hidden
; descriptor cache is only reloaded when the register is written -- so
; if we (1) load a big-flat descriptor in protected mode, (2) load that
; descriptor into FS/GS, (3) drop back to real mode, and (4) never
; touch FS/GS again with a real-mode "mov fs, ax" instruction, then FS
; and GS keep their flat cached descriptors while CS/DS/SS/ES stay
; ordinary real-mode segments.
;
; After go_unreal() returns, real-mode code can do:
;
;     mov  edi, 0x00100000              ; 32-bit linear address
;     mov  al,  [byte fs:edi]           ; 32-bit displacement + FS = flat!
;
; and reach any physical byte below 4 GiB.  Instructions with 32-bit
; addressing require an operand-size / address-size prefix, which nasm
; emits automatically when the address expression exceeds 16 bits.
;
; This routine is idempotent -- calling it a second time simply reloads
; the same descriptors.
; =============================================================================

; go_unreal -- lift FS and GS into a 32-bit flat view.
; Preserves every general-purpose register and every segment register
; that a real-mode caller cares about (CS, DS, SS, ES).  Interrupts
; are masked across the CR0 toggle and the caller's IF is restored.
go_unreal:
    pushf
    pusha
    push ds
    push es
    cli

    ; The GDTR base is a 32-bit linear address.  Because we ORG at
    ; 0x8000 and were loaded there physically, "unreal_gdt" evaluates
    ; to that same physical address at assembly time -- perfect.
    lgdt [unreal_gdtr]

    ; Enter protected mode (CR0.PE = 1) just long enough to reload the
    ; hidden caches of FS/GS.
    mov  eax, cr0
    or   al, 1
    mov  cr0, eax

    ; Loading FS/GS while in pmode caches the descriptor's base=0 /
    ; limit=0xFFFFFFFF into the hidden portion of the segment register.
    mov  bx, 0x10                       ; unreal_gdt[2] = flat data
    mov  fs, bx
    mov  gs, bx

    ; Leave protected mode.  CS/DS/SS/ES were never touched inside
    ; pmode, so their hidden caches still hold the real-mode 64 KiB
    ; view they had on entry.  FS and GS retain the flat 4 GiB view.
    mov  eax, cr0
    and  al, 0xFE
    mov  cr0, eax

    pop  es
    pop  ds
    popa
    popf
    ret

; ---------------------------------------------------------------------------
; Mini GDT: three entries -- null, 32-bit flat code, 32-bit flat data.
; The code descriptor is included for symmetry with BL4's full GDT but
; is not actually used by unreal mode.
; ---------------------------------------------------------------------------
align 8
unreal_gdt:
    dq  0x0000000000000000              ; 0x00  null
    dq  0x00CF9A000000FFFF              ; 0x08  code32: base=0, limit=4G-1
    dq  0x00CF92000000FFFF              ; 0x10  data32: base=0, limit=4G-1
unreal_gdt_end:

unreal_gdtr:
    dw  unreal_gdt_end - unreal_gdt - 1
    dd  unreal_gdt                      ; 32-bit linear GDT base
