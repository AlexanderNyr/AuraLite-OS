; w32a4_raise.asm — RaiseException with parameters.  W32A-4.
;
; Raises 0xE000CAFE with two parameters inside a __try; the filter checks
; the code AND both parameters through EXCEPTION_POINTERS, then EXECUTEs.
; The filter also calls the imported _XcptFilter (proving the export
; binds; its SEARCH answer for this unknown code is noted and overruled,
; which is exactly what filters are for).

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern RaiseException
extern _XcptFilter
extern __C_specific_handler

%define STD_OUTPUT_HANDLE -11
%define RAISE_CODE 0xE000CAFE

section .rdata
msg_ok:   db "W32A4-RAISE-CAUGHT", 10
msg_ok_l  equ $ - msg_ok

section .data
raise_args: dq 0x1111, 0x2222

section .bss
written:  resq 1
stdout_h: resq 1

section .text

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
puts_raw_end:

seh_personality:
    jmp  __C_specific_handler

guarded:
    push rbp
    sub  rsp, 20h
.try_begin:
    mov  ecx, RAISE_CODE
    xor  edx, edx               ; flags
    mov  r8d, 2                 ; two parameters
    lea  r9, [raise_args]
    call RaiseException
    ; NOTREACHED (the filter EXECUTEs)
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
guarded_end:

; Filter: (EXCEPTION_POINTERS* rcx, establisher rdx).
; EXCEPTION_RECORD: code @0, num_params @24, params @32.
filter_raise:
    push rbp
    sub  rsp, 20h
    mov  [rsp+18h], rcx
    mov  [rsp+10h], rdx
    ; Prove _XcptFilter binds (returns SEARCH for this code; overruled).
    mov  rdx, rcx               ; arg2 = EXCEPTION_POINTERS*
    mov  rcx, [rcx]             ; record...
    mov  ecx, [rcx]             ; ...arg1 = code
    call _XcptFilter
    mov  rcx, [rsp+18h]
    mov  rax, [rcx]             ; record
    cmp  dword [rax], RAISE_CODE
    jne  .search
    cmp  dword [rax+24], 2
    jne  .search
    cmp  qword [rax+32], 0x1111
    jne  .search
    cmp  qword [rax+40], 0x2222
    jne  .search
    mov  eax, 1
    add  rsp, 20h
    pop  rbp
    ret
.search:
    xor  eax, eax
    add  rsp, 20h
    pop  rbp
    ret
filter_raise_end:

except_raise:
    ; Funclet-entry convention (w32a4_try.asm header, = the x64 Windows
    ; rule): entry rsp == the catcher's establisher frame, so the catcher's
    ; return address is at [rsp] and rsp%16 == 8 like any post-call entry.
    ; (This body used to document the LIVE-rsp entry the unwinder had --
    ; off by 8 from the ABI.  Same shape as every except_* in
    ; w32a4_try.asm now.)  On exit: tear the scratch frame and `ret` pops
    ; the catcher's own return address -- straight back into start.
    sub  rsp, 40
    lea  rsi, [msg_ok]
    mov  edi, msg_ok_l
    call puts_raw
    add  rsp, 40
    ret
except_raise_end:

global start
start:
    push rbp
    sub  rsp, 20h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax
    call guarded
    mov  ecx, 44
    call ExitProcess
    hlt
start_end:

section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd guarded, guarded_end, guarded_xdata
    dd filter_raise, filter_raise_end, filter_raise_xdata
    dd except_raise, except_raise_end, except_raise_xdata
    dd start, start_end, start_xdata

section .xdata
puts_raw_xdata:
    db 0x01, 8, 3, 0x05
    db 8, 0x72
    db 4, 0x03
    db 1, 0x50
    db 0, 0

guarded_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd guarded.try_begin, guarded.try_end
    dd filter_raise, except_raise

filter_raise_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50

except_raise_xdata:             ; sub rsp,40 (same shape as try.asm's except_*)
    db 0x01, 4, 1, 0x00
    db 4, 0x42                  ; ALLOC_SMALL(4)
    db 0, 0

start_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
