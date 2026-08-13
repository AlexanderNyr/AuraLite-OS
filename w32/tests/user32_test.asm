; user32_test.asm — a PE32+ .exe that creates a window.  WIN32_PLAN.md W32-5.
;
; This is the callback direction of the ABI, which W32-4's test did not cover:
; the personality calls INTO this image's WNDPROC.  The WNDPROC checks that
; hwnd/msg/wParam/lParam arrived intact -- under the Windows convention they
; are in RCX/RDX/R8/R9 -- and records what it saw, so the test can assert the
; callback worked rather than merely that nothing crashed.
;
; It runs headless: the compositor draws to the framebuffer, which no one is
; looking at in CI, so correctness is reported over the serial console via
; WriteFile and the exit status.

bits 64
default rel

extern RegisterClassExA
extern CreateWindowExA
extern ShowWindow
extern UpdateWindow
extern DestroyWindow
extern PeekMessageA
extern DispatchMessageA
extern DefWindowProcA
extern PostQuitMessage
extern BeginPaint
extern EndPaint
extern FillRect
extern GetClientRect
extern TextOutA
extern SetTextColor
extern MoveToEx
extern LineTo
extern CreateSolidBrush
extern MessageBoxA

extern GetStdHandle
extern WriteFile
extern ExitProcess

%define STD_OUTPUT_HANDLE -11
%define WM_CREATE   0x0001
%define WM_DESTROY  0x0002
%define WM_PAINT    0x000F
%define WM_CLOSE    0x0010
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define WS_VISIBLE  0x10000000
%define SW_SHOW     5
%define PM_REMOVE   1

section .rdata
cls_name:   db "AuraW32TestClass", 0
win_title:  db "AuraLite W32 Test", 0
paint_txt:  db "PE window", 0
m_created:  db "WNDPROC-WM_CREATE", 10
m_created_l equ $ - m_created
m_window:   db "WINDOW-CREATED", 10
m_window_l  equ $ - m_window
m_painted:  db "WNDPROC-WM_PAINT", 10
m_painted_l equ $ - m_painted
m_args:     db "WNDPROC-ARGS-OK", 10
m_args_l    equ $ - m_args
m_destroy:  db "WNDPROC-WM_DESTROY", 10
m_destroy_l equ $ - m_destroy
m_done:     db "W32-USER32-OK", 10
m_done_l    equ $ - m_done

section .bss
alignb 8
stdout_h:   resq 1
hwnd:       resq 1
written:    resq 1
wc:         resb 80            ; WNDCLASSEXA
msg:        resb 48            ; MSG
ps:         resb 72            ; PAINTSTRUCT
rc:         resb 16            ; RECT
saw_create: resq 1
saw_paint:  resq 1
saw_destroy:resq 1
args_ok:    resq 1

section .text
global start

; ---- puts_raw(rsi=buf, edi=len) -------------------------------------------
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

; ---- LRESULT WNDPROC(HWND rcx, UINT edx, WPARAM r8, LPARAM r9) -------------
; Called BY the personality.  Must follow the Windows convention: RBX/RSI/RDI
; and R12-R15 are callee-saved and must survive.
; Stack alignment matters here and is easy to get wrong: Windows x64 requires
; RSP % 16 == 0 at every CALL.  Entry leaves RSP % 16 == 8 (the return address),
; and four pushes bring it back to 8, so the reserved block must be an odd
; multiple of 8 to land on 0.  0x68 does; 0x60 does not, and produced a GPF on
; the first call out of this function.
wndproc:
    push rbp
    mov  rbp, rsp
    push rbx
    push rsi
    push rdi
    sub  rsp, 68h

    mov  rbx, rcx              ; hwnd
    mov  esi, edx              ; msg

    ; The personality must pass a non-NULL hwnd; record that it did.
    test rbx, rbx
    jz   .skip_argcheck
    mov  qword [args_ok], 1
.skip_argcheck:

    cmp  esi, WM_CREATE
    je   .on_create
    cmp  esi, WM_PAINT
    je   .on_paint
    cmp  esi, WM_DESTROY
    je   .on_destroy
    jmp  .defproc

.on_create:
    mov  qword [saw_create], 1
    lea  rsi, [m_created]
    mov  edi, m_created_l
    call puts_raw
    xor  eax, eax
    jmp  .done

