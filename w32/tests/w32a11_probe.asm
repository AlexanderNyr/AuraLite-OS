; W32A-11 activation/refusal fixture: exercise the REAL ProgID lookup and
; the committed (empty) in-process activation table through the Win64 import
; binder. Positive path: HKCR\<progid>\CLSID is seeded through the W32A-9
; registry and resolved back byte-exact. These identifiers are SYNTHETIC
; test vectors, not pinned-app CLSID/IID requests -- the pinned-app probe
; observed no pair, so the activation table stays empty and every in-process
; activation is REGDB_E_CLASSNOTREG. Apache-2.0.
bits 64
default rel
extern CoInitialize
extern CoUninitialize
extern CoCreateInstance
extern CLSIDFromProgID
extern RegCreateKeyExW
extern RegSetValueExW
extern RegCloseKey
extern GetLastError
extern GetStdHandle
extern WriteFile
extern ExitProcess
%define E_INVALIDARG 0x80070057
%define E_NOTIMPL 0x80004001
%define CO_E_CLASSSTRING 0x800401f3
%define REGDB_E_CLASSNOTREG 0x80040154
%define CLASS_E_NOAGGREGATION 0x80040110
%define ERROR_FILE_NOT_FOUND 2
%define ERROR_INVALID_DATA 13
%define ERROR_NOT_SUPPORTED 50
%define ERROR_INVALID_PARAMETER 87
%define ERROR_SUCCESS 0
%define HKCR 0x80000000
%define REG_SZ 1
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
progid_any: dw 'N','P','P','.',0x00e9,0xd840,0xdc00,0x005c,0x000a,'=',' ',0
key_probe: dw 'A','u','r','a','W','3','2','A','1','1','.','P','r','o','b','e',0x5c,'C','L','S','I','D',0
progid_probe: dw 'A','u','r','a','W','3','2','A','1','1','.','P','r','o','b','e',0
key_bad: dw 'A','u','r','a','W','3','2','A','1','1','.','B','a','d',0x5c,'C','L','S','I','D',0
progid_badcls: dw 'A','u','r','a','W','3','2','A','1','1','.','B','a','d',0
guid_text: dw '{','1','2','3','4','5','6','7','8','-','9','a','b','c','-','d','e','f','0','-', \
              '1','1','2','2','-','3','3','4','4','5','5','6','6','7','7','8','8','}',0
