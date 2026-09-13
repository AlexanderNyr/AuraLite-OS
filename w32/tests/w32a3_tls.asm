; w32/tests/w32a3_tls.asm — W32A-3 guest fixture (TLS + FLS).
;
; Proves under AuraLite's own libc + swapgs what the host suite proves
; against Linux: per-thread Tls/Fls slots, FLS exit callbacks, the loader's
; TLS-template registration (index cell + PROCESS/THREAD attach/detach
; counts), and worker/main isolation.
;
; Block CONTENTS are pinned white-box by the host suite; the guest pins the
; loader/runtime contract (no %gs reads here — slot isolation itself proves
; per-thread TEBs).
;
; Exit code 67 means every check passed; 1 means one did not.  Frame rule
; (W32-5): RSP % 16 == 8 at every function entry, == 0 at every CALL, so
; pushes + sub must total 8 mod 16.  Every frame below is annotated.
;
; Imports: 17 (the gate asserts the bound count).

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern CreateThread
extern WaitForSingleObject
extern CloseHandle
extern Sleep
extern TlsAlloc
extern TlsGetValue
extern TlsSetValue
extern TlsFree
extern FlsAlloc
extern FlsGetValue
extern FlsSetValue
extern FlsFree
extern GetLastError
extern SetLastError

%define INFINITE           0xFFFFFFFF
%define WAIT_OBJECT_0      0
%define TLS_OUT_OF_INDEXES 0xFFFFFFFF
%define FLS_OUT_OF_INDEXES 0xFFFFFFFF
%define ERROR_SUCCESS      0

section .text

; puts_raw(rsi = buf, edi = len) — kernel32_test's helper, verbatim shape.
; Called: entry RSP % 16 == 8; 1 push + 0x40 = 72 -> 8 mod 16. OK.
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

; --- the TLS callback ------------------------------------------------------
; ms_abi (dll, reason, reserved).  Counts by reason; THREAD_ATTACH also sets
; the flag the workers observe (it runs before the worker proc).
; 0 pushes + 0 sub: leaf, stores only.  No frame needed.
tls_cb:
    cmp  edx, 1                  ; DLL_PROCESS_ATTACH
    je   .pa
    cmp  edx, 2                  ; DLL_THREAD_ATTACH
    je   .ta
    cmp  edx, 3                  ; DLL_THREAD_DETACH
    je   .td
    cmp  edx, 0                  ; DLL_PROCESS_DETACH
    je   .pd
    ret
.pa:
    mov  byte [saw_pattach], 1
    inc  dword [c_pattach]
    ret
.ta:
    mov  byte [saw_tattach], 1
    inc  dword [c_tattach]
    ret
.td:
    inc  dword [c_tdetach]
    ret
.pd:
    inc  dword [c_pdetach]
    ret

; --- the FLS callback ------------------------------------------------------
; ms_abi (value).  Records the value + counts.  Leaf, no frame.
fls_cb:
    mov  [fls_val], rcx
    inc  dword [fls_fired]
    ret

; --- worker_tls(rcx = tls index) -------------------------------------------
; Sets + reads its OWN slot value; observes THREAD_ATTACH already ran.
; Returns 0 ok / 1 fail.  1 push + 0x30 = 56 -> 8 mod 16. OK.
worker_tls:
    push rbx
    sub  rsp, 30h
    mov  ebx, ecx                  ; save idx
    mov  ecx, ebx
    mov  rdx, 0x2222
    call TlsSetValue
    test eax, eax
    jz   .fail
    mov  ecx, ebx
    call TlsGetValue
    cmp  rax, 0x2222
    jne  .fail
    cmp  byte [saw_tattach], 1
    jne  .fail
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret
.fail:
    mov  eax, 1
    add  rsp, 30h
    pop  rbx
    ret

; --- worker_fls(rcx = fls index) -------------------------------------------
; Sets an FLS value (the exit callback must fire for it) and reads it back.
; 1 push + 0x30 = 56 -> 8 mod 16. OK.
worker_fls:
    push rbx
    sub  rsp, 30h
    mov  ebx, ecx
    mov  ecx, ebx
    mov  rdx, 0x77
    call FlsSetValue
    test eax, eax
    jz   .fail
    mov  ecx, ebx
    call FlsGetValue
    cmp  rax, 0x77
    jne  .fail
    xor  eax, eax
    add  rsp, 30h
    pop  rbx
    ret
.fail:
    mov  eax, 1
    add  rsp, 30h
    pop  rbx
    ret

