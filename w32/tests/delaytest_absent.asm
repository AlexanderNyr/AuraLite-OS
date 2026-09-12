; w32/tests/delaytest_absent.asm — W32A-1 fixture: delay target absent.
;
; The delay directory must NOT fail the load: this exe binds and starts
; fine, and only the first call through the thunk fails -- with the DLL
; named (DELAY-LOADFAIL nosuchdll.dll) and exit code 77.  That is the
; documented Windows behaviour, not a load refusal.

bits 64
default rel

extern NoSuchFunc
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

    call NoSuchFunc                   ; helper fails: names DLL, exits 77

    ; Unreached: the helper terminates.  If it ever returns, that is the
    ; failure -- a failed delay load must not resume.
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
msg_start: db "DELAYTEST-ABSENT-START", 13, 10, 0
msg_fail:  db "DELAY-FAIL", 13, 10, 0
