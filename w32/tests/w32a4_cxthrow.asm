; w32a4_cxthrow.asm — the MSVC-shaped C++ throw.  W32A-4.
;
; Two frames with cleanups; the inner calls the imported msvcrt
; _CxxThrowException.  The sweep runs both cleanups in order, then the
; process terminates with W32-CXX-TYPED-CATCH-GAP (D7: catch clauses are
; never matched).  The gate asserts the order, the message, and that the
; post-call line never prints.

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern __C_specific_handler
extern _CxxThrowException

%define STD_OUTPUT_HANDLE -11

section .rdata
msg_inner: db "W32A4-CXX-CLEANUP inner", 10
msg_inner_l equ $ - msg_inner
msg_outer: db "W32A4-CXX-CLEANUP outer", 10
msg_outer_l equ $ - msg_outer
msg_surv:  db "W32A4-SURVIVED", 10
msg_surv_l equ $ - msg_surv

section .data
throw_obj:   dq 0x0badc0de
throw_info:  dq 0, 0, 0, 0      ; dummy ThrowInfo (accepted, ignored)

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

seh_personality:
    jmp  __C_specific_handler

f_inner:
    push rbp
    sub  rsp, 20h
.try_begin:
    lea  rcx, [throw_obj]
    lea  rdx, [throw_info]
    call _CxxThrowException     ; NORETURN (sweep + named death)
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
f_inner_end:

cleanup_inner:
    push rsi                      ; Win64 callee-saved (C-called)
    push rdi
    sub  rsp, 40
    lea  rsi, [msg_inner]
    mov  edi, msg_inner_l
    call puts_raw
    add  rsp, 40
    pop  rdi
    pop  rsi
    ret
cleanup_inner_end:

f_outer:
    push rbp
    sub  rsp, 20h
.try_begin:
    call f_inner
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
f_outer_end:

cleanup_outer:
    push rsi                      ; Win64 callee-saved (C-called)
    push rdi
    sub  rsp, 40
    lea  rsi, [msg_outer]
    mov  edi, msg_outer_l
    call puts_raw
    add  rsp, 40
    pop  rdi
    pop  rsi
    ret
cleanup_outer_end:

global start
start:
    push rbp
    sub  rsp, 20h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax
    call f_outer
    lea  rsi, [msg_surv]        ; must never print
    mov  edi, msg_surv_l
    call puts_raw
    mov  ecx, 44
    call ExitProcess
    hlt
start_end:

section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd f_inner, f_inner_end, f_inner_xdata
    dd cleanup_inner, cleanup_inner_end, cleanup_inner_xdata
    dd f_outer, f_outer_end, f_outer_xdata
    dd cleanup_outer, cleanup_outer_end, cleanup_outer_xdata
    dd start, start_end, start_xdata

section .xdata
puts_raw_xdata:
    db 0x01, 8, 3, 0x05
    db 8, 0x72
    db 4, 0x03
    db 1, 0x50
    db 0, 0

f_inner_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd f_inner.try_begin, f_inner.try_end
    dd 1, cleanup_inner

cleanup_inner_xdata:
    db 0x01, 6, 3, 0x00
    db 6, 0x42
    db 2, 0x70
    db 1, 0x60
    db 0, 0

f_outer_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd f_outer.try_begin, f_outer.try_end
    dd 1, cleanup_outer

cleanup_outer_xdata:
    db 0x01, 6, 3, 0x00
    db 6, 0x42
    db 2, 0x70
    db 1, 0x60
    db 0, 0

start_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
