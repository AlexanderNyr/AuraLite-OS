; =============================================================================
; gdt_flush.asm — atomically load a new GDT and reload every segment register.
;
; C prototype: void gdt_flush(uint64_t gdtr_ptr);
;   rdi -> struct gdt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));
;
; Reloading CS in long mode requires a far control-transfer; we use a far return
; (retfq) that pops {RIP, CS} off the stack.
; =============================================================================

bits 64
default rel

section .text

AURALITE_SEL_NULL  equ 0x00
AURALITE_SEL_CODE  equ 0x08        ; GDT entry 1
AURALITE_SEL_DATA  equ 0x10        ; GDT entry 2

global gdt_flush
gdt_flush:
    lgdt [rdi]                 ; load the GDT register from the descriptor

    mov ax, AURALITE_SEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS via far return: push new CS, then push target RIP, then retfq.
    push AURALITE_SEL_CODE
    lea rax, [.reload_cs]
    push rax
    retfq                      ; RIP <- [.reload_cs], CS <- AURALITE_SEL_CODE

.reload_cs:
    ret