bad_text: dw 'n','o','t','-','a','-','g','u','i','d',0
ok_msg: db 'A11-PROBE-FIXTURE-OK',13,10
ok_len equ $-ok_msg
fail_msg: db 'A11-PROBE-FIXTURE-FAIL',13,10
fail_len equ $-fail_msg
section .bss
stdout: resq 1
written: resd 1
disp: resd 1
hkey: resq 1
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
    sub rsp,80 ; shadow + up to five stack args, 16-byte aligned at calls
    mov ecx,-11
    call GetStdHandle
    mov [stdout],rax
    xor ecx,ecx
    call CoInitialize
    test eax,eax
    jnz fail
    ; ---- activation vocabulary, committed empty table ---------------------
    ; in-process on an unobserved CLSID: REGDB_E_CLASSNOTREG, out cleared.
    mov qword [result_ptr],-1
    lea rcx,[clsid]
    xor edx,edx
    mov r8d,1
    lea r9,[iid_one]
    lea rax,[result_ptr]
    mov [rsp+32],rax
    call CoCreateInstance
    cmp eax,REGDB_E_CLASSNOTREG
    jne fail
    cmp qword [result_ptr],0
    jne fail
    call GetLastError
    cmp eax,ERROR_FILE_NOT_FOUND
    jne fail
    ; out-of-process context: named E_NOTIMPL, never coerced to in-proc.
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
    call GetLastError
    cmp eax,ERROR_NOT_SUPPORTED
    jne fail
    ; ctx==0: invalid argument, not a silent default context.
    mov qword [result_ptr],-1
    lea rcx,[clsid]
    xor edx,edx
    xor r8d,r8d
    lea r9,[iid_one]
    lea rax,[result_ptr]
    mov [rsp+32],rax
    call CoCreateInstance
    cmp eax,E_INVALIDARG
    jne fail
    cmp qword [result_ptr],0
    jne fail
    call GetLastError
    cmp eax,ERROR_INVALID_PARAMETER
    jne fail
    ; aggregation: refused by name.
    mov qword [result_ptr],-1
    lea rcx,[clsid]
    mov edx,1
    mov r8d,1
    lea r9,[iid_one]
    lea rax,[result_ptr]
    mov [rsp+32],rax
    call CoCreateInstance
    cmp eax,CLASS_E_NOAGGREGATION
    jne fail
    cmp qword [result_ptr],0
    jne fail
    ; ---- ProgID lookup: refusals, then the hive round-trip ----------------
    mov rax,-1
    mov [result_guid],rax
    mov [result_guid+8],rax
    xor ecx,ecx
    lea rdx,[result_guid]
    call CLSIDFromProgID
    cmp eax,E_INVALIDARG
    jne fail
    cmp qword [result_guid],0
    jne fail
    cmp qword [result_guid+8],0
    jne fail
    call GetLastError
    cmp eax,ERROR_INVALID_PARAMETER
    jne fail
    ; unregistered (lossy-hostile Unicode) ProgID: not found, output zeroed.
    mov rax,-1
    mov [result_guid],rax
    mov [result_guid+8],rax
    lea rcx,[progid_any]
    lea rdx,[result_guid]
    call CLSIDFromProgID
    cmp eax,CO_E_CLASSSTRING
    jne fail
    cmp qword [result_guid],0
    jne fail
    cmp qword [result_guid+8],0
    jne fail
    call GetLastError
    cmp eax,ERROR_FILE_NOT_FOUND
    jne fail
    ; seed HKCR\AuraW32A11.Probe\CLSID default value through the registry.
    mov rcx,HKCR
    lea rdx,[key_probe]
    xor r8d,r8d
    xor r9d,r9d
    mov qword [rsp+32],0
    mov qword [rsp+40],0
    mov qword [rsp+48],0
    lea rax,[hkey]
    mov [rsp+56],rax
    lea rax,[disp]
    mov [rsp+64],rax
    call RegCreateKeyExW
    test eax,eax
    jnz fail
    mov rcx,[hkey]
    xor edx,edx
    xor r8d,r8d
    mov r9d,REG_SZ
    lea rax,[guid_text]
    mov [rsp+32],rax
    mov qword [rsp+40],78
    call RegSetValueExW
    test eax,eax
    jnz fail
    mov rcx,[hkey]
    call RegCloseKey
    test eax,eax
    jnz fail
    ; resolve it back: S_OK and the CLSID bytes are exact.
    mov rax,-1
    mov [result_guid],rax
    mov [result_guid+8],rax
    lea rcx,[progid_probe]
    lea rdx,[result_guid]
    call CLSIDFromProgID
    test eax,eax
    jnz fail
    mov rax,0xdef09abc12345678
    cmp [result_guid],rax
    jne fail
    mov rax,0x8877665544332211
    cmp [result_guid+8],rax
    jne fail
    ; registered but malformed CLSID text: refused as invalid data.
    mov rcx,HKCR
    lea rdx,[key_bad]
    xor r8d,r8d
    xor r9d,r9d
    mov qword [rsp+32],0
    mov qword [rsp+40],0
    mov qword [rsp+48],0
    lea rax,[hkey]
    mov [rsp+56],rax
    lea rax,[disp]
    mov [rsp+64],rax
    call RegCreateKeyExW
    test eax,eax
    jnz fail
    mov rcx,[hkey]
    xor edx,edx
    xor r8d,r8d
    mov r9d,REG_SZ
    lea rax,[bad_text]
    mov [rsp+32],rax
    mov qword [rsp+40],22
    call RegSetValueExW
    test eax,eax
    jnz fail
    mov rcx,[hkey]
    call RegCloseKey
    test eax,eax
    jnz fail
    mov rax,-1
    mov [result_guid],rax
    mov [result_guid+8],rax
    lea rcx,[progid_badcls]
    lea rdx,[result_guid]
    call CLSIDFromProgID
    cmp eax,CO_E_CLASSSTRING
    jne fail
    cmp qword [result_guid],0
    jne fail
    cmp qword [result_guid+8],0
    jne fail
    call GetLastError
    cmp eax,ERROR_INVALID_DATA
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
