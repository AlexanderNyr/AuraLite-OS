; w32a4_crash.asm — the unguarded fault.  W32A-4.
;
; Divides by zero with no __try and no filter: the process must die on
; SIGFPE (exit code 136, the [signal] line), exactly like the old sehtest
; receipt.  The unwinder's serial dump (W32-SEH-UNHANDLED) accompanies it.

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess

%define STD_OUTPUT_HANDLE -11

section .rdata
msg_fault: db "SEH-UNGUARDED-FAULT", 10
msg_fault_l equ $ - msg_fault
msg_surv:  db "SEH-SURVIVED-UNGUARDED", 10
msg_surv_l equ $ - msg_surv

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

global start
start:
    push rbp
    sub  rsp, 20h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax
    lea  rsi, [msg_fault]
    mov  edi, msg_fault_l
    call puts_raw
    xor  ecx, ecx
    mov  eax, 1
    xor  edx, edx
    div  ecx                    ; #DE, unguarded: SIGFPE death
    lea  rsi, [msg_surv]        ; NOTREACHED
    mov  edi, msg_surv_l
    call puts_raw
    mov  ecx, 44
    call ExitProcess
    hlt
start_end:

section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd start, start_end, start_xdata

section .xdata
puts_raw_xdata:
    db 0x01, 8, 3, 0x05
    db 8, 0x72
    db 4, 0x03
    db 1, 0x50
    db 0, 0

start_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
