; w32/tests/delayhelper.asm — W32A-1 fixture: __delayLoadHelper2.
;
; The delay-load helper is NOT loader code: on Windows it ships in the
; application's own CRT (delayimp.lib), and the OS side is just
; LoadLibrary + GetProcAddress.  This is the fixture's equivalent, written
; against the documented delay-descriptor layout (PE/COFF spec: 32-byte
; RVA-based descriptors on x86-64, Attributes bit 0 = dlattrRva):
;
;   pidd layout: grAttrs@0(D) szName@4(D) phmod@8(D) pIAT@12(D) pINT@16(D)
;                pBoundIAT@20(D) pUnloadIAT@24(D) dwTimeStamp@28(D)
;   phmod is the RVA of the HMODULE *storage* (a QWORD the helper fills),
;   not the handle: reading it as a handle reuses RVA 0x3098 as an hmod
;   and every resolution dies PROCFAIL (measured in W32A-1 bring-up).
;
; __delayLoadHelper2(pidd, ppfn): resolve the target through the loader,
; patch *ppfn, return the address.  The image base comes from the linker's
; __ImageBase and is verified (MZ + PE) before use -- a wrong base would
; corrupt, not fail.
;
; Failure contract (what the absent-fixture asserts): the DLL is named and
; the process exits 77.  That is the documented Windows behaviour -- the
; failure happens at first call, not at load -- minus the VcppException,
; which needs SEH the fixture does not have.

bits 64
default rel

extern LoadLibraryA
extern GetProcAddress
extern GetStdHandle
extern WriteFile
extern ExitProcess
extern __ImageBase

section .text

global __delayLoadHelper2
__delayLoadHelper2:                  ; rcx = pidd, rdx = ppfn
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 20h                     ; 5 pushes (40) + 32 = 72, ok
    mov rbx, rcx                     ; pidd
    mov r12, rdx                     ; ppfn
    lea r13, [rel __ImageBase]       ; image base

    cmp word [r13], 5A4Dh
    jne .nobase
    mov eax, [r13+3Ch]
    cmp dword [r13+rax], 4550h
    jne .nobase

    test dword [rbx], 1              ; dlattrRva?
    jz .nonrva

    mov eax, [rbx+4]                 ; dll name
    lea r15, [r13+rax]

    mov eax, [rbx+8]                 ; hmod storage RVA -> cached hmod?
    lea r14, [r13+rax]
    mov r14, [r14]
    test r14, r14
    jnz .havemod
    mov rcx, r15
    call LoadLibraryA
    test rax, rax
    jz .loadfail
    mov ecx, [rbx+8]
    lea rcx, [r13+rcx]
    mov [rcx], rax
    mov r14, rax
.havemod:
    mov eax, [rbx+12]                ; pIAT -> index from ppfn
    lea rcx, [r13+rax]
    mov rax, r12
    sub rax, rcx
    shr rax, 3
    mov ecx, [rbx+16]                ; pINT[ index ]
    lea rcx, [r13+rcx]
    mov rax, [rcx+rax*8]
    bt rax, 63
    jc .byord
    mov r15d, eax                    ; Hint/Name RVA + 2
    lea rdx, [r13+r15+2]
    mov rcx, r14
    call GetProcAddress
    jmp .patch
.byord:
    and eax, 0FFFFh
    mov edx, eax
    mov rcx, r14
    call GetProcAddress
.patch:
    test rax, rax
    jz .procfail
    mov [r12], rax                   ; patch the slot
    add rsp, 20h
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.loadfail:
    lea rcx, [m_loadfail]
    call puts
    mov rcx, r15
    call puts
    lea rcx, [m_crlf]
    call puts
    mov ecx, 77
    call ExitProcess
.procfail:
    lea rcx, [m_procfail]
    call puts
    mov ecx, 76
    call ExitProcess
.nonrva:
    lea rcx, [m_nonrva]
    call puts
    mov ecx, 78
    call ExitProcess
.nobase:
    lea rcx, [m_nobase]
    call puts
    mov ecx, 78
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
m_loadfail: db "DELAY-LOADFAIL ", 0
m_procfail: db "DELAY-PROCFAIL", 13, 10, 0
m_nonrva:   db "DELAY-NONRVA", 13, 10, 0
m_nobase:   db "DELAY-NOBASE", 13, 10, 0
m_crlf:     db 13, 10, 0
