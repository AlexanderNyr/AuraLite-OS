; w32a4_continue.asm — EXCEPTION_CONTINUE_EXECUTION with a mended
; context.  W32A-4.
;
; The guarded store faults (RAX is NULL); the filter writes the valid
; address into the fault CONTEXT's RAX slot and returns CONTINUE, so the
; re-executed store succeeds.  This proves the resume uses the MUTATED
; context, not a copy: without the mend, the retry would fault forever.
; CONTEXT.RAX sits at offset 0x78 (w32/w32_seh.h pins it).

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern __C_specific_handler

%define STD_OUTPUT_HANDLE -11
%define ACCESS_VIOLATION 0xC0000005
%define CTX_RAX 0x78

section .rdata
msg_ok:   db "W32A4-CONTINUE-OK", 10
msg_ok_l  equ $ - msg_ok
msg_bad:  db "W32A4-CONTINUE-BROKEN", 10
msg_bad_l equ $ - msg_bad

section .bss
written:  resq 1
stdout_h: resq 1
flag_cell: resq 1

section .text

puts_raw:
    push rbp
    mov  rbp, rsp
    sub  rsp, 40h
    mov  rcx, [stdout_h]
    mov  rdx, rsi
    mov  r8d, edi
    lea  r9, [written]
    mov  qword [rsp+20h], 0
    call WriteFile
    add  rsp, 40h
    pop  rbp
    ret
puts_raw_end:

seh_personality:
    jmp  __C_specific_handler

guarded:
    push rbp
    sub  rsp, 20h
.try_begin:
    xor  eax, eax
    mov  dword [rax], 1         ; faults: write through NULL
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
guarded_end:

filter_continue:                ; (EXCEPTION_POINTERS*, establisher)
    mov  rax, [rcx]             ; record
    cmp  dword [rax], ACCESS_VIOLATION
    jne  .search
    mov  rdx, [rcx+8]           ; context
    lea  rax, [flag_cell]
    mov  [rdx+CTX_RAX], rax     ; mend RAX: the retry stores here
    mov  eax, -1                ; CONTINUE_EXECUTION
    ret
.search:
    xor  eax, eax
    ret

global start
start:
    push rbp
    sub  rsp, 20h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax
    call guarded
    cmp  qword [flag_cell], 1
    jne  .broken
    lea  rsi, [msg_ok]
    mov  edi, msg_ok_l
    call puts_raw
    mov  ecx, 44
    call ExitProcess
    hlt
.broken:
    lea  rsi, [msg_bad]
    mov  edi, msg_bad_l
    call puts_raw
    mov  ecx, 45
    call ExitProcess
    hlt
start_end:

section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd guarded, guarded_end, guarded_xdata
    dd start, start_end, start_xdata

section .xdata
puts_raw_xdata:
    db 0x01, 8, 3, 0x05
    db 8, 0x72
    db 4, 0x03
    db 1, 0x50
    db 0, 0

guarded_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd guarded.try_begin, guarded.try_end
    dd filter_continue, guarded.try_end

start_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
