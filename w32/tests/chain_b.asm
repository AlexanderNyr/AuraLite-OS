; w32/tests/chain_b.asm — W32A-1 fixture: bottom of the chain.
; Exports chain_b_fn (-> 7); announces attach and detach for the order gate.
bits 64
default rel

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

global chain_b_fn
chain_b_fn:
    mov eax, 7
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
msg_attach: db "CHAIN-B-ATTACH", 13, 10, 0
msg_detach: db "CHAIN-B-DETACH", 13, 10, 0
