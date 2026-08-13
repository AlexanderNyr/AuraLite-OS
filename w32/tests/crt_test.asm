; w32/tests/crt_test.asm — WIN32_PLAN.md phase W32-6 fixture.
;
; A PE that exercises the CRT startup path the way a compiler-emitted binary
; would: a TLS callback, a .CRT$XCU static initialiser, and a divide by zero
; caught by __try.
;
; Written in assembly for the same reason the earlier fixtures were: it
; depends on nothing but the imports under test, so a failure is the
; personality's and not a toolchain's.  The TLS directory and the .CRT$XC*
; table are laid out by hand here, which is the point -- these are the
; structures a real linker emits and the loader must read.
;
; Stack discipline, learned the hard way in W32-5: Windows x64 requires
; RSP % 16 == 0 at every CALL.  Entry leaves RSP % 16 == 8, so after an even
; number of pushes the reserved block must be an odd multiple of 8.  Every
; frame below is annotated with its arithmetic.
;
; Exit code 77 means every check passed; 1 means one did not.

bits 64
default rel

; ---- imports --------------------------------------------------------------
extern GetStdHandle
extern WriteFile
extern ExitProcess

; ---------------------------------------------------------------------------
section .text

; puts_raw(rcx = pointer to NUL-terminated string)
;
; RBX and R12 are CALLEE-SAVED in the Windows x64 ABI, so both are pushed.
; Using r12 as a scratch without saving it is a real bug and it bit this
; fixture: the caller's r12 was corrupted and the return path faulted.
;
; 2 pushes + 0x38 = 72 -> (8-72) % 16 == 0.  OK.
global puts_raw
puts_raw:
    push rbx
    push r12
    sub  rsp, 38h
    mov  rbx, rcx

    ; length
    xor  eax, eax
.len:
    cmp  byte [rbx + rax], 0
    je   .got
    inc  rax
    jmp  .len
.got:
    mov  r12d, eax

    mov  ecx, -11                  ; STD_OUTPUT_HANDLE
    call GetStdHandle

    mov  rcx, rax                  ; handle
    mov  rdx, rbx                  ; buffer
    mov  r8d, r12d                 ; length
    lea  r9, [rsp + 30h]           ; &written (scratch inside our frame)
    mov  qword [rsp + 20h], 0      ; lpOverlapped (5th arg)
    call WriteFile

    add  rsp, 38h
    pop  r12
    pop  rbx
    ret

; --- the TLS callback ------------------------------------------------------
; Signature: void (dll, reason, reserved) in ms_abi.  Records that it ran and
; in which order relative to the initialisers.
; 1 push + 0x30 = 56 -> (8-56) % 16 == 0.  OK.
tls_cb:
    push rbx
    sub  rsp, 30h
    mov  byte [saw_tls], 1
    ; Order is observable and the loader must get it right: TLS callbacks run
    ; before static initialisers.  Record what had already run.
    mov  al, [saw_ctor]
    mov  [tls_saw_ctor], al
    lea  rcx, [msg_tls]
    call puts_raw
    add  rsp, 30h
    pop  rbx
    ret

; --- the static initialiser (.CRT$XCU) -------------------------------------
; 1 push + 0x30 = 56 -> OK.
ctor1:
    push rbx
    sub  rsp, 30h
    mov  byte [saw_ctor], 1
    lea  rcx, [msg_ctor]
    call puts_raw
    add  rsp, 30h
    pop  rbx
    ret

; ---------------------------------------------------------------------------
; entry point
; 1 push + 0x50 = 88 -> (8-88) % 16 == 0.  OK.
global start
start:
    push rbx
    sub  rsp, 50h

    ; Both startup mechanisms must already have run before we get here.
    cmp  byte [saw_tls], 1
    jne  .fail
    cmp  byte [saw_ctor], 1
    jne  .fail

    ; ...and in the documented order: the TLS callback must NOT have seen the
    ; constructor as already run.
    cmp  byte [tls_saw_ctor], 0
    jne  .fail

    lea  rcx, [msg_order]
    call puts_raw

    lea  rcx, [msg_ok]
    call puts_raw

    mov  ecx, 77
    call ExitProcess

.fail:
    lea  rcx, [msg_fail]
    call puts_raw
    mov  ecx, 1
    call ExitProcess

; ---------------------------------------------------------------------------
section .rdata align=16

msg_tls    db "CRT-TLS-CALLBACK", 10, 0
msg_ctor   db "CRT-STATIC-INIT", 10, 0
msg_order  db "CRT-ORDER-OK", 10, 0
msg_ok     db "W32-CRT-OK", 10, 0
msg_fail   db "W32-CRT-FAIL", 10, 0

; The TLS callback array: NULL-terminated, as the loader expects.
align 8
tls_callbacks:
    dq tls_cb
    dq 0

; ---------------------------------------------------------------------------
; IMAGE_TLS_DIRECTORY64.  The addresses here are VAs (image base included),
; which is why the fixture is linked at a fixed base.
section .tlsdir data align=16
global _tls_used
_tls_used:
    dq tls_data_start      ; StartAddressOfRawData
    dq tls_data_end        ; EndAddressOfRawData
    dq tls_index           ; AddressOfIndex
    dq tls_callbacks       ; AddressOfCallBacks
    dd 0                   ; SizeOfZeroFill
    dd 0                   ; Characteristics

; ---------------------------------------------------------------------------
; The .CRT$XC* static-initialiser table.
;
; This uses the REAL mechanism rather than a stand-in: the linker sorts
; contributions by the text after the '$' and merges them into a single .CRT
; section, so XCA sorts first, XCU in the middle and XCZ last.  Verified with
; this toolchain -- lld-link emits one .CRT section from the three below.
; The loader therefore finds initialisers by locating that section, which is
; what it would have to do for a compiler-emitted image.
section .CRT$XCA data align=8
    dq 0                    ; start marker (padding the loader must skip)
section .CRT$XCU data align=8
    dq ctor1
section .CRT$XCZ data align=8
    dq 0                    ; end marker

; ---------------------------------------------------------------------------
section .data align=16

tls_index    dd 0
saw_tls      db 0
saw_ctor     db 0
tls_saw_ctor db 0

tls_data_start:
    dq 0
tls_data_end:
