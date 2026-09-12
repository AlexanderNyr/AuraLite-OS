; w32/tests/cycmain.asm — W32A-1 fixture: statically imports cyc_c_fn,
; pulling the cyc_c <-> cyc_d cycle into the load.  w32run must refuse
; with "dependency cycle: cyc_c.dll -> cyc_d.dll -> cyc_c.dll" (or the
; equivalent .dll-suffixed spelling); start must never run.

bits 64
default rel

extern cyc_c_fn
extern GetStdHandle
extern WriteFile
extern ExitProcess

section .text

global start
start:
    ; Reached only if the loader bound a cycle -- the failure marker.
    push rbx
    push r12
    sub rsp, 38h
    call cyc_c_fn
    lea rcx, [msg_fail]
    call puts
    mov ecx, 3
    call ExitProcess

puts:
    push rbx
    push r12
    sub rsp, 38h
    mov rbx, rcx
    mov ecx, -11
    call GetStdHandle
    mov r12, rax
    mov qword [rsp+20h], 0
.len:
    mov rax, [rsp+20h]
    cmp byte [rbx+rax], 0
    je .have
    inc qword [rsp+20h]
    jmp .len
.have:
    mov rcx, r12
    mov rdx, rbx
    mov r8d, [rsp+20h]
    lea r9, [rsp+28h]
    mov qword [rsp+28h], 0
    call WriteFile
    add rsp, 38h
    pop r12
    pop rbx
    ret

section .data
msg_fail: db "CYC-FAIL", 13, 10, 0
