; w32/tests/fwdstatic.asm — W32A-1 fixture: the STATIC forwarder path.
; A static import of the forwarded FwdFunc must refuse at bind: the
; refusal names the target ("forwarder exports are not supported (target
; kernel32.GetTickCount64)") and the bind reports "w32run: unresolved
; import fwddll.dll!FwdFunc" with exit 1.  start must never run.

bits 64
default rel

extern FwdFunc
extern GetStdHandle
extern WriteFile
extern ExitProcess

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h
    call FwdFunc
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
msg_fail: db "FWD-FAIL", 13, 10, 0
