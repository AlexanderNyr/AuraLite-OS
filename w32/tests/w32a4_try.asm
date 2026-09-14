; w32a4_try.asm — nested __try over real .pdata/.xdata.  W32A-4.
;
; The W32-6 sehtest cases, ported from the siglongjmp shim to the
; table-driven unwinder (the shim is deleted in the same phase): guarded
; divide-by-zero, a second fault (mask restore), a null-write AV, and a
; three-deep nesting that proves inner-wins + search-stops-at-handler +
; balanced return.  Every receipt string is unchanged, so the gate that
; used to watch /apps/sehtest now watches this PE instead.
;
; Unwind-info shapes vary on purpose: push-only frames (case_*), push+alloc
; (start/mid/outer/inner), push+mov+alloc with SET_FPREG (puts_raw), and
; frameless funclets (except_*).  Filters are leaves (no .pdata: correct,
; they make no calls).  The personality column points at seh_personality,
; an in-image tail-call stub into the imported __C_specific_handler --
; .xdata can only name RVAs, never imports, and this is the shape real
; compiled code takes (a direct import reference would not link).
;
; Funclet-entry convention (what the unwinder guarantees): an except body
; starts with RSP == establisher (the faulted frame's return address on
; top).  It prints, then returns straight out of the faulted function --
; from the establisher, the epilogue is just `ret`.

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern __C_specific_handler

%define STD_OUTPUT_HANDLE -11
%define INT_DIVIDE_BY_ZERO 0xC0000094
%define ACCESS_VIOLATION   0xC0000005

section .rdata
msg_div0:     db "SEH-DIV0-CAUGHT", 10
msg_div0_l    equ $ - msg_div0
msg_second:   db "SEH-SECOND-CAUGHT", 10
msg_second_l  equ $ - msg_second
msg_av:       db "SEH-AV-CAUGHT", 10
msg_av_l      equ $ - msg_av
msg_inner:    db "SEH-INNER-CAUGHT", 10
msg_inner_l   equ $ - msg_inner
msg_midfilt:  db "SEH-MID-FILTER", 10
msg_midfilt_l equ $ - msg_midfilt
msg_mid:      db "SEH-MID-CAUGHT", 10
msg_mid_l     equ $ - msg_mid
msg_outerfilt: db "SEH-OUTER-FILTER", 10
msg_outerfilt_l equ $ - msg_outerfilt
msg_balanced: db "SEH-BALANCED", 10
msg_balanced_l equ $ - msg_balanced
msg_ok:       db "W32-SEH-OK", 10
msg_ok_l      equ $ - msg_ok

section .bss
written:  resq 1
stdout_h: resq 1

section .text

; write(stdout, rsi=buf, edx=len) -- the kernel32_test idiom.
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

; The personality column: tail-call into the imported handler.
seh_personality:
    jmp  __C_specific_handler

; --- case 1: guarded divide by zero ---------------------------------------
case_div0:
    push rbp
.case_try_begin:
    xor  ecx, ecx
    mov  eax, 1
    xor  edx, edx
    div  ecx                    ; #DE
.case_try_end:
    add  rsp, 0                 ; normal path (unreached)
    pop  rbp
    ret
case_div0_end:

filter_div0:                    ; (EXCEPTION_POINTERS*, establisher)
    mov  rax, [rcx]
    cmp  dword [rax], INT_DIVIDE_BY_ZERO
    jne  .search
    mov  eax, 1                 ; EXECUTE
    ret
.search:
    xor  eax, eax
    ret

except_div0:                    ; rsp == establisher
    sub  rsp, 40
    lea  rsi, [msg_div0]
    mov  edi, msg_div0_l
    call puts_raw
    add  rsp, 40
    ret
except_div0_end:

; --- case 2: a second fault, proving the mask came back --------------------
case_second:
    push rbp
.case_try_begin:
    xor  ecx, ecx
    mov  eax, 7
    xor  edx, edx
    div  ecx                    ; #DE again
.case_try_end:
    pop  rbp
    ret
case_second_end:

filter_second:
    mov  rax, [rcx]
    cmp  dword [rax], INT_DIVIDE_BY_ZERO
    jne  .search
    mov  eax, 1
    ret
