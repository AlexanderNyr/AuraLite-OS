; =============================================================================
; isr.asm — 256 IDT entry-point stubs + the common save/restore handler.
;
; Each stub normalises the stack to: [int_no][err_code], then jumps to the common
; handler, which pushes all general registers, calls isr_handler(regs*) in C,
; restores them, drops the two header words, and returns with iretq.
;
; Stack frame layout on entry to isr_handler (low -> high address):
;   r15 r14 r13 r12 r11 r10 r9 r8 rdi rsi rbp rdx rcx rbx rax
;   int_no err_code  rip cs rflags rsp ss
; =============================================================================

bits 64
default rel

section .rodata
align 8
global isr_table

; 256-entry table of handler addresses, indexed by vector number, used by
; idt.c to populate the IDT. Built via a macro so plain %1 substitution forms
; the symbol names isr0 .. isr255 (no fragile token-pasting).
%macro TABLE_ENTRY 1
    dq isr%1
%endmacro

isr_table:
%assign i 0
%rep 256
    TABLE_ENTRY i
%assign i i+1
%endrep


section .text
extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0          ; dummy error code -> uniform frame
    push qword %1         ; vector number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1         ; CPU already pushed the error code
    jmp isr_common_stub
%endmacro

isr_common_stub:
    ; Save all general-purpose registers. First push ends highest; the struct
    ; in isr.h lists them in the opposite (low-first) order to match.
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp          ; System V AMD64: arg0 = pointer to registers
    cld                   ; ABI requires the direction flag clear
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16           ; discard err_code + int_no
    iretq

; ---- CPU exceptions 0-31 ----
; Vectors that push an error code: 8, 10, 11, 12, 13, 14, 17 (Intel SDM 3A 6-13).
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- Hardware IRQs 32-47 and remaining vectors 48-255 (all no error code) ----
%assign i 32
%rep 224
    ISR_NOERR i
%assign i i+1
%endrep
