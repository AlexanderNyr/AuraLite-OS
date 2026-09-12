; w32/tests/delaytest_present.asm — W32A-1 fixture: delay target present.
;
; Proves laziness, not just success: delaytarget.dll must NOT be loaded
; before the first call through the thunk (GetModuleHandleA returns NULL),
; and must be loaded after (the helper's LoadLibrary ran).  Then the call
; result (20 + 22 = 42) proves the patched slot points at real code.

bits 64
default rel

extern delayed_add
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

    lea rcx, [msg_start]
    call puts

    lea rcx, [s_target]
    call GetModuleHandleA
    test rax, rax
    jnz fail                         ; loaded already? not lazy.

    mov ecx, 20
    mov edx, 22
    call delayed_add
    cmp eax, 42
    jne fail

    lea rcx, [s_target]
    call GetModuleHandleA
    test rax, rax
    jz fail                          ; helper never loaded it?

    lea rcx, [msg_lazy]
    call puts
    lea rcx, [msg_ok]
    call puts
    xor eax, eax
    add rsp, 38h
    pop r12
    pop rbx
    ret

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
msg_start: db "DELAYTEST-START", 13, 10, 0
msg_lazy:  db "DELAY-LAZY-OK", 13, 10, 0
msg_ok:    db "DELAY-PRESENT-OK", 13, 10, 0
msg_fail:  db "DELAY-FAIL", 13, 10, 0
s_target:  db "delaytarget.dll", 0
