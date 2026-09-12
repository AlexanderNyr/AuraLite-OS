; w32/tests/datamain.asm — W32A-1 fixture: reads an imported DWORD.
;
; `answer` is a DATA export of datadll.dll.  The import address table slot
; holds its ADDRESS (not code), so the read is two hops: the __imp_ slot
; gives the address, the address gives the DWORD.  MSVC spells this for C
; as __declspec(dllimport); in nasm the __imp_ name is spelled outright.
; Expected value: 42.  data_fn (-> 7) proves code exports beside it work.

bits 64
default rel

extern __imp_answer
extern data_fn
extern GetStdHandle
extern WriteFile
extern ExitProcess

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h

    mov rax, [rel __imp_answer]
    cmp dword [rax], 42
    jne fail

    call data_fn
    cmp eax, 7
    jne fail

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
msg_ok:   db "DATA-OK", 13, 10, 0
msg_fail: db "DATA-FAIL", 13, 10, 0
