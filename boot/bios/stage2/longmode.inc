; =============================================================================
; boot/bios/stage2/longmode.inc -- transition real mode -> long mode.
;
; Sequence (Intel SDM Vol.3 s.9.8.5):
;   1. Disable interrupts, load a 64-bit GDT (already-flat entries).
;   2. Load CR3 with the PML4 physical address built by build_page_tables.
;   3. Set CR4.PAE (bit 5) and CR4.PGE (bit 7, optional but harmless).
;   4. Set IA32_EFER.LME (bit 8, MSR 0xC0000080) and NXE (bit 11).
;   5. Set CR0.PE (bit 0) and CR0.PG (bit 31) atomically.  Once PG
;      is set, the CPU is in IA-32e compatibility mode.
;   6. Far-jump into a 64-bit code segment; the L bit in the descriptor
;      triggers 64-bit mode on the far jump.
;   7. Reload data segments (any 64-bit data selector works; we use
;      the null selector for FS/GS and the flat data selector for
;      DS/ES/SS).
;   8. Set up RSP to a low-memory scratch stack -- kernel replaces it
;      immediately anyway.
;   9. RDI = pointer to boot_info_t.  Far-jump / indirect-jump to the
;      kernel entry point at [elf_entry_addr].
;
; This routine NEVER returns.
; =============================================================================

; Global-descriptor table.  Three entries: null, 64-bit code, 64-bit data.
; The 64-bit code selector has the L bit set (bit 21 of the second dword).
align 8
gdt64_start:
    dq  0x0000000000000000                  ; 0x00 null
    dq  0x00AF9A000000FFFF                  ; 0x08 code64 (L=1, R/E, DPL=0)
    dq  0x00AF92000000FFFF                  ; 0x10 data64 (R/W, DPL=0)
gdt64_end:

gdt64_ptr:
    dw  gdt64_end - gdt64_start - 1
    dd  gdt64_start                         ; 32-bit linear base (Stage 2
                                            ;   sits in the first MiB, so
                                            ;   the low 32 bits suffice
                                            ;   and the upper 16 bits are
                                            ;   ignored by 32-bit LGDT).

; ---------------------------------------------------------------------------
; enter_long_mode -- takes over the CPU permanently.
;
; On entry: unreal mode active, page tables built at PT_BASE,
;           boot_info_t filled at BOOT_INFO_PHYS,
;           [elf_entry_addr] holds the kernel virtual entry point.
; On exit : does not return.
; ---------------------------------------------------------------------------
enter_long_mode:
    cli

    ; Load the 64-bit GDT (three entries: null / code64-L / data64).
    lgdt [gdt64_ptr]

    ; Load CR3 with the PML4 physical address built by
    ; build_page_tables (paging.inc).
    mov  eax, PML4_ADDR
    mov  cr3, eax

    ; Enable PAE (CR4.PAE = bit 5).  Required for long mode.
    mov  eax, cr4
    or   eax, (1 << 5)
    mov  cr4, eax

    ; Set IA32_EFER.LME (bit 8) and NXE (bit 11).  MSR 0xC0000080.
    mov  ecx, 0xC0000080
    rdmsr
    or   eax, (1 << 8)                     ; LME
    or   eax, (1 << 11)                    ; NXE
    wrmsr

    ; Turn on paging (CR0.PG = bit 31) and confirm PE (bit 0).
    ; The very next instruction fetched from CS:IP is decoded per the
    ; new mode -- but CS is still a 16-bit selector, so the CPU is in
    ; IA-32e compatibility mode.  The far jump below switches to a
    ; 64-bit code segment (L bit set) which activates true 64-bit
    ; operation.
    mov  eax, cr0
    or   eax, (1 << 31) | (1 << 0)
    mov  cr0, eax

    ; Far-jump into 64-bit code selector 0x08 whose L bit is set.
    jmp  0x08:.long64

bits 64
.long64:
    ; Reload segment registers with the 64-bit data selector.
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    xor  ax, ax                             ; null selectors work for FS/GS
    mov  fs, ax
    mov  gs, ax

    ; Temporary stack; the kernel's boot.asm replaces it immediately.
    mov  rsp, 0x7C00

    ; RDI = physical pointer to boot_info_t (SysV AMD64 arg 0).  The
    ; kernel accesses it through the identity map (0..1 GiB) before
    ; installing its own page tables.
    mov  rdi, BOOT_INFO_PHYS

    ; Jump to the kernel entry point (elf_entry_addr = e_entry).
    ; The address stored at [abs elf_entry_addr] is the higher-half
    ; virtual address of `_start`; PML4[511]->...PD511[0..1] map it.
    mov  rax, [abs elf_entry_addr]
    jmp  rax
    ; unreachable

bits 16