.on_paint:
    mov  qword [saw_paint], 1

    ; BeginPaint(hwnd, &ps)
    mov  rcx, rbx
    lea  rdx, [ps]
    call BeginPaint
    mov  rdi, rax              ; hdc (callee-saved across the calls below)

    ; GetClientRect(hwnd, &rc)
    mov  rcx, rbx
    lea  rdx, [rc]
    call GetClientRect

    ; FillRect(hdc, &rc, CreateSolidBrush(0x00E0F0FF))
    mov  ecx, 0x00E0F0FF
    call CreateSolidBrush
    mov  r8, rax
    mov  rcx, rdi
    lea  rdx, [rc]
    call FillRect

    ; SetTextColor(hdc, 0x00202020) then TextOutA(hdc, 12, 12, "PE window", 9)
    mov  rcx, rdi
    mov  edx, 0x00202020
    call SetTextColor
    mov  rcx, rdi
    mov  edx, 12
    mov  r8d, 12
    lea  r9, [paint_txt]
    mov  qword [rsp+20h], 9
    call TextOutA

    ; MoveToEx(hdc, 8, 40, NULL); LineTo(hdc, 120, 40)
    mov  rcx, rdi
    mov  edx, 8
    mov  r8d, 40
    xor  r9, r9
    call MoveToEx
    mov  rcx, rdi
    mov  edx, 120
    mov  r8d, 40
    call LineTo

    ; EndPaint(hwnd, &ps)
    mov  rcx, rbx
    lea  rdx, [ps]
    call EndPaint

    lea  rsi, [m_painted]
    mov  edi, m_painted_l
    call puts_raw
    xor  eax, eax
    jmp  .done

.on_destroy:
    mov  qword [saw_destroy], 1
    lea  rsi, [m_destroy]
    mov  edi, m_destroy_l
    call puts_raw
    xor  ecx, ecx
    call PostQuitMessage
    xor  eax, eax
    jmp  .done

.defproc:
    mov  rcx, rbx
    mov  edx, esi
    call DefWindowProcA

.done:
    add  rsp, 68h
    pop  rdi
    pop  rsi
    pop  rbx
    pop  rbp
    ret

; ---- entry ------------------------------------------------------------------
start:
    push rbp
    mov  rbp, rsp
    sub  rsp, 70h

    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax

    ; WNDCLASSEXA: cbSize=80, style=0, lpfnWndProc, ..., hbrBackground,
    ; lpszClassName.  Offsets per the documented layout.
    mov  dword [wc + 0], 80
    mov  dword [wc + 4], 0
    lea  rax, [wndproc]
    mov  [wc + 8], rax               ; lpfnWndProc
    mov  qword [wc + 48], 0x00F0F0F0 ; hbrBackground = colour (see user32.c)
    lea  rax, [cls_name]
    mov  [wc + 64], rax              ; lpszClassName

    lea  rcx, [wc]
    call RegisterClassExA
    test ax, ax
    jz   .fail

    ; CreateWindowExA(0, cls, title, WS_OVERLAPPEDWINDOW|WS_VISIBLE,
    ;                 40, 40, 320, 200, NULL, NULL, NULL, NULL)
    xor  ecx, ecx
    lea  rdx, [cls_name]
    lea  r8,  [win_title]
    mov  r9d, WS_OVERLAPPEDWINDOW | WS_VISIBLE
    mov  dword [rsp+20h], 40
    mov  dword [rsp+28h], 40
    mov  dword [rsp+30h], 320
    mov  dword [rsp+38h], 200
    mov  qword [rsp+40h], 0
    mov  qword [rsp+48h], 0
    mov  qword [rsp+50h], 0
    mov  qword [rsp+58h], 0
    call CreateWindowExA
    test rax, rax
    jz   .fail
    mov  [hwnd], rax

    lea  rsi, [m_window]
    mov  edi, m_window_l
    call puts_raw

    ; ShowWindow + UpdateWindow
    mov  rcx, [hwnd]
    mov  edx, SW_SHOW
    call ShowWindow
    mov  rcx, [hwnd]
    call UpdateWindow

    ; Pump a bounded number of messages: headless, so we must not block.
    mov  r12d, 64
.pump:
    lea  rcx, [msg]
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    mov  dword [rsp+20h], PM_REMOVE
    call PeekMessageA
    test eax, eax
    jz   .pump_done
    lea  rcx, [msg]
    call DispatchMessageA
    dec  r12d
    jnz  .pump
.pump_done:

    ; Every callback must have fired.
    cmp  qword [saw_create], 1
    jne  .fail
    cmp  qword [saw_paint], 1
    jne  .fail
    cmp  qword [args_ok], 1
    jne  .fail

    lea  rsi, [m_args]
    mov  edi, m_args_l
    call puts_raw

    ; DestroyWindow must drive WM_DESTROY into the WNDPROC.
    mov  rcx, [hwnd]
    call DestroyWindow
    cmp  qword [saw_destroy], 1
    jne  .fail

    lea  rsi, [m_done]
    mov  edi, m_done_l
    call puts_raw

    mov  ecx, 66
    call ExitProcess

.fail:
    mov  ecx, 1
    call ExitProcess
