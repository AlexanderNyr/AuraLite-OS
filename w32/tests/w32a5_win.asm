; w32a5_win.asm — W32APP_PLAN.md phase W32A-5 guest gate.
;
; One window, one worker thread, and the message core walked end to end:
; the W entry points (D6), the NCCREATE-before-CREATE order, style and class
; refusals by name, geometry against the compositor's own metrics, a real
; subclass chain through CallWindowProcW, window text, the update region and
; a paint that really happens, scroll state, focus/capture/Z-order, the
; single monitor, input state, and -- the phase's headline -- a
; SendMessageW that crosses threads and returns the value the *other*
; thread's window procedure computed.
;
; Every section prints A5-<NAME>-OK only after all of its checks passed, and
; a failure prints A5-<NAME>-FAIL and exits 79.  The final W32A5-WIN-OK plus
; exit code 78 is what tests/integration/cases/test_w32a5_user32win.sh
; asserts; exit 1 means the personality refused to load the image at all.

bits 64
default rel

extern GetStdHandle
extern WriteFile
extern ExitProcess
extern GetLastError
extern CreateThread
extern WaitForSingleObject
extern GetCurrentThreadId
extern GetTickCount64

extern RegisterClassExW
extern RegisterClassA
extern CreateWindowExW
extern DefWindowProcW
extern DefWindowProcA
extern CallWindowProcW
extern GetClassNameW
extern IsWindow
extern DestroyWindow
extern GetWindowRect
extern GetClientRect
extern ClientToScreen
extern AdjustWindowRectEx
extern SetWindowLongPtrW
extern GetWindowLongPtrW
extern SendMessageW
extern PostMessageW
extern GetMessageA
extern TranslateMessage
extern DispatchMessageA
extern SetWindowTextW
extern GetWindowTextW
extern GetWindowTextLengthW
extern InvalidateRect
extern ValidateRect
extern GetUpdateRgn
extern BeginPaint
extern EndPaint
extern RedrawWindow
extern ScrollWindow
extern SetScrollInfo
extern GetScrollInfo
extern SetScrollPos
extern GetScrollPos
extern SetFocus
extern GetFocus
extern SetCapture
extern GetCapture
extern ReleaseCapture
extern SetWindowPos
extern BringWindowToTop
extern GetSystemMetrics
extern GetSysColor
extern GetSysColorBrush
extern SystemParametersInfoW
extern MonitorFromWindow
extern GetMonitorInfoW
extern MapVirtualKeyW
extern ToAscii
extern GetCursorPos
extern GetWindowThreadProcessId
extern PostQuitMessage
extern GetDC
extern CreateSolidBrush
extern FillRect
extern Sleep

%define STD_OUTPUT_HANDLE -11

%define WS_CHILD     0x40000000
%define WS_VISIBLE   0x10000000
%define WS_OVERLAPPEDWINDOW 0x00CF0000

%define WM_CREATE     0x0001
%define WM_PAINT      0x000F
%define WM_NCCREATE   0x0081
%define WM_APP        0x8000
%define WM_QUIT       0x0012

%define GWL_WNDPROC   -4

%define ERR_INVALID_PARAMETER      87
%define ERR_CALL_NOT_IMPLEMENTED  120
%define ERR_ALREADY_EXISTS        183

%define SWP_NOSIZE    0x0001
%define SWP_NOMOVE    0x0002
%define SWP_NOREDRAW  0x0008
%define RDW_INVALIDATE 0x0001
%define RDW_UPDATENOW  0x0100

%define SM_CXSCREEN   0
%define SM_CYSCREEN   1
%define SM_CYCAPTION  4
%define SM_CYFULLSCREEN 17
%define SM_CXFRAME    32
%define SM_CYFRAME    33
%define SM_CMONITORS  80
%define COLOR_WINDOW       5
%define COLOR_ACTIVECAPTION 2
%define SPI_GETWORKAREA 48
%define MONITOR_DEFAULTTONEAREST 2

%define SB_VERT       1
%define SIF_RANGE     0x0001
%define SIF_PAGE      0x0002
%define SIF_POS       0x0004
%define SIF_ALL       0x017F

%define PROBE_MSG     0x8004       ; the cross-thread send
%define PROBE_ARG     41
%define PROBE_REPLY   42           ; the worker returns arg+1; only it can know

%define INFINITE      0xFFFFFFFF

; The pixel the VNC half of the gate looks for: pure green, in Win32 COLORREF
; order (0x00BBGGRR), which the personality converts to AuraLite's 0x00RRGGBB.
%define HOLD_MS        10000

%define PROBE_COLORREF 0x0000FF00
%define PROBE_RGB      0x0000FF00

section .rdata
cls_w:      dw 'A','5','W','i','n',0
title_w:    dw 'A','5',' ','w','i','n','d','o','w',0
text_w:     dw 's','u','b','c','l','a','s','s','e','d',0
cls_work:   db "A5Work",0                       ; the ANSI name RegisterClassA takes
cls_work_w: dw 'A','5','W','o','r','k',0        ; ...and the UTF-16 one CreateWindowExW
title_work_w: dw 'A','5',' ','w','o','r','k','e','r',0

m_cls:      db "A5-CLS-OK",10
m_cls_l     equ $ - m_cls
m_create:   db "A5-CREATE-OK",10
m_create_l  equ $ - m_create
m_geom:     db "A5-GEOM-OK",10
m_geom_l    equ $ - m_geom
m_sub:      db "A5-SUBCLASS-OK",10
m_sub_l     equ $ - m_sub
m_text:     db "A5-TEXT-OK",10
m_text_l    equ $ - m_text
m_paint:    db "A5-PAINT-OK",10
m_paint_l   equ $ - m_paint
m_scroll:   db "A5-SCROLL-OK",10
m_scroll_l  equ $ - m_scroll
m_zorder:   db "A5-ZORDER-OK",10
m_zorder_l  equ $ - m_zorder
m_theme:    db "A5-THEME",10
m_theme_l   equ $ - m_theme
m_metric:   db "A5-METRIC-OK",10
m_metric_l  equ $ - m_metric
m_monitor:  db "A5-MONITOR-OK",10
m_monitor_l equ $ - m_monitor
m_input_hdr: db "A5-INPUT-AS-REPORTED",10
m_input_hdr_l equ $ - m_input_hdr
m_input_ok: db "A5-INPUT-OK",10
m_input_ok_l equ $ - m_input_ok
m_thread_hdr: db "A5-THREAD-AS-REPORTED",10
m_thread_hdr_l equ $ - m_thread_hdr
m_thread_send_hdr: db "A5-THREAD-SEND-AS-REPORTED",10
m_thread_send_hdr_l equ $ - m_thread_send_hdr
m_thread:   db "A5-THREAD-OK",10
m_thread_l  equ $ - m_thread
m_pixel:    db "A5-PIXEL-AT",10
m_pixel_l   equ $ - m_pixel
m_visible:  db "A5-PAINT-VISIBLE",10
m_sleep:    db "A5-HOLD-MS",10
m_sleep_l   equ $ - m_sleep
m_visible_l equ $ - m_visible
m_ok:       db "W32A5-WIN-OK",10
m_ok_l      equ $ - m_ok

f_cls:      db "A5-CLS-FAIL",10
f_cls_l     equ $ - f_cls
f_create:   db "A5-CREATE-FAIL",10
f_create_l  equ $ - f_create
m_probe:    db "A5-GEOMETRY-AS-REPORTED",10
m_probe_l   equ $ - m_probe
f_geom1:    db "A5-GEOM-FAIL-1",10
f_geom1_l   equ $ - f_geom1
f_geom2:    db "A5-GEOM-FAIL-2",10
f_geom2_l   equ $ - f_geom2
f_geom3:    db "A5-GEOM-FAIL-3",10
f_geom3_l   equ $ - f_geom3
f_geom4:    db "A5-GEOM-FAIL-4",10
f_geom4_l   equ $ - f_geom4
f_geom5:    db "A5-GEOM-FAIL-5",10
f_geom5_l   equ $ - f_geom5
f_geom6:    db "A5-GEOM-FAIL-6",10
f_geom6_l   equ $ - f_geom6
f_geom7:    db "A5-GEOM-FAIL-7",10
f_geom7_l   equ $ - f_geom7
f_geom8:    db "A5-GEOM-FAIL-8",10
f_geom8_l   equ $ - f_geom8
f_geom9:    db "A5-GEOM-FAIL-9",10
f_geom9_l   equ $ - f_geom9
f_sub:      db "A5-SUBCLASS-FAIL",10
f_sub_l     equ $ - f_sub
f_text:     db "A5-TEXT-FAIL",10
f_text_l    equ $ - f_text
f_pixel:    db "A5-PIXEL-FAIL",10
f_pixel_l   equ $ - f_pixel
f_paint:    db "A5-PAINT-FAIL",10
f_paint_l   equ $ - f_paint
f_scroll:   db "A5-SCROLL-FAIL",10
f_scroll_l  equ $ - f_scroll
f_zorder:   db "A5-ZORDER-FAIL",10
f_zorder_l  equ $ - f_zorder
f_metric1:  db "A5-METRIC-FAIL-1",10
f_metric1_l equ $ - f_metric1
f_metric2:  db "A5-METRIC-FAIL-2",10
f_metric2_l equ $ - f_metric2
f_metric3:  db "A5-METRIC-FAIL-3",10
f_metric3_l equ $ - f_metric3
f_metric4:  db "A5-METRIC-FAIL-4",10
f_metric4_l equ $ - f_metric4
f_metric5:  db "A5-METRIC-FAIL-5",10
f_metric5_l equ $ - f_metric5
f_metric6:  db "A5-METRIC-FAIL-6",10
f_metric6_l equ $ - f_metric6
f_metric7:  db "A5-METRIC-FAIL-7",10
f_metric7_l equ $ - f_metric7
f_metric8:  db "A5-METRIC-FAIL-8",10
f_metric8_l equ $ - f_metric8
f_metric9:  db "A5-METRIC-FAIL-9",10
f_metric9_l equ $ - f_metric9
f_monitor:  db "A5-MONITOR-FAIL",10
f_monitor_l equ $ - f_monitor
f_input1:   db "A5-INPUT-FAIL-1",10
f_input1_l equ $ - f_input1
f_input2:   db "A5-INPUT-FAIL-2",10
f_input2_l equ $ - f_input2
f_input3:   db "A5-INPUT-FAIL-3",10
f_input3_l equ $ - f_input3
f_thread1:  db "A5-THREAD-FAIL-1",10
f_thread1_l equ $ - f_thread1
f_thread2:  db "A5-THREAD-FAIL-2",10
f_thread2_l equ $ - f_thread2
f_thread3:  db "A5-THREAD-FAIL-3",10
f_thread3_l equ $ - f_thread3
f_thread4:  db "A5-THREAD-FAIL-4",10
f_thread4_l equ $ - f_thread4
f_thread5:  db "A5-THREAD-FAIL-5",10
f_thread5_l equ $ - f_thread5
f_thread6:  db "A5-THREAD-FAIL-6",10
f_thread6_l equ $ - f_thread6
f_thread7:  db "A5-THREAD-FAIL-7",10
f_thread7_l equ $ - f_thread7

section .bss
written:     resq 1
stdout_h:    resq 1
hwnd:        resq 1
work_hwnd:   resq 1
work_tid:    resq 1
work_proc_tid: resq 1
work_thread: resq 1
ready_flag:  resd 1
sub_prev:    resq 1
sub_hits:    resd 1
class_hits:  resd 1
nccreate_seen: resd 1
create_seen: resd 1
paint_count: resd 1
pending_wp:  resq 1
geom_w:      resd 1
probe:       resd 16
hexbuf:      resb 128
geom_h:      resd 1
work_err:    resd 1
t0:          resq 1
color_tmp:   resq 1
dc_tmp:      resq 1
wc:          resb 96
msg:         resb 48
ps:          resb 72
sinfo:          resb 32
rect:        resb 16
rect2:       resb 16
pt:          resb 8
mi:          resb 112
keystate:    resb 256
wbuf:        resb 64
out_ch:      resb 4

section .text

; ---- WriteFile-backed marker printer: rsi = bytes, edi = length -----------
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

; ---- hex dump: rsi = dwords, ecx = count (a debug aid the gate keeps: the
; numbers the compositor reported are worth having in the log when a
; geometry assertion fails).
hexout:
    push rbp
    mov  rbp, rsp
    sub  rsp, 40h
    lea  rdi, [hexbuf]
.loop:
    mov  eax, [rsi]
    add  rsi, 4
    mov  edx, 8
.nib:
    rol  eax, 4
    mov  r8d, eax
    and  r8d, 0Fh
    cmp  r8d, 9
    jbe  .dig
    add  r8d, 'A'-10
    jmp  .st
.dig:
    add  r8d, '0'
.st:
    mov  [rdi], r8b
    inc  rdi
    dec  edx
    jnz  .nib
    mov  byte [rdi], ' '
    inc  rdi
    dec  ecx
    jnz  .loop
    mov  byte [rdi], 10
    lea  rdx, [hexbuf]
    mov  r8, rdi
    sub  r8, rdx
    inc  r8
    mov  rcx, [stdout_h]
    lea  r9, [written]
    mov  qword [rsp+20h], 0
    call WriteFile
    add  rsp, 40h
    pop  rbp
    ret
hexout_end:

; ---- failure paths: print the section marker and exit 79 ------------------
fail_now:
    xor  ecx, ecx
    call ExitProcess
    hlt

; ---- the class procedure of the main window -------------------------------
; Counts the creation messages and the subclass probe, then defers.
wndproc_main:
    push rbp
    mov  rbp, rsp
    sub  rsp, 30h
    cmp  edx, WM_NCCREATE
    jne  .not_nc
    mov  dword [nccreate_seen], 1
    jmp  .def
.not_nc:
    cmp  edx, WM_CREATE
    jne  .not_create
    mov  dword [create_seen], 1
    jmp  .def
.not_create:
    cmp  edx, WM_PAINT
    jne  .not_paint
    inc  dword [paint_count]
    jmp  .def
.not_paint:
    cmp  edx, WM_APP
    jne  .def
    mov  dword [class_hits], 1
.def:
    call DefWindowProcW
    add  rsp, 30h
    pop  rbp
    ret
wndproc_main_end:

; ---- the subclass: it must see the probe and forward through the chain ----
wndproc_sub:
    push rbp
    mov  rbp, rsp
    sub  rsp, 40h
    mov  [rsp+28h], r9                     ; keep lParam
    mov  r9, r8                            ; wParam
    mov  r8, rdx                           ; msg
    mov  rdx, rcx                          ; hwnd
    cmp  r8d, WM_APP
    jne  .forward
    mov  dword [sub_hits], 1
.forward:
    mov  rcx, [sub_prev]                   ; the procedure it replaced
    mov  rax, [rsp+28h]
    mov  [rsp+20h], rax
    call CallWindowProcW
    add  rsp, 40h
    pop  rbp
    ret
wndproc_sub_end:

; ---- the worker thread's class procedure ---------------------------------
; Answers the cross-thread probe with wParam+1 and records the thread it ran
; on: the sender compares that against the worker's own tid.
wndproc_work:
    push rbp
    mov  rbp, rsp
    sub  rsp, 30h
    cmp  edx, PROBE_MSG
    jne  .def
    mov  [pending_wp], r8
    call GetCurrentThreadId
    mov  [work_proc_tid], rax
    mov  rax, [pending_wp]
    inc  rax
    add  rsp, 30h
    pop  rbp
    ret
.def:
    call DefWindowProcA
    add  rsp, 30h
    pop  rbp
    ret
wndproc_work_end:

; ---- the worker: creates its own window and pumps until it is told to stop -
worker_main:
    push rbp
    mov  rbp, rsp
    sub  rsp, 90h
    call GetCurrentThreadId
    mov  [work_tid], rax

    ; RegisterClassA("A5Work") with wndproc_work
    lea  rdi, [wc]
    xor  eax, eax
    mov  ecx, 12
.zero:
    mov  [rdi], rax
    add  rdi, 8
    dec  ecx
    jnz  .zero
    mov  dword [wc+0], 80
    lea  rax, [wndproc_work]
    mov  [wc+8], rax
    mov  qword [wc+48], 0x00F0F0F0
    lea  rax, [cls_work]
    mov  [wc+64], rax
    lea  rcx, [wc]
    call RegisterClassA
    test ax, ax
    jz   .die_class

    xor  ecx, ecx
    lea  rdx, [cls_work_w]
    lea  r8,  [title_work_w]
    mov  r9d, WS_OVERLAPPEDWINDOW | WS_VISIBLE
    mov  dword [rsp+20h], 40
    mov  dword [rsp+28h], 40
    mov  dword [rsp+30h], 200
    mov  dword [rsp+38h], 120
    mov  qword [rsp+40h], 0
    mov  qword [rsp+48h], 0
    mov  qword [rsp+50h], 0
    mov  qword [rsp+58h], 0
    call CreateWindowExW
    test rax, rax
    jz   .die_create
    mov  [work_hwnd], rax
    mov  dword [ready_flag], 1

.loop:
    lea  rcx, [msg]
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    call GetMessageA
    test eax, eax
    jz   .done
    lea  rcx, [msg]
    call TranslateMessage
    lea  rcx, [msg]
    call DispatchMessageA
    jmp  .loop
