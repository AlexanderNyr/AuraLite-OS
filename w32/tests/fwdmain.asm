; w32/tests/fwdmain.asm — W32A-1 fixture: the DYNAMIC forwarder path.
;
; fwdtest.dll is loaded at runtime (never imported statically -- a static
; import would refuse at bind, which is fwdstatic's case).  GetProcAddress
; of the forwarded FwdFunc must return NULL with GetLastError() == 127
; (ERROR_PROC_NOT_FOUND), and the refusal on stdout must name the target:
; "forwarder exports are not supported (target
; kernel32.GetTickCount64)".  FWD-OK and 0 on success.

bits 64
default rel

extern LoadLibraryA
extern GetProcAddress
extern GetLastError
extern FreeLibrary
extern GetStdHandle
extern WriteFile
extern ExitProcess

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h

    lea rcx, [s_fwdtest]
    call LoadLibraryA
    test rax, rax
    jz fail
    mov rbx, rax

    mov rcx, rax
    lea rdx, [s_fwdfunc]
    call GetProcAddress
    test rax, rax
    jnz fail

    call GetLastError
    cmp eax, 127
    jne fail

    mov rcx, rbx
    call FreeLibrary
    test eax, eax
    jz fail

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
msg_ok:    db "FWD-OK", 13, 10, 0
msg_fail:  db "FWD-FAIL", 13, 10, 0
s_fwdtest:  db "fwdtest.dll", 0
s_fwdfunc: db "FwdFunc", 0
