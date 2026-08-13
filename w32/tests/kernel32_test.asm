; kernel32_test.asm — a PE32+ .exe that imports KERNEL32.  WIN32_PLAN.md W32-4.
;
; Unlike the W32-3 fixture, this program does NOT talk to the kernel directly.
; It calls Win32 functions through a real PE import table, which is the whole
; point of the phase: the loader must bind those imports to the w32
; implementation, and the calls must cross the ms_abi boundary intact.
;
; Written in assembly rather than C so the test depends on no cross-compiler:
; nasm -f win64 and lld-link are already in REQUIRED_TOOLS.  It follows the
; Windows x64 convention by hand -- arguments in RCX/RDX/R8/R9, 32 bytes of
; shadow space, 16-byte stack alignment at each call -- which is exactly what
; a real compiler emits and therefore exactly what the personality must accept.

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern GetLastError
extern SetLastError
extern CloseHandle
extern HeapAlloc
extern HeapFree
extern GetProcessHeap
extern GetTickCount64
extern ExitProcess

%define STD_OUTPUT_HANDLE -11
%define ERROR_INVALID_HANDLE 6

section .rdata
msg_hello:   db "W32-KERNEL32-OK", 10
msg_hello_l  equ $ - msg_hello
msg_badh:    db "BADHANDLE-REFUSED", 10
msg_badh_l   equ $ - msg_badh
msg_heap:    db "HEAP-OK", 10
msg_heap_l   equ $ - msg_heap
msg_tick:    db "TICK-OK", 10
msg_tick_l   equ $ - msg_tick

section .bss
written:  resq 1
stdout_h: resq 1

section .text
global start

; write(stdout, rsi=buf, edx=len) -- small helper, Windows convention inside
puts_raw:
    push rbp
    mov  rbp, rsp
    sub  rsp, 40h
    mov  rcx, [stdout_h]
    mov  rdx, rsi
    mov  r8d, edi
    lea  r9, [written]
    mov  qword [rsp+20h], 0
    call WriteFile
    add  rsp, 40h
    pop  rbp
    ret

start:
    sub  rsp, 28h                  ; shadow space, keeps RSP 16-aligned

    ; --- GetStdHandle + WriteFile: the basic path ---
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax

    lea  rsi, [msg_hello]
    mov  edi, msg_hello_l
    call puts_raw

    ; --- a bad handle must be refused with ERROR_INVALID_HANDLE ---
    xor  ecx, ecx
    call SetLastError              ; clear to prove the code that follows set it
    mov  rcx, 0x4242               ; a handle we never minted
    lea  rdx, [msg_hello]
    mov  r8d, 1
    lea  r9, [written]
    mov  qword [rsp+20h], 0
    call WriteFile
    test eax, eax                  ; must be FALSE (0)
    jnz  .fail
    call GetLastError
    cmp  eax, ERROR_INVALID_HANDLE
    jne  .fail
    lea  rsi, [msg_badh]
    mov  edi, msg_badh_l
    call puts_raw

    ; --- heap round trip ---
    call GetProcessHeap
    mov  rcx, rax
    mov  rbx, rax                  ; keep the heap handle (RBX is callee-saved)
    xor  edx, edx
    mov  r8, 64
    call HeapAlloc
    test rax, rax
    jz   .fail
    mov  rsi, rax
    mov  byte [rsi], 0x5A          ; prove it is writable
    cmp  byte [rsi], 0x5A
    jne  .fail
    mov  rcx, rbx
    xor  edx, edx
    mov  r8, rsi
    call HeapFree
    test eax, eax
    jz   .fail
    lea  rsi, [msg_heap]
    mov  edi, msg_heap_l
    call puts_raw

    ; --- GetTickCount64 returns something plausible (non-zero) ---
    call GetTickCount64
    test rax, rax
    jz   .fail
    lea  rsi, [msg_tick]
    mov  edi, msg_tick_l
    call puts_raw

    ; --- CloseHandle on a std handle: succeeds, must NOT kill stdout ---
    mov  rcx, [stdout_h]
    call CloseHandle
    test eax, eax
    jz   .fail
    lea  rsi, [msg_hello]          ; if stdout died this prints nothing
    mov  edi, msg_hello_l
    call puts_raw

    mov  ecx, 55
    call ExitProcess

.fail:
    mov  ecx, 1
    call ExitProcess
