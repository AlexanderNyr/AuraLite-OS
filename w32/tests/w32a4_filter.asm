; w32a4_filter.asm — SetUnhandledExceptionFilter.  W32A-4.
;
; Installs a top filter, then divides by zero with no __try active.  The
; filter prints SEH-FILTER-CALLED and returns EXECUTE, so the process
; exits with the exception code instead of dying on the signal.  (The old
; sehtest receipt strings, unchanged.)

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern SetUnhandledExceptionFilter

%define STD_OUTPUT_HANDLE -11

section .rdata
msg_called: db "SEH-FILTER-CALLED", 10
msg_called_l equ $ - msg_called
msg_surv:   db "SEH-SURVIVED-UNGUARDED", 10
msg_surv_l  equ $ - msg_surv

section .bss
written:  resq 1
stdout_h: resq 1

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

top_filter:                     ; (EXCEPTION_POINTERS*) -> EXECUTE
    push rbp                      ; (rsi/rdi saved: Win64 callee-saved)
    push rsi
    push rdi
    sub  rsp, 20h
    lea  rsi, [msg_called]
    mov  edi, msg_called_l
    call puts_raw
    mov  eax, 1                 ; EXCEPTION_EXECUTE_HANDLER
    add  rsp, 20h
    pop  rdi
    pop  rsi
    pop  rbp
    ret
top_filter_end:

global start
start:
    push rbp
    sub  rsp, 20h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax
    lea  rcx, [top_filter]
    call SetUnhandledExceptionFilter
    ; Unguarded divide by zero: the top filter owns it now.
    xor  ecx, ecx
    mov  eax, 1
    xor  edx, edx
    div  ecx
    ; NOTREACHED
    lea  rsi, [msg_surv]
    mov  edi, msg_surv_l
    call puts_raw
    mov  ecx, 44
    call ExitProcess
    hlt
start_end:

section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd top_filter, top_filter_end, top_filter_xdata
    dd start, start_end, start_xdata

section .xdata
puts_raw_xdata:
    db 0x01, 8, 3, 0x05
    db 8, 0x72
    db 4, 0x03
    db 1, 0x50
    db 0, 0

top_filter_xdata:
    db 0x01, 7, 4, 0x00
    db 7, 0x32
    db 3, 0x70
    db 2, 0x60
    db 1, 0x50

start_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
