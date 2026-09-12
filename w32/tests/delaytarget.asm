; w32/tests/delaytarget.asm — W32A-1 fixture: a DLL that exists to be
; delay-loaded.  Its DllMain announces attach so the log shows the load
; happening at first call, not at exe bind time.
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
    cmp edx, 1                       ; DLL_PROCESS_ATTACH?
    jne .done
    lea rcx, [msg_attach]
    call puts
.done:
    mov eax, 1
    add rsp, 38h
    pop r12
    pop rbx
    ret

global delayed_add
delayed_add:                         ; (ecx, edx) -> eax = ecx + edx
    lea eax, [ecx+edx]
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
msg_attach: db "DELAYTARGET-ATTACH", 13, 10, 0
