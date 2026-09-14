; w32a4_unwind.asm — __finally order on the unwind pass.  W32A-4.
;
; f1 -> f2 -> f3; f3 divides by zero.  f1's filter EXECUTEs, so the second
; pass runs f3's then f2's cleanups and stops AT f1 (the target keeps its
; frame: f1's own cleanup must NOT run -- the gate asserts its absence).
; Receipt order on stdout is the assertion: f3's line precedes f2's.

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern __C_specific_handler

%define STD_OUTPUT_HANDLE -11
%define INT_DIVIDE_BY_ZERO 0xC0000094

section .rdata
msg_f3:   db "W32A4-FINALLY f3", 10
msg_f3_l  equ $ - msg_f3
msg_f2:   db "W32A4-FINALLY f2", 10
msg_f2_l  equ $ - msg_f2
msg_f1:   db "W32A4-FINALLY f1", 10
msg_f1_l  equ $ - msg_f1
msg_ok:   db "W32A4-UNWIND-OK", 10
msg_ok_l  equ $ - msg_ok

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

; --- f3: faults inside its guarded range -------------------------------------
f3:
    push rbp
    sub  rsp, 20h
.try_begin:
    xor  ecx, ecx
    mov  eax, 1
    xor  edx, edx
    div  ecx                    ; #DE
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
f3_end:

cleanup_f3:                     ; void f(establisher)
    push rsi                      ; Win64 callee-saved (C-called)
    push rdi
    sub  rsp, 40
    lea  rsi, [msg_f3]
    mov  edi, msg_f3_l
    call puts_raw
    add  rsp, 40
    pop  rdi
    pop  rsi
    ret
cleanup_f3_end:

; --- f2: calls f3 inside its guarded range -----------------------------------
f2:
    push rbp
    sub  rsp, 20h
.try_begin:
    call f3
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
f2_end:

cleanup_f2:
    push rsi                      ; Win64 callee-saved (C-called)
    push rdi
    sub  rsp, 40
    lea  rsi, [msg_f2]
    mov  edi, msg_f2_l
    call puts_raw
    add  rsp, 40
    pop  rdi
    pop  rsi
    ret
cleanup_f2_end:

; --- f1: the catcher (filter scope + finally scope, same range) --------------
f1:
    push rbp
    sub  rsp, 20h
.try_begin:
    call f2
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
f1_end:

filter_f1:
    mov  rax, [rcx]
    cmp  dword [rax], INT_DIVIDE_BY_ZERO
    jne  .search
    mov  eax, 1
    ret
.search:
    xor  eax, eax
    ret

cleanup_f1:                     ; must NEVER run (f1 is the target)
    push rsi                      ; Win64 callee-saved (C-called)
    push rdi
    sub  rsp, 40
    lea  rsi, [msg_f1]
    mov  edi, msg_f1_l
    call puts_raw
    add  rsp, 40
    pop  rdi
    pop  rsi
    ret
cleanup_f1_end:

except_f1:
    ; Entry rsp is the catcher's LIVE rsp (post-prologue, call-ready):
    ; no shadow alloc (the dead frame below is scratch).  On exit restore
    ; the 40-byte frame (push rbp + sub 20h) and ret to the catcher's caller.
    lea  rsi, [msg_ok]
    mov  edi, msg_ok_l
    call puts_raw
    add  rsp, 40
    ret
except_f1_end:

global start
start:
    push rbp
    sub  rsp, 20h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax
    call f1
    mov  ecx, 44
    call ExitProcess
    hlt
start_end:

section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd f3, f3_end, f3_xdata
    dd cleanup_f3, cleanup_f3_end, cleanup_f3_xdata
    dd f2, f2_end, f2_xdata
    dd cleanup_f2, cleanup_f2_end, cleanup_f2_xdata
    dd f1, f1_end, f1_xdata
    dd cleanup_f1, cleanup_f1_end, cleanup_f1_xdata
    dd except_f1, except_f1_end, except_f1_xdata
    dd start, start_end, start_xdata

section .xdata
puts_raw_xdata:
    db 0x01, 8, 3, 0x05
    db 8, 0x72
    db 4, 0x03
    db 1, 0x50
    db 0, 0

f3_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd f3.try_begin, f3.try_end
    dd 1, cleanup_f3            ; pure cleanup (HandlerAddress == 1)

cleanup_f3_xdata:
    db 0x01, 6, 3, 0x00
    db 6, 0x42
    db 2, 0x70
    db 1, 0x60
    db 0, 0

f2_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd f2.try_begin, f2.try_end
    dd 1, cleanup_f2

cleanup_f2_xdata:
    db 0x01, 6, 3, 0x00
    db 6, 0x42
    db 2, 0x70
    db 1, 0x60
    db 0, 0

f1_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 2                        ; filter (outer) + cleanup (inner)
    dd f1.try_begin, f1.try_end
    dd filter_f1, except_f1
    dd f1.try_begin, f1.try_end
    dd 1, cleanup_f1

cleanup_f1_xdata:
    db 0x01, 6, 3, 0x00
    db 6, 0x42
    db 2, 0x70
    db 1, 0x60
    db 0, 0

except_f1_xdata:
    db 0x01, 0, 0, 0x00

start_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
