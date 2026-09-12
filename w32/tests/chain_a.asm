; w32/tests/chain_a.asm — W32A-1 fixture: top of the chain.
; Imports chain_b_fn from chain_b.dll -- the recursive-load case -- and
; re-exports the composition as chain_a_fn (-> 8).
bits 64
default rel

extern chain_b_fn
extern GetStdHandle
extern WriteFile

section .text

global DllMain
DllMain:
    push rbx
    push r12
    sub rsp, 38h
    cmp edx, 1
    jne .not_attach
    lea rcx, [msg_attach]
    call puts
    jmp .done
.not_attach:
    cmp edx, 0
    jne .done
    lea rcx, [msg_detach]
    call puts
.done:
    mov eax, 1
    add rsp, 38h
    pop r12
    pop rbx
    ret

global chain_a_fn
chain_a_fn:
    push rbx
    sub rsp, 20h                      ; (8-8-32) % 16 == 0
    call chain_b_fn
    inc eax
    add rsp, 20h
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
msg_attach: db "CHAIN-A-ATTACH", 13, 10, 0
msg_detach: db "CHAIN-A-DETACH", 13, 10, 0