.search:
    xor  eax, eax
    ret

except_second:
    sub  rsp, 40
    lea  rsi, [msg_second]
    mov  edi, msg_second_l
    call puts_raw
    add  rsp, 40
    ret
except_second_end:

; --- case 3: null-write AV -------------------------------------------------
case_av:
    push rbp
.case_try_begin:
    xor  eax, eax
    mov  dword [rax], 0x12345678  ; write through NULL
.case_try_end:
    pop  rbp
    ret
case_av_end:

filter_av:
    mov  rax, [rcx]
    cmp  dword [rax], ACCESS_VIOLATION
    jne  .search
    mov  eax, 1
    ret
.search:
    xor  eax, eax
    ret

except_av:
    sub  rsp, 40
    lea  rsi, [msg_av]
    mov  edi, msg_av_l
    call puts_raw
    add  rsp, 40
    ret
except_av_end:

; --- case 4: three-deep nesting --------------------------------------------
inner:
    push rbp
    sub  rsp, 20h
.try_begin:
    xor  ecx, ecx
    mov  eax, 1
    xor  edx, edx
    div  ecx                    ; #DE (inner handles)
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
inner_end:

filter_inner:
    mov  rax, [rcx]
    cmp  dword [rax], INT_DIVIDE_BY_ZERO
    jne  .search
    mov  eax, 1
    ret
.search:
    xor  eax, eax
    ret

except_inner:
    sub  rsp, 40
    lea  rsi, [msg_inner]
    mov  edi, msg_inner_l
    call puts_raw
    add  rsp, 40
    ret
except_inner_end:

mid:
    push rbp
    sub  rsp, 20h
.try_begin:
    call inner                  ; inner's fault stays in inner
    xor  ecx, ecx               ; ...then fault ourselves
    mov  eax, 1
    xor  edx, edx
    div  ecx                    ; #DE (mid handles)
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
mid_end:

