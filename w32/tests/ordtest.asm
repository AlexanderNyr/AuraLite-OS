; w32/tests/ordtest.asm — W32APP_PLAN.md phase W32A-1 fixture.
;
; Imports five functions BY ORDINAL (no names in the ILT -- see the
; *_ord.def files) and proves each binds to the right thing:
;
;   comctl32 #17  InitCommonControls      (TODO stub: sets ERROR_NOT_SUPPORTED)
;   comctl32 #381 LoadIconWithScaleDown   (TODO stub: returns E_NOTIMPL)
;   oleaut32 #2   SysAllocString          (REAL: BSTR round-trip)
;   oleaut32 #7   SysStringLen            (REAL: length 2 for "OK")
;   oleaut32 #6   SysFreeString           (REAL: frees it)
;
; Then the same-table alias: GetProcAddress(comctl32, "InitCommonControls")
; and GetProcAddress(comctl32, 17) must return the SAME address.
;
; Prints ORD-OK and returns 0.  Any failure prints ORD-FAIL and exits 3,
; so the integration case tells "fixture failed" from "loader refused".

bits 64
default rel

extern InitCommonControls
extern LoadIconWithScaleDown
extern SysAllocString
extern SysStringLen
extern SysFreeString
extern GetLastError
extern GetModuleHandleA
extern GetProcAddress
extern GetStdHandle
extern WriteFile
extern ExitProcess

section .text

global start
start:
    push rbx
    push r12
    sub rsp, 38h                    ; (8-16-56) % 16 == 0, like testdll

    ; comctl32 #17 through the ordinal: the TODO stub runs (no crash) and
    ; reports NOT_SUPPORTED, which is the honest "not yet".
    call InitCommonControls
    call GetLastError
    cmp eax, 50
    jne fail

    ; oleaut32 #2/#7/#6: a real BSTR round-trip through ordinal bindings.
    lea rcx, [u_ok]
    call SysAllocString
    test rax, rax
    jz fail
    mov rbx, rax
    mov rcx, rax
    call SysStringLen
    cmp eax, 2
    jne fail
    mov rcx, rbx
    call SysFreeString

    ; comctl32 #381: an HRESULT stub fails with E_NOTIMPL, never S_OK.
    xor ecx, ecx
    xor edx, edx
    xor r8d, r8d
    xor r9d, r9d
    call LoadIconWithScaleDown
    cmp eax, 80004001h
    jne fail

    ; The alias: name and number answer with one address.
    lea rcx, [s_comctl32]
    call GetModuleHandleA
    test rax, rax
    jz fail
    mov r12, rax
    mov rcx, rax
    lea rdx, [s_init]
    call GetProcAddress
    test rax, rax
    jz fail
    mov rbx, rax
    mov rcx, r12
    mov rdx, 17
    call GetProcAddress
    cmp rax, rbx
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

; puts(rcx = NUL string).  Same frame as testdll's dll_puts.
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
msg_ok:      db "ORD-OK", 13, 10, 0
msg_fail:    db "ORD-FAIL", 13, 10, 0
s_comctl32:  db "comctl32", 0
s_init:      db "InitCommonControls", 0
u_ok:        dw 'O', 'K', 0
