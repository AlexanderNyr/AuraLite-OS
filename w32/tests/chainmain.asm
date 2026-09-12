; w32/tests/chainmain.asm — W32A-1 fixture: drives the two-DLL chain.
; Statically imports chain_a_fn (which pulls in chain_b.dll recursively),
; checks the composed result (7 + 1 = 8) and that both modules are loaded,
; then exits through ExitProcess so the detach order is observable too:
; B-ATTACH < A-ATTACH < A-DETACH < B-DETACH.

bits 64
default rel

extern chain_a_fn
extern GetModuleHandleA
extern GetStdHandle
extern WriteFile
extern ExitProcess

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h

    call chain_a_fn
    cmp eax, 8
    jne fail

    lea rcx, [s_a]
    call GetModuleHandleA
    test rax, rax
    jz fail
    lea rcx, [s_b]
    call GetModuleHandleA
    test rax, rax
    jz fail

    lea rcx, [msg_ok]
    call puts
    xor ecx, ecx
    call ExitProcess

fail:
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
msg_ok:   db "CHAIN-OK", 13, 10, 0
msg_fail: db "CHAIN-FAIL", 13, 10, 0
s_a:      db "chain_a.dll", 0
s_b:      db "chain_b.dll", 0
