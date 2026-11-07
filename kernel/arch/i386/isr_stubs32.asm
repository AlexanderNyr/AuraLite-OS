; =============================================================================
; kernel/arch/i386/isr_stubs32.asm -- 256 interrupt entry stubs + dispatch
; trampoline (I386_PLAN I2).  Sibling of kernel/arch/x86_64/isr_stubs.asm.
;
; The CPU pushes an error code for vectors 8, 10-14, 17, 21 and nothing
; for the rest; the stubs normalise that by pushing a zero for the
; latter group, then push the vector number, save the register file,
; and call the C dispatcher with a pointer to the frame (cdecl: one
; stack argument, vs the x86_64 RDI convention).
;
; Frame layout handed to C (struct registers32 in isr.h, top down):
;   [pusha: edi esi ebp esp_dummy ebx edx ecx eax]
;   [ds]
;   [vector] [error_code]
;   [eip cs eflags]  (+ useresp ss when arriving from Ring 3)
; =============================================================================

bits 32

section .text
extern isr_dispatch32

; ---- common trampoline ------------------------------------------------------
isr_common:
    pusha                       ; edi..eax (8 dwords)

    xor  eax, eax
    mov  ax, ds
    push eax                    ; saved DS

    mov  ax, 0x10               ; kernel data
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    ; RESIDUE3 C2: the same dynamic realign the 64-bit isr_common_stub has
    ; carried since RESIDUE2 T1.  pusha + the saved DS + this argument leave
    ; ESP at an arbitrary 4-mod-16 phase (it depends on the interrupted ESP),
    ; so an aligned SSE spill in isr_dispatch32's callees would #GP.  EBP is
    ; already saved by pusha, so it can hold the frame pointer across the
    ; alignment; EAX is saved there too and carries the argument.
    mov  ebp, esp               ; remember the frame chain
    mov  eax, esp               ; cdecl arg: pointer to the frame
    and  esp, -16               ; 16-byte C-ABI alignment
    sub  esp, 16
    mov  [esp], eax
    call isr_dispatch32
    mov  esp, ebp               ; restore the frame chain

    pop  eax                    ; restore DS
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    popa
    add  esp, 8                 ; drop vector + error code
    iret

; ---- stub generators --------------------------------------------------------
%macro ISR_NOERR 1
isr_stub_%1:
    push dword 0                ; fake error code
    push dword %1               ; vector
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    ; CPU already pushed the error code
    push dword %1
    jmp  isr_common
%endmacro

; Vectors with a CPU-pushed error code: 8, 10-14, 17, 21 (SDM Vol.3 6-2).
%assign v 0
%rep 256
  %if (v == 8) || ((v >= 10) && (v <= 14)) || (v == 17) || (v == 21)
    ISR_ERR v
  %else
    ISR_NOERR v
  %endif
%assign v v+1
%endrep

; ---- the dispatch table consumed by idt.c -----------------------------------
section .rodata
global isr_table32
isr_table32:
%assign v 0
%rep 256
    dd isr_stub_%+v
%assign v v+1
%endrep
