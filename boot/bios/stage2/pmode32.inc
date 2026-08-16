; =============================================================================
; boot/bios/stage2/pmode32.inc -- transition real mode -> 32-bit protected
; mode and jump to the i386 kernel (I386_PLAN I1).
;
; This is the 32-bit sibling of longmode.inc.  The hand-off contract is
; the same boot_info_t, differently delivered:
;
;   64-bit path: boot_info_t phys in RDI  (System V AMD64 argument 0)
;   32-bit path: boot_info_t phys in ESI  (register parameter; the i386
;                stub's boot32.asm pushes it for the cdecl C entry)
;
; ESI rather than the i386 cdecl stack because there IS no C stack yet at
; the jump -- the kernel's own entry code sets one up, exactly as the
; 64-bit _start does, and a register survives that; a stack argument
; would have to be re-pushed anyway.
;
; Sequence (Intel SDM Vol.3 s.9.9.1):
;   1. cli, load a flat 32-bit GDT (null / code32 / data32).
;   2. CR0.PE = 1.
;   3. Far-jump into the 32-bit code segment (serialises, loads CS).
;   4. Reload data segments with the flat data selector.
;   5. ESP to a low-memory scratch stack (kernel replaces it).
;   6. ESI = boot_info_t phys, jump to [elf32_entry_addr].
;
; No paging is enabled here -- the i386 kernel owns that (phase I3), the
; same division of labour the original kernel had before BL4 moved page
; tables into the loader.  This routine NEVER returns.
; =============================================================================

; Flat 32-bit GDT.  Base 0, limit 4 GiB (G=1), D=1.
align 8
gdt32_start:
    dq  0x0000000000000000                  ; 0x00 null
    dq  0x00CF9A000000FFFF                  ; 0x08 code32 (D=1, R/E, DPL=0)
    dq  0x00CF92000000FFFF                  ; 0x10 data32 (R/W, DPL=0)
gdt32_end:

gdt32_ptr:
    dw  gdt32_end - gdt32_start - 1
    dd  gdt32_start

; ---------------------------------------------------------------------------
; enter_prot32 -- takes over the CPU permanently.
;
; On entry: unreal mode active (A20 on, real-mode segments),
;           boot_info_t filled at BOOT_INFO_PHYS,
;           [elf32_entry_addr] holds the kernel32 physical entry point.
; On exit : does not return.
; ---------------------------------------------------------------------------
enter_prot32:
    cli
    lgdt [gdt32_ptr]

    mov  eax, cr0
    or   eax, (1 << 0)                     ; CR0.PE
    mov  cr0, eax

    ; Far jump to flush the prefetch queue and load the 32-bit CS.
    jmp  dword 0x08:.pm32

bits 32
.pm32:
    mov  ax, 0x10                          ; flat data selector
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Scratch stack just under the EBDA floor; the kernel entry
    ; replaces it with its own .bss stack immediately.
    mov  esp, 0x0009F000

    ; Hand-off: ESI = boot_info_t physical address.
    mov  esi, BOOT_INFO_PHYS

    ; Indirect jump to the ELF32 entry point.
    mov  eax, [elf32_entry_addr]
    jmp  eax

bits 16