; --- entry -----------------------------------------------------------------
; 1 push + 0x50 = 88 -> 8 mod 16. OK.
global start
start:
    push rbx
    sub  rsp, 50h

    mov  ecx, -11                  ; STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax

    ; --- 1. PROCESS_ATTACH ran before main; exe took slot 0 ---
    cmp  byte [saw_pattach], 1
    jne  .fail
    cmp  dword [tls_index], 0
    jne  .fail
    lea  rsi, [msg_idx]
    mov  edi, msg_idx_l
    call puts_raw

    ; --- 2. main's TLS slot ---
    call TlsAlloc
    cmp  eax, TLS_OUT_OF_INDEXES
    je   .fail
    mov  [tls_idx], eax
    mov  ecx, eax
    mov  rdx, 0x1111
    call TlsSetValue
    test eax, eax
    jz   .fail
    mov  ecx, [tls_idx]
    call TlsGetValue
    cmp  rax, 0x1111
    jne  .fail

    ; --- 3. worker isolation + THREAD_ATTACH/DETACH ---
    xor  ecx, ecx                  ; security
    xor  edx, edx                  ; stack size (default)
    lea  r8, [worker_tls]
    mov  r9d, [tls_idx]            ; param = index value
    mov  qword [rsp+20h], 0        ; flags
    lea  rax, [tid]
    mov  [rsp+28h], rax            ; tid out
    call CreateThread
    test rax, rax
    jz   .fail
    mov  [h1], rax
    mov  rcx, rax
    mov  edx, INFINITE
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h1]
    lea  rdx, [ecode]
    call GetExitCodeThread
    test eax, eax
    jz   .fail
    cmp  dword [ecode], 0
    jne  .fail
    ; main's value untouched by the worker
    mov  ecx, [tls_idx]
    call TlsGetValue
    cmp  rax, 0x1111
    jne  .fail
    cmp  dword [c_tdetach], 1
    jne  .fail
    mov  rcx, [h1]
    call CloseHandle
    lea  rsi, [msg_iso]
    mov  edi, msg_iso_l
    call puts_raw

    ; --- 4. free + realloc zeroes ---
    mov  ecx, [tls_idx]
    call TlsFree
    test eax, eax
    jz   .fail
    call TlsAlloc
    cmp  eax, TLS_OUT_OF_INDEXES
    je   .fail
    mov  [tls_idx], eax
    mov  ecx, eax
    call TlsGetValue
    test rax, rax
    jnz  .fail
    call GetLastError
    cmp  eax, ERROR_SUCCESS
    jne  .fail
    mov  ecx, [tls_idx]
    call TlsFree
    lea  rsi, [msg_free]
    mov  edi, msg_free_l
    call puts_raw

    ; --- 5. FLS + exit callback ---
    lea  rcx, [fls_cb]
    call FlsAlloc
    cmp  eax, FLS_OUT_OF_INDEXES
    je   .fail
    mov  [fls_idx], eax
    mov  ecx, eax
    mov  rdx, 0x42
    call FlsSetValue
    test eax, eax
    jz   .fail
    mov  ecx, [fls_idx]
    call FlsGetValue
    cmp  rax, 0x42
    jne  .fail
    xor  ecx, ecx
    xor  edx, edx
    lea  r8, [worker_fls]
    mov  r9d, [fls_idx]
    mov  qword [rsp+20h], 0
    lea  rax, [tid]
    mov  [rsp+28h], rax
    call CreateThread
    test rax, rax
    jz   .fail
    mov  [h1], rax
    mov  rcx, rax
    mov  edx, INFINITE
    call WaitForSingleObject
    cmp  eax, WAIT_OBJECT_0
    jne  .fail
    mov  rcx, [h1]
    lea  rdx, [ecode]
    call GetExitCodeThread
    cmp  dword [ecode], 0
    jne  .fail
    mov  rcx, [h1]
    call CloseHandle
    cmp  dword [fls_fired], 1
    jne  .fail
    cmp  qword [fls_val], 0x77
    jne  .fail
    mov  ecx, [fls_idx]
    call FlsFree
    lea  rsi, [msg_fls]
    mov  edi, msg_fls_l
    call puts_raw

    ; --- 6. callback counts: 1 process + 2 threads each way ---
    cmp  dword [c_pattach], 1
    jne  .fail
    cmp  dword [c_tattach], 2
    jne  .fail
    cmp  dword [c_tdetach], 2
    jne  .fail
    lea  rsi, [msg_cb]
    mov  edi, msg_cb_l
    call puts_raw

    lea  rsi, [msg_ok]
    mov  edi, msg_ok_l
    call puts_raw
    mov  ecx, 67
    call ExitProcess

.fail:
    lea  rsi, [msg_fail]
    mov  edi, msg_fail_l
    call puts_raw
    mov  ecx, 1
    call ExitProcess

section .rdata
msg_idx:   db "TLS-IDX-OK", 10
msg_idx_l  equ $ - msg_idx
msg_iso:   db "TLS-ISO-OK", 10
msg_iso_l  equ $ - msg_iso
msg_free:  db "TLS-FREE-OK", 10
msg_free_l equ $ - msg_free
msg_fls:   db "FLS-OK", 10
msg_fls_l  equ $ - msg_fls
msg_cb:    db "TLS-CB-OK", 10
msg_cb_l   equ $ - msg_cb
msg_ok:    db "W32A3-TLS-OK", 10
msg_ok_l   equ $ - msg_ok
msg_fail:  db "W32A3-TLS-FAIL", 10
msg_fail_l equ $ - msg_fail

align 8
tls_callbacks:
    dq tls_cb
    dq 0

; IMAGE_TLS_DIRECTORY64 (VAs — linked at a fixed base, like crttest).
section .tlsdir data align=16
global _tls_used
_tls_used:
    dq tls_data_start
    dq tls_data_end
    dq tls_index
    dq tls_callbacks
    dd 8                            ; SizeOfZeroFill (one qword)
    dd 0                            ; Characteristics

section .data align=16
tls_index    dd 0
saw_pattach  db 0
saw_tattach  db 0
c_pattach    dd 0
c_tattach    dd 0
c_tdetach    dd 0
c_pdetach    dd 0
tls_data_start:
    dq 0xAAAABBBBCCCCDDDD
    dq 0x1111222233334444
tls_data_end:

section .bss
stdout_h: resq 1
written:  resq 1
h1:       resq 1
tid:      resd 1
ecode:    resd 1
tls_idx:  resd 1
fls_idx:  resd 1
fls_fired: resd 1
fls_val:  resq 1