filter_mid:
    ; Proves consultation: runs exactly once (mid's own fault, never
    ; inner's).  Prints through a call -- so, unlike the other filters,
    ; this one is NOT a leaf and carries .pdata.  rsi/rdi are Win64
    ; callee-saved (the C personality keeps its scope index in edi
    ; across this call); the pushes keep rsp%16==0 at the call below.
    push rbp
    push rsi
    push rdi
    sub  rsp, 20h
    mov  [rsp+18h], rcx         ; spill EXCEPTION_POINTERS*
    mov  [rsp+10h], rdx
    lea  rsi, [msg_midfilt]
    mov  edi, msg_midfilt_l
    call puts_raw
    mov  rcx, [rsp+18h]
    mov  rax, [rcx]
    cmp  dword [rax], INT_DIVIDE_BY_ZERO
    jne  .search
    mov  eax, 1
    add  rsp, 20h
    pop  rdi
    pop  rsi
    pop  rbp
    ret
.search:
    xor  eax, eax
    add  rsp, 20h
    pop  rdi
    pop  rsi
    pop  rbp
    ret
filter_mid_end:

except_mid:
    sub  rsp, 40
    lea  rsi, [msg_mid]
    mov  edi, msg_mid_l
    call puts_raw
    add  rsp, 40
    ret
except_mid_end:

outer:
    push rbp
    sub  rsp, 20h
.try_begin:
    call mid
.try_end:
    add  rsp, 20h
    pop  rbp
    ret
outer_end:

filter_outer:                   ; SEARCH always; prints if ever consulted
    push rbp                      ; (rsi/rdi saved: Win64 callee-saved)
    push rsi
    push rdi
    sub  rsp, 20h
    lea  rsi, [msg_outerfilt]
    mov  edi, msg_outerfilt_l
    call puts_raw
    xor  eax, eax
    add  rsp, 20h
    pop  rdi
    pop  rsi
    pop  rbp
    ret
filter_outer_end:

; --- entry -----------------------------------------------------------------
global start
start:
    push rbp
    sub  rsp, 20h
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax

    call case_div0
    call case_second
    call case_av
    call outer

    lea  rsi, [msg_balanced]
    mov  edi, msg_balanced_l
    call puts_raw
    lea  rsi, [msg_ok]
    mov  edi, msg_ok_l
    call puts_raw

    mov  ecx, 44
    call ExitProcess
    ; NOTREACHED
    hlt
start_end:

; --- .pdata (sorted by BeginAddress: same order as .text) -------------------
section .pdata
    dd puts_raw, puts_raw_end, puts_raw_xdata
    dd case_div0, case_div0_end, case_div0_xdata
    dd except_div0, except_div0_end, except_div0_xdata
    dd case_second, case_second_end, case_second_xdata
    dd except_second, except_second_end, except_second_xdata
    dd case_av, case_av_end, case_av_xdata
    dd except_av, except_av_end, except_av_xdata
    dd inner, inner_end, inner_xdata
    dd except_inner, except_inner_end, except_inner_xdata
    dd mid, mid_end, mid_xdata
    dd filter_mid, filter_mid_end, filter_mid_xdata
    dd except_mid, except_mid_end, except_mid_xdata
    dd outer, outer_end, outer_xdata
    dd filter_outer, filter_outer_end, filter_outer_xdata
    dd start, start_end, start_xdata

; --- .xdata ------------------------------------------------------------------
; ver_flags: 0x01 plain, 0x09 with __C_specific_handler.
; Code bytes: (offset, op|info<<4), stored in reverse prolog order.
section .xdata
puts_raw_xdata:                 ; push rbp; mov rbp,rsp; sub rsp,0x40
    db 0x01, 8, 3, 0x05         ; ver, prolog, count, fpreg=rbp
    db 8, 0x72                  ; ALLOC_SMALL(7)
    db 4, 0x03                  ; SET_FPREG
    db 1, 0x50                  ; PUSH rbp
    db 0, 0                     ; pad to even count

case_div0_xdata:                ; push rbp
    db 0x09, 1, 1, 0x00
    db 1, 0x50                  ; PUSH rbp
    db 0, 0                     ; pad
    dd seh_personality
    dd 1                        ; one scope
    dd case_div0.case_try_begin, case_div0.case_try_end
    dd filter_div0, except_div0

except_div0_xdata:              ; sub rsp,40
    db 0x01, 4, 1, 0x00
    db 4, 0x42                  ; ALLOC_SMALL(4)
    db 0, 0

case_second_xdata:
    db 0x09, 1, 1, 0x00
    db 1, 0x50
    db 0, 0
    dd seh_personality
    dd 1
    dd case_second.case_try_begin, case_second.case_try_end
    dd filter_second, except_second

except_second_xdata:
    db 0x01, 4, 1, 0x00
    db 4, 0x42
    db 0, 0

case_av_xdata:
    db 0x09, 1, 1, 0x00
    db 1, 0x50
    db 0, 0
    dd seh_personality
    dd 1
    dd case_av.case_try_begin, case_av.case_try_end
    dd filter_av, except_av

except_av_xdata:
    db 0x01, 4, 1, 0x00
    db 4, 0x42
    db 0, 0

inner_xdata:                    ; push rbp; sub rsp,0x20
    db 0x09, 5, 2, 0x00
    db 5, 0x32                  ; ALLOC_SMALL(3)
    db 1, 0x50                  ; PUSH rbp
    dd seh_personality
    dd 1
    dd inner.try_begin, inner.try_end
    dd filter_inner, except_inner

except_inner_xdata:
    db 0x01, 4, 1, 0x00
    db 4, 0x42
    db 0, 0

mid_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd mid.try_begin, mid.try_end
    dd filter_mid, except_mid

filter_mid_xdata:               ; push rbp; sub rsp,0x20 (no handler)
    db 0x01, 7, 4, 0x00
    db 7, 0x32
    db 3, 0x70
    db 2, 0x60
    db 1, 0x50

except_mid_xdata:
    db 0x01, 4, 1, 0x00
    db 4, 0x42
    db 0, 0

outer_xdata:
    db 0x09, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
    dd seh_personality
    dd 1
    dd outer.try_begin, outer.try_end
    dd filter_outer, outer.try_end

filter_outer_xdata:
    db 0x01, 7, 4, 0x00
    db 7, 0x32
    db 3, 0x70
    db 2, 0x60
    db 1, 0x50

start_xdata:
    db 0x01, 5, 2, 0x00
    db 5, 0x32
    db 1, 0x50
