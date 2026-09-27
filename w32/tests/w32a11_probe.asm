; W32A-11 independent PE: exercise the *generated TODO probe* through the
; real Win64 import binder. These identifiers are SYNTHETIC test vectors, not
; pinned-app CLSID/IID requests or a class activation table. Apache-2.0.
bits 64
default rel
extern CoInitialize
extern CoUninitialize
extern CoCreateInstance
extern CLSIDFromProgID
extern GetLastError
extern GetStdHandle
extern WriteFile
extern ExitProcess
%define E_NOTIMPL 0x80004001
%define ERROR_NOT_SUPPORTED 50
section .rdata
; GUID memory layout is little-endian for Data1/Data2/Data3.
clsid: dd 0x12345678
       dw 0x9abc,0xdef0
       db 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88
iid_one: dd 0
         dw 0,0
         db 0xc0,0,0,0,0,0,0,0x46
iid_two: times 16 db 0
; Includes a BMP code point, a surrogate pair, a backslash, newline,
; equals sign and space. The logger must escape it into ONE reversible line.
progid: dw 'N','P','P','.',0x00e9,0xd840,0xdc00,0x005c,0x000a,'=',' ',0
ok_msg: db 'A11-PROBE-FIXTURE-OK',13,10
ok_len equ $-ok_msg
fail_msg: db 'A11-PROBE-FIXTURE-FAIL',13,10
fail_len equ $-fail_msg
section .bss
stdout: resq 1
written: resd 1
result_ptr: resq 1
result_guid: resq 2
section .text
global winstart
%macro PRINT 2
    mov qword [rsp+32],0
    mov rcx,[stdout]
    lea rdx,[%1]
    mov r8d,%2
    lea r9,[written]
    call WriteFile
%endmacro
winstart:
    push rbx
    sub rsp,80 ; shadow + fifth arg, 16-byte aligned at every Win64 call
    mov ecx,-11
    call GetStdHandle
    mov [stdout],rax
    xor ecx,ecx
    call CoInitialize
    test eax,eax
    jnz fail
    mov qword [result_ptr],-1
    lea rcx,[clsid]
    xor edx,edx
    mov r8d,1
    lea r9,[iid_one]
    lea rax,[result_ptr]
    mov [rsp+32],rax ; CoCreateInstance's fifth argument
    call CoCreateInstance
    cmp eax,E_NOTIMPL
    jne fail
    cmp qword [result_ptr],0
    jne fail
    call GetLastError
    cmp eax,ERROR_NOT_SUPPORTED
    jne fail
    mov qword [result_ptr],-1
    lea rcx,[clsid]
    xor edx,edx
    mov r8d,4
    lea r9,[iid_two]
    lea rax,[result_ptr]
    mov [rsp+32],rax
    call CoCreateInstance
    cmp eax,E_NOTIMPL
    jne fail
    cmp qword [result_ptr],0
    jne fail
    mov qword [result_guid],-1
    mov qword [result_guid+8],-1
    lea rcx,[progid]
    lea rdx,[result_guid]
    call CLSIDFromProgID
    cmp eax,E_NOTIMPL
    jne fail
    cmp qword [result_guid],0
    jne fail
    cmp qword [result_guid+8],0
    jne fail
    call GetLastError
    cmp eax,ERROR_NOT_SUPPORTED
    jne fail
    call CoUninitialize
    PRINT ok_msg,ok_len
    mov ecx,78
    call ExitProcess
    ud2
fail:
    PRINT fail_msg,fail_len
    mov ecx,79
    call ExitProcess
    ud2
