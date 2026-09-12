; w32/tests/mantest.asm — W32A-1 fixture: the manifest is the test.
; One object, five executables: the Makefile links this same mantest.obj
; with four different .res files (v6, v5, admin, bad) plus once with no
; .res at all (none).  The body only proves "the gate let me run".
bits 64
default rel

extern GetStdHandle
extern WriteFile

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h
    lea rcx, [msg_ok]
    call puts
    xor eax, eax
    add rsp, 38h
    pop r12
    pop rbx
    ret

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
msg_ok: db "MAN-OK", 13, 10, 0