.done:
    xor  eax, eax
    add  rsp, 90h
    pop  rbp
    ret
.die_class:
    mov  dword [ready_flag], 0FFFFFF01h     ; the stage that failed
    call GetLastError
    mov  [work_err], eax
    mov  eax, 1
    add  rsp, 90h
    pop  rbp
    ret
.die_create:
    mov  dword [ready_flag], 0FFFFFF02h
    call GetLastError
    mov  [work_err], eax
    mov  eax, 1
    add  rsp, 90h
    pop  rbp
    ret
worker_main_end:

; ---- the main thread -----------------------------------------------------
global start
start:
    push rbp
    mov  rbp, rsp
    sub  rsp, 100h

    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [stdout_h], rax

    ; ---------------- classes -------------------------------------------
    lea  rdi, [wc]
    xor  eax, eax
    mov  ecx, 12
.zero: mov [rdi], rax
    add  rdi, 8
    dec  ecx
    jnz  .zero
    mov  dword [wc+0], 80
    mov  dword [wc+4], 0
    lea  rax, [wndproc_main]
    mov  [wc+8], rax
    mov  qword [wc+48], 0x00F0F0F0
    lea  rax, [cls_w]
    mov  [wc+64], rax
    lea  rcx, [wc]
    call RegisterClassExW
    test ax, ax
    jz   .f_cls
    ; the same class again must be refused by name
    lea  rcx, [wc]
    call RegisterClassExW
    test ax, ax
    jnz  .f_cls
    call GetLastError
    cmp  eax, ERR_ALREADY_EXISTS
    jne  .f_cls
    lea  rsi, [m_cls]
    mov  edi, m_cls_l
    call puts_raw

    ; ---------------- creation ------------------------------------------
    xor  ecx, ecx
    lea  rdx, [cls_w]
    lea  r8,  [title_w]
    mov  r9d, WS_OVERLAPPEDWINDOW | WS_VISIBLE
    mov  dword [rsp+20h], 100
    mov  dword [rsp+28h], 80
    mov  dword [rsp+30h], 400
    mov  dword [rsp+38h], 300
    mov  qword [rsp+40h], 0
    mov  qword [rsp+48h], 0
    mov  qword [rsp+50h], 0
    mov  qword [rsp+58h], 0
    call CreateWindowExW
    test rax, rax
    jz   .f_create
    mov  [hwnd], rax
    cmp  dword [nccreate_seen], 1
    jne  .f_create
    cmp  dword [create_seen], 1
    jne  .f_create
    mov  rcx, [hwnd]
    call IsWindow
    test eax, eax
    jz   .f_create
    ; WS_CHILD: refused by name, because there is no child compositing
    xor  ecx, ecx
    lea  rdx, [cls_w]
    lea  r8,  [title_w]
    mov  r9d, WS_CHILD
    mov  qword [rsp+20h], 0
    mov  qword [rsp+28h], 0
    mov  qword [rsp+30h], 0
    mov  qword [rsp+38h], 0
    mov  qword [rsp+40h], 0
    mov  qword [rsp+48h], 0
    mov  qword [rsp+50h], 0
    mov  qword [rsp+58h], 0
    call CreateWindowExW
    test rax, rax
    jnz  .f_create
    call GetLastError
    cmp  eax, ERR_CALL_NOT_IMPLEMENTED
    jne  .f_create
    ; a style bit with no compositor meaning is refused, not ignored
    xor  ecx, ecx
    lea  rdx, [cls_w]
    lea  r8,  [title_w]
    mov  r9d, 0x20000000
    mov  qword [rsp+20h], 0
    mov  qword [rsp+28h], 0
    mov  qword [rsp+30h], 0
    mov  qword [rsp+38h], 0
    mov  qword [rsp+40h], 0
    mov  qword [rsp+48h], 0
    mov  qword [rsp+50h], 0
    mov  qword [rsp+58h], 0
    call CreateWindowExW
    test rax, rax
    jnz  .f_create
    call GetLastError
    cmp  eax, ERR_INVALID_PARAMETER
    jne  .f_create
    ; the class name comes back
    mov  rcx, [hwnd]
    lea  rdx, [wbuf]
    mov  r8d, 32
    call GetClassNameW
    cmp  eax, 5
    jne  .f_create
    lea  rsi, [m_create]
    mov  edi, m_create_l
    call puts_raw

    ; ---------------- geometry against the theme's own numbers ----------
    ; CreateWindowExW's cx/cy is the WHOLE window, as in Win32: the
    ; compositor reserves the theme's frame inside it.  So the client rect is
    ; the requested rect minus SM_CXFRAME/SM_CYCAPTION, and GetWindowRect
    ; gives the outer rect back.  Both numbers come from the same theme, so
    ; a mismatch here means the personality and the compositor disagree.
    mov  rcx, [hwnd]
    lea  rdx, [rect]
    call GetClientRect
    test eax, eax
    jz   .f_geom9
    mov  ecx, SM_CXFRAME
    call GetSystemMetrics
    shl  eax, 1
    mov  edx, 400
    sub  edx, eax
    cmp  [rect+8], edx
    jne  .f_geom1
    mov  ecx, SM_CYCAPTION
    call GetSystemMetrics
    mov  [geom_w], eax
    mov  ecx, SM_CXFRAME
    call GetSystemMetrics
    shl  eax, 1
    add  eax, [geom_w]
    mov  edx, 300
    sub  edx, eax
    cmp  [rect+12], edx
    jne  .f_geom2

    mov  rcx, [hwnd]
    lea  rdx, [rect2]
    call GetWindowRect
    test eax, eax
    jz   .f_geom9
    ; the numbers the compositor reported, in the log
    mov  eax, [rect+0]
    mov  [probe+0], eax
    mov  eax, [rect+4]
    mov  [probe+4], eax
    mov  eax, [rect+8]
    mov  [probe+8], eax
    mov  eax, [rect+12]
    mov  [probe+12], eax
    mov  eax, [rect2+0]
    mov  [probe+16], eax
    mov  eax, [rect2+4]
    mov  [probe+20], eax
    mov  eax, [rect2+8]
    mov  [probe+24], eax
    mov  eax, [rect2+12]
    mov  [probe+28], eax
    lea  rsi, [m_probe]
    mov  edi, m_probe_l
    call puts_raw
    mov  ecx, SM_CXSCREEN
    call GetSystemMetrics
    mov  [probe+32], eax
    mov  ecx, SM_CYSCREEN
    call GetSystemMetrics
    mov  [probe+36], eax
    lea  rsi, [probe]
    mov  ecx, 10
    call hexout
    cmp  dword [rect2+0], 100
    jne  .f_geom3
    cmp  dword [rect2+4], 80
    jne  .f_geom4
    cmp  dword [rect2+8], 500
    jne  .f_geom5
    cmp  dword [rect2+12], 380
    jne  .f_geom6

    ; the client's (0,0) sits one frame in from the window's corner
    mov  rcx, [hwnd]
    lea  rdx, [pt]
    mov  dword [pt+0], 0
    mov  dword [pt+4], 0
    call ClientToScreen
    mov  ecx, SM_CXFRAME
    call GetSystemMetrics
    add  eax, 100
    cmp  [pt+0], eax
    jne  .f_geom7
    mov  ecx, SM_CYCAPTION
    call GetSystemMetrics
    mov  [geom_h], eax                      ; rdx does not survive a call
    mov  ecx, SM_CXFRAME
    call GetSystemMetrics
    add  eax, [geom_h]
    add  eax, 80
    cmp  [pt+4], eax
    jne  .f_geom8
    lea  rsi, [m_geom]
    mov  edi, m_geom_l
    call puts_raw

    ; ---------------- subclassing ---------------------------------------
    mov  rcx, [hwnd]
    mov  edx, GWL_WNDPROC
    call GetWindowLongPtrW
    test rax, rax
    jz   .f_sub
    mov  [sub_prev], rax
    mov  rcx, [hwnd]
    mov  edx, GWL_WNDPROC
    lea  r8,  [wndproc_sub]
    call SetWindowLongPtrW
    test rax, rax
    jz   .f_sub
    mov  rcx, [hwnd]
    mov  edx, GWL_WNDPROC
    call GetWindowLongPtrW
    lea  r8,  [wndproc_sub]
    cmp  rax, r8
    jne  .f_sub
    ; the probe must reach the subclass *and* the class procedure below it
    mov  rcx, [hwnd]
    mov  edx, WM_APP
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  dword [sub_hits], 1
    jne  .f_sub
    cmp  dword [class_hits], 1
    jne  .f_sub
    lea  rsi, [m_sub]
    mov  edi, m_sub_l
    call puts_raw

    ; ---------------- window text ---------------------------------------
    mov  rcx, [hwnd]
    lea  rdx, [text_w]
    call SetWindowTextW
    test eax, eax
    jz   .f_text
    mov  rcx, [hwnd]
    lea  rdx, [wbuf]
    mov  r8d, 32
    call GetWindowTextW
    cmp  eax, 10                          ; "subclassed"
    jne  .f_text
    mov  rcx, [hwnd]
    call GetWindowTextLengthW
    cmp  eax, 10
    jne  .f_text
    lea  rsi, [m_text]
    mov  edi, m_text_l
    call puts_raw

    ; ---------------- paint and the update region -----------------------
    mov  rcx, [hwnd]
    xor  edx, edx
    mov  r8d, 1
    call InvalidateRect
    test eax, eax
    jz   .f_paint
    mov  rcx, [hwnd]
    xor  edx, edx
    xor  r8d, r8d
    call GetUpdateRgn
    cmp  eax, 1
    jne  .f_paint
    mov  rcx, [hwnd]
    lea  rdx, [ps]
    call BeginPaint
    test rax, rax
    jz   .f_paint
    cmp  dword [ps+12], 0                 ; rcPaint.left
    jne  .f_paint
    cmp  dword [ps+16], 0                 ; rcPaint.top
    jne  .f_paint
    mov  eax, [ps+20]                     ; rcPaint.right: the whole client,
    cmp  eax, [rect+8]                    ; which the compositor sized
    jne  .f_paint
    mov  eax, [ps+24]
    cmp  eax, [rect+12]
    jne  .f_paint
    mov  rcx, [hwnd]
    lea  rdx, [ps]
    call EndPaint
    mov  rcx, [hwnd]
    xor  edx, edx
    xor  r8d, r8d
    call GetUpdateRgn
    test eax, eax
    jnz  .f_paint
    ; RedrawWindow with RDW_UPDATENOW must paint inside the call
    mov  eax, [paint_count]
    mov  [rect+0], eax
    mov  rcx, [hwnd]
    xor  edx, edx
    xor  r8d, r8d
    mov  r9d, RDW_INVALIDATE | RDW_UPDATENOW
    call RedrawWindow
    test eax, eax
    jz   .f_paint
    mov  eax, [paint_count]
    cmp  eax, [rect+0]
    jle  .f_paint
    lea  rsi, [m_paint]
    mov  edi, m_paint_l
    call puts_raw

    ; ---------------- the pixels a VNC screenshot can check --------------
    ; Paint the whole client green through a DC, then print the screen
    ; coordinates the gate should look at.  The compositor's own z-order,
    ; focus and capture logic is what the earlier sections drove; this is the
    ; part that proves the paint really reached the surface.
    mov  rcx, [hwnd]
    call GetDC
    test rax, rax
    jz   .f_pixel
    mov  [dc_tmp], rax
    mov  ecx, PROBE_COLORREF
    call CreateSolidBrush
    mov  r8, rax                           ; the brush
    mov  rcx, [dc_tmp]
    lea  rdx, [rect]                       ; the client rect
    call FillRect
    mov  eax, [rect+8]
    sar  eax, 1
    mov  ecx, SM_CXFRAME
    mov  [geom_w], eax
    call GetSystemMetrics
    add  eax, 100
    add  eax, [geom_w]                     ; client centre, screen x
    mov  [probe+0], eax
    mov  eax, [rect+12]
    sar  eax, 1
    mov  [geom_w], eax
    mov  ecx, SM_CYCAPTION
    call GetSystemMetrics
    mov  [geom_h], eax
    mov  ecx, SM_CXFRAME
    call GetSystemMetrics
    add  eax, [geom_h]
    add  eax, 80
    add  eax, [geom_w]                     ; client centre, screen y
    mov  [probe+4], eax
    mov  dword [probe+8], PROBE_RGB
    lea  rsi, [m_pixel]
    mov  edi, m_pixel_l
    call puts_raw
    lea  rsi, [probe]
    mov  ecx, 3
    call hexout
    lea  rsi, [m_visible]
    mov  edi, m_visible_l
    call puts_raw
    ; hold still for the screenshot -- and report how long the hold really
    ; took, because a guest tick that lags real time makes every budget in
    ; the gate a guess (GetTickCount64 is CLOCK_MONOTONIC, the same clock
    ; Sleep's deadline loop uses).
    call GetTickCount64
    mov  [t0], rax
    mov  ecx, HOLD_MS
    call Sleep
    call GetTickCount64
    sub  rax, [t0]
    mov  [probe+0], eax
    lea  rsi, [m_sleep]
    mov  edi, m_sleep_l
    call puts_raw
    lea  rsi, [probe]
    mov  ecx, 1
    call hexout

    ; ---------------- scroll --------------------------------------------
    mov  dword [sinfo+0], 28
    mov  dword [sinfo+4], SIF_ALL
    mov  dword [sinfo+8], 0
    mov  dword [sinfo+12], 100
    mov  dword [sinfo+16], 10
    mov  dword [sinfo+20], 30
    mov  dword [sinfo+24], 0
    mov  rcx, [hwnd]
    mov  edx, SB_VERT
    lea  r8,  [sinfo]
    mov  r9d, 0
    call SetScrollInfo
    mov  rcx, [hwnd]
    mov  edx, SB_VERT
    lea  r8,  [sinfo]
    call GetScrollInfo
    test eax, eax
    jz   .f_scroll
    cmp  dword [sinfo+12], 100
    jne  .f_scroll
    cmp  dword [sinfo+20], 30
    jne  .f_scroll
    mov  rcx, [hwnd]
    mov  edx, SB_VERT
    mov  r8d, 500                          ; beyond nMax: must clamp
    xor  r9d, r9d
    call SetScrollPos
    cmp  eax, 30                           ; ...and return the old position
    jne  .f_scroll
    mov  rcx, [hwnd]
    mov  edx, SB_VERT
    call GetScrollPos
    cmp  eax, 100
    jne  .f_scroll
    ; a vertical scroll bares a strip at the bottom of the client area
    mov  rcx, [hwnd]
    xor  edx, edx
    mov  r8d, -8
    xor  r9d, r9d
    mov  qword [rsp+20h], 0
    call ScrollWindow
    test eax, eax
    jz   .f_scroll
    mov  rcx, [hwnd]
    xor  edx, edx
    xor  r8d, r8d
    call GetUpdateRgn
    cmp  eax, 1
    jne  .f_scroll
    mov  rcx, [hwnd]
    xor  edx, edx
    call ValidateRect
    lea  rsi, [m_scroll]
    mov  edi, m_scroll_l
    call puts_raw

    ; ---------------- focus, capture, Z-order ---------------------------
    mov  rcx, [hwnd]
    call SetCapture
    test rax, rax                          ; nothing owned it: NULL
    jnz  .f_zorder
    call GetCapture
    cmp  rax, [hwnd]
    jne  .f_zorder
    call ReleaseCapture
    call GetCapture
    test rax, rax
    jnz  .f_zorder
    mov  rcx, [hwnd]
    call SetFocus
    call GetFocus
    cmp  rax, [hwnd]
    jne  .f_zorder
    mov  rcx, [hwnd]
    mov  rdx, -1                           ; HWND_TOPMOST
    xor  r8d, r8d
    xor  r9d, r9d
    mov  qword [rsp+20h], 0
    mov  qword [rsp+28h], 0
    mov  dword [rsp+30h], SWP_NOSIZE | SWP_NOMOVE | SWP_NOREDRAW
    call SetWindowPos
    test eax, eax
    jz   .f_zorder
    mov  rcx, [hwnd]
    call BringWindowToTop
    test eax, eax
    jz   .f_zorder
    lea  rsi, [m_zorder]
    mov  edi, m_zorder_l
    call puts_raw

    ; ---------------- metrics and colours from the theme ----------------
    ; The compositor is the single source: the frame numbers come from the
    ; theme, and the phase gate re-runs this binary under a tinted theme to
    ; prove they follow it rather than being constants.
    mov  ecx, SM_CMONITORS
    call GetSystemMetrics
    cmp  eax, 1
    jne  .f_metric1
    mov  ecx, SM_CYCAPTION
    call GetSystemMetrics
    test eax, eax
    jle  .f_metric2
    mov  [geom_w], eax                     ; the live titlebar height
    mov  ecx, SM_CXFRAME
    call GetSystemMetrics
    test eax, eax
    jle  .f_metric3
    mov  [geom_h], eax                     ; the live border width
    ; The work area and SM_CYFULLSCREEN are one number from one source, and
    ; the work area cannot be wider than the screen.  A lane with no linear
    ; framebuffer reports a 0x0 screen and both numbers go negative
    ; together, so the relations hold on every lane.
    mov  ecx, SPI_GETWORKAREA
    xor  edx, edx
    lea  r8,  [rect2]
    xor  r9d, r9d
    call SystemParametersInfoW
    test eax, eax
    jz   .f_metric4
    mov  ecx, SM_CXSCREEN
    call GetSystemMetrics
    cmp  [rect2+8], eax                    ; work area right
    jne  .f_metric5
    mov  ecx, SM_CYFULLSCREEN
    call GetSystemMetrics
    cmp  eax, [rect2+12]                   ; work area bottom
    jne  .f_metric6
    mov  ecx, COLOR_WINDOW
    call GetSysColor
    test eax, eax
    jz   .f_metric7
    mov  qword [color_tmp], 0              ; a 32-bit return leaves rax's
    mov  dword [color_tmp], eax            ; top half undefined
    mov  ecx, COLOR_WINDOW
    call GetSysColorBrush
    cmp  rax, [color_tmp]
    jne  .f_metric8
    ; the live numbers, for the gate's second boot
    lea  rsi, [m_theme]
    mov  edi, m_theme_l
    call puts_raw
    mov  eax, [geom_w]
    mov  [probe+0], eax
    mov  eax, [geom_h]
    mov  [probe+4], eax
    mov  ecx, COLOR_ACTIVECAPTION
    call GetSysColor
    mov  [probe+8], eax
    mov  ecx, COLOR_WINDOW
    call GetSysColor
    mov  [probe+12], eax
    lea  rsi, [probe]
    mov  ecx, 4
    call hexout
    lea  rsi, [m_metric]
    mov  edi, m_metric_l
    call puts_raw

    ; ---------------- the single monitor ---------------------------------
    mov  rcx, [hwnd]
    mov  edx, MONITOR_DEFAULTTONEAREST
    call MonitorFromWindow
    test rax, rax
    jz   .f_monitor
    mov  rcx, rax
    lea  rdx, [mi]
    call GetMonitorInfoW
    test eax, eax
    jz   .f_monitor
    mov  eax, [mi+8]                       ; rcMonitor.right
    cmp  eax, [rect+0]
    jne  .f_monitor
    mov  eax, [mi+12]                      ; rcMonitor.bottom
    cmp  eax, [rect+4]
    jne  .f_monitor
    lea  rsi, [m_monitor]
    mov  edi, m_monitor_l
    call puts_raw

    ; ---------------- input state ----------------------------------------
    mov  ecx, 'A'
    xor  edx, edx
    call MapVirtualKeyW
    mov  [probe+0], eax                    ; VK 'A' -> scan code
    cmp  eax, 0x1E
    jne  .f_input1
    ; ToAscii on the whole keyboard state: shift makes it upper case
    lea  rdi, [keystate]
    xor  eax, eax
    mov  ecx, 32
.zk: mov [rdi], rax
    add  rdi, 8
    dec  ecx
    jnz  .zk
    mov  byte [keystate+16], 0x80          ; VK_SHIFT
    mov  ecx, 'A'
    xor  edx, edx
    lea  r8,  [keystate]
    lea  r9,  [out_ch]
    mov  qword [rsp+20h], 0
    call ToAscii
    mov  [probe+4], eax                    ; translated keystrokes
    movzx eax, word [out_ch]
    mov  [probe+8], eax                    ; the character itself
    cmp  dword [probe+4], 1
    jne  .f_input2
    cmp  dword [probe+8], 'A'
    jne  .f_input2
    lea  rcx, [pt]
    call GetCursorPos
    mov  [probe+12], eax                   ; the pointer, if the lane has one
    call GetLastError
    mov  [probe+16], eax
    lea  rsi, [m_input_hdr]
    mov  edi, m_input_hdr_l
    call puts_raw
    lea  rsi, [probe]
    mov  ecx, 5
    call hexout
    cmp  dword [probe+12], 1
    jne  .f_input3
    lea  rsi, [m_input_ok]
    mov  edi, m_input_ok_l
    call puts_raw

    ; ---------------- cross-thread messages ------------------------------
    xor  ecx, ecx
    xor  edx, edx
    lea  r8,  [worker_main]
    xor  r9d, r9d
    mov  qword [rsp+20h], 0
    mov  qword [rsp+28h], 0
    call CreateThread
    test rax, rax
    jz   .f_thread1
    mov  [work_thread], rax

    call GetTickCount64                     ; the worker must come up in time
    mov  [t0], rax
.wait_ready:
    mov  eax, [ready_flag]
    test eax, eax
    jnz  .ready
    call GetTickCount64
    sub  rax, [t0]
    cmp  rax, 15000
    jb   .wait_ready
    mov  dword [ready_flag], 0FFFFFF03h     ; it never came up

.ready:
    ; Whatever the worker reported, in the log: the gate reads these numbers
    ; when the section fails, exactly like the geometry probe above.
    mov  eax, [ready_flag]
    mov  [probe+0], eax
    mov  eax, [work_err]
    mov  [probe+4], eax
    mov  rax, [work_tid]
    mov  [probe+8], eax
    mov  rax, [work_hwnd]
    mov  [probe+12], eax
    lea  rsi, [m_thread_hdr]
    mov  edi, m_thread_hdr_l
    call puts_raw
    lea  rsi, [probe]
    mov  ecx, 4
    call hexout

    mov  eax, [ready_flag]
    cmp  eax, 1
    jne  .f_thread2                         ; the worker failed a stage
    ; the corner of the window it created, so the gate can see which surface
    ; the cross-thread call landed on
    mov  rcx, [work_hwnd]
    xor  edx, edx
    call GetWindowThreadProcessId
    mov  [probe+16], eax
    cmp  rax, [work_tid]
    jne  .f_thread3
    call GetCurrentThreadId
    mov  [probe+20], eax
    cmp  rax, [work_tid]
    je   .f_thread4
    ; the send: blocks until the worker's procedure returns wParam+1
    mov  rcx, [work_hwnd]
    mov  edx, PROBE_MSG
    mov  r8d, PROBE_ARG
    xor  r9d, r9d
    call SendMessageW
    mov  [probe+24], eax
    mov  rax, [work_proc_tid]
    mov  [probe+28], eax
    lea  rsi, [m_thread_send_hdr]
    mov  edi, m_thread_send_hdr_l
    call puts_raw
    lea  rsi, [probe]
    mov  ecx, 8
    call hexout
    cmp  dword [probe+24], PROBE_REPLY
    jne  .f_thread5
    mov  rax, [work_proc_tid]
    cmp  rax, [work_tid]
    jne  .f_thread6
    ; a posted quit ends the worker's loop; join it before leaving
    mov  rcx, [work_hwnd]
    mov  edx, WM_QUIT
    xor  r8d, r8d
    xor  r9d, r9d
    call PostMessageW
    test eax, eax
    jz   .f_thread7
    mov  rcx, [work_thread]
    mov  edx, INFINITE
    call WaitForSingleObject
    lea  rsi, [m_thread]
    mov  edi, m_thread_l
    call puts_raw

    ; ---------------- done -----------------------------------------------
    mov  rcx, [hwnd]
    call DestroyWindow
    lea  rsi, [m_ok]
    mov  edi, m_ok_l
    call puts_raw
    mov  ecx, 78
    call ExitProcess
    hlt

.f_cls:
    lea  rsi, [f_cls]
    mov  edi, f_cls_l
    jmp  .fail
.f_create:
    lea  rsi, [f_create]
    mov  edi, f_create_l
    jmp  .fail
.f_geom1:
    lea  rsi, [f_geom1]
    mov  edi, f_geom1_l
    jmp  .fail
.f_geom2:
    lea  rsi, [f_geom2]
    mov  edi, f_geom2_l
    jmp  .fail
.f_geom3:
    lea  rsi, [f_geom3]
    mov  edi, f_geom3_l
    jmp  .fail
.f_geom4:
    lea  rsi, [f_geom4]
    mov  edi, f_geom4_l
    jmp  .fail
.f_geom5:
    lea  rsi, [f_geom5]
    mov  edi, f_geom5_l
    jmp  .fail
.f_geom6:
    lea  rsi, [f_geom6]
    mov  edi, f_geom6_l
    jmp  .fail
.f_geom7:
    lea  rsi, [f_geom7]
    mov  edi, f_geom7_l
    jmp  .fail
.f_geom8:
    lea  rsi, [f_geom8]
    mov  edi, f_geom8_l
    jmp  .fail
.f_geom9:
    lea  rsi, [f_geom9]
    mov  edi, f_geom9_l
    jmp  .fail
.f_sub:
    lea  rsi, [f_sub]
    mov  edi, f_sub_l
    jmp  .fail
.f_text:
    lea  rsi, [f_text]
    mov  edi, f_text_l
    jmp  .fail
.f_pixel:
    lea  rsi, [f_pixel]
    mov  edi, f_pixel_l
    jmp  .fail
.f_paint:
    lea  rsi, [f_paint]
    mov  edi, f_paint_l
    jmp  .fail
.f_scroll:
    lea  rsi, [f_scroll]
    mov  edi, f_scroll_l
    jmp  .fail
.f_zorder:
    lea  rsi, [f_zorder]
    mov  edi, f_zorder_l
    jmp  .fail
.f_metric1:
    lea  rsi, [f_metric1]
    mov  edi, f_metric1_l
    jmp  .fail
.f_metric2:
    lea  rsi, [f_metric2]
    mov  edi, f_metric2_l
    jmp  .fail
.f_metric3:
    lea  rsi, [f_metric3]
    mov  edi, f_metric3_l
    jmp  .fail
.f_metric4:
    lea  rsi, [f_metric4]
    mov  edi, f_metric4_l
    jmp  .fail
.f_metric5:
    lea  rsi, [f_metric5]
    mov  edi, f_metric5_l
    jmp  .fail
.f_metric6:
    lea  rsi, [f_metric6]
    mov  edi, f_metric6_l
    jmp  .fail
.f_metric7:
    lea  rsi, [f_metric7]
    mov  edi, f_metric7_l
    jmp  .fail
.f_metric8:
    lea  rsi, [f_metric8]
    mov  edi, f_metric8_l
    jmp  .fail
.f_metric9:
    lea  rsi, [f_metric9]
    mov  edi, f_metric9_l
    jmp  .fail
.f_monitor:
    lea  rsi, [f_monitor]
    mov  edi, f_monitor_l
    jmp  .fail
.f_input1:
    lea  rsi, [f_input1]
    mov  edi, f_input1_l
    jmp  .fail
.f_input2:
    lea  rsi, [f_input2]
    mov  edi, f_input2_l
    jmp  .fail
.f_input3:
    lea  rsi, [f_input3]
    mov  edi, f_input3_l
    jmp  .fail
.f_thread1:
    lea  rsi, [f_thread1]
    mov  edi, f_thread1_l
    jmp  .fail
.f_thread2:
    lea  rsi, [f_thread2]
    mov  edi, f_thread2_l
    jmp  .fail
.f_thread3:
    lea  rsi, [f_thread3]
    mov  edi, f_thread3_l
    jmp  .fail
.f_thread4:
    lea  rsi, [f_thread4]
    mov  edi, f_thread4_l
    jmp  .fail
.f_thread5:
    lea  rsi, [f_thread5]
    mov  edi, f_thread5_l
    jmp  .fail
.f_thread6:
    lea  rsi, [f_thread6]
    mov  edi, f_thread6_l
    jmp  .fail
.f_thread7:
    lea  rsi, [f_thread7]
    mov  edi, f_thread7_l
    jmp  .fail
.fail:
    call puts_raw
    mov  ecx, 79
    call ExitProcess
    hlt
start_end:
