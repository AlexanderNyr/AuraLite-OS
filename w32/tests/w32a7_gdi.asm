; w32a7_gdi.asm — W32APP_PLAN.md phase W32A-7 guest gate.
;
; One host window plus its DC, and the GDI surface drawn end to end:
; caps (GetDeviceCaps, including the DPI contract -- this image has no
; dpiAware manifest, so it must see LOGPIXELSX = 96 whatever the theme
; says), stock objects and pen/brush lifetimes (GetStockObject /
; CreateSolidBrush / CreatePen / SelectObject round-trip / GetObjectW
; LOGPEN / DeleteObject idempotence), a 64x48 compatible bitmap drawn
; into (PatBlt WHITENESS, SetPixel/GetPixel, MoveToEx/LineTo, Rectangle,
; Ellipse, Polygon, SetROP2/GetROP2, SaveDC/RestoreDC with the text
; colour restored), text through the shipped PSF2 font (SetTextColor /
; SetBkMode previous-value contracts, TextOutA, GetTextMetricsA height
; 16 / ave 8, GetTextExtentPoint32A), regions (CreateRectRgn,
; CombineRgn AND, SelectClipRgn / GetClipRgn / RectVisible /
; ExcludeClipRect), DIBs (CreateDIBSection 32bpp top-down, the BGRA word
; the engine writes, GetDIBits' bottom-up row flip, SetDIBits back),
; palettes (CreatePalette 4 entries realized into an 8bpp section,
; GetPixel reading through the realised table, SetPaletteEntries +
; re-realise, UpdateColors, UnrealizeObject), and the window-DC blit
; path (BitBlt SRCCOPY from a memory DC onto the window DC + FillRect,
; each read back with GetPixel).  Every section prints A7-<NAME>-OK
; after all its checks, or A7-<NAME>-FAIL and exits 79.  The final
; W32A7-GDI-OK plus exit 78 is what
; tests/integration/cases/test_w32a7_gdi.sh asserts; exit 1 means the
; personality refused to load the image at all.
;
; Two contracts worth calling out because the host gate proved them and
; this fixture depends on them:
;
;   * The metrics this image may assert are the PSF2 font's: 8x16, so
;     GetTextMetricsA reports tmHeight 16 / tmAveCharWidth 8 and a
;     5-char extent is 40x16.  Anything else means the fixture and the
;     engine disagree about the shipped font, not about Win32.
;   * This PE carries no manifest, so w32_manifest.c recorded it as
;     DPI-unaware and GetDeviceCaps(LOGPIXELSX) must report 96 (the
;     gdi32.h comment: "unaware apps see LOGPIXELSX/Y = 96").  The
;     DPI-aware path is exercised on the host with a manifest fixture;
;     here the check is that the unaware default holds.
;
; The object-model section runs on a memory DC (CreateCompatibleDC),
; not the window DC: that is where the host gate exercised it, and a
; window DC's selected-object state belongs to the compositor's common
; DC.  The window DC is used only for what the host gate validated on
; it: caps, BitBlt SRCCOPY from a memory source, FillRect, GetPixel.

bits 64
default rel

; ---- kernel32 imports --------------------------------------------------------
extern GetStdHandle
extern WriteFile
extern ExitProcess
extern GetModuleHandleW

; ---- user32 imports ----------------------------------------------------------
extern RegisterClassExW
extern CreateWindowExW
extern ShowWindow
extern GetDC
extern DefWindowProcW
extern PostQuitMessage
extern DestroyWindow
extern FillRect

; ---- gdi32 imports -----------------------------------------------------------
extern GetDeviceCaps
extern CreateCompatibleDC
extern CreateCompatibleBitmap
extern DeleteDC
extern DeleteObject
extern GetStockObject
extern CreateSolidBrush
extern CreatePen
extern SelectObject
extern GetObjectW
extern SaveDC
extern RestoreDC
extern SetROP2
extern GetROP2
extern PatBlt
extern SetPixel
extern GetPixel
extern MoveToEx
extern LineTo
extern Rectangle
extern Ellipse
extern Polygon
extern SetTextColor
extern SetBkMode
extern TextOutA
extern GetTextMetricsA
extern GetTextExtentPoint32A
extern CreateRectRgn
extern CombineRgn
extern SelectClipRgn
extern GetClipRgn
extern ExcludeClipRect
extern RectVisible
extern CreateDIBSection
extern GetDIBits
extern SetDIBits
extern CreatePalette
extern SelectPalette
extern RealizePalette
extern SetPaletteEntries
extern UpdateColors
extern UnrealizeObject
extern BitBlt

; ---- constants (mirror w32/include/w32/*.h) ----------------------------------
%define STD_OUTPUT_HANDLE -11

%define WM_DESTROY     0x0002
%define WM_PAINT       0x000F

%define WS_CAPTION     0x00C00000
%define WS_SYSMENU     0x00080000
%define WS_VISIBLE     0x10000000
%define SW_SHOW        5

; GetDeviceCaps indices
%define HORZRES        8
%define VERTRES        10
%define BITSPIXEL      12
%define LOGPIXELSX     88

; stock objects
%define WHITE_BRUSH    0
%define BLACK_PEN      7
%define NULL_PEN       8

; ROPs
%define SRCCOPY        0x00CC0020
%define WHITENESS      0x00FF0062

; pen / brush
%define PS_SOLID       0

; text
%define TRANSPARENT    1
%define OPAQUE         2
%define R2_COPYPEN     13

; regions
%define RGN_AND        1
%define SIMPLEREGION   2
%define NULLREGION     1

; DIB
%define DIB_RGB_COLORS 0

; colours are COLORREF 0x00BBGGRR
%define RGB_RED        0x000000FF
%define RGB_BLUE       0x00FF0000
%define RGB_YELLOW     0x0000FFFF

; ---- structures (must mirror w32/include/w32/gdi32.h byte for byte) ----------
struc WNDCLASSEXW
    .cbSize         resd 1
    .style          resd 1
    .lpfnWndProc    resq 1
    .cbClsExtra     resd 1
    .cbWndExtra     resd 1
    .hInstance      resq 1
    .hIcon          resq 1
    .hCursor        resq 1
    .hbrBackground  resq 1
    .lpszMenuName   resq 1
    .lpszClassName  resq 1
    .hIconSm        resq 1
endstruc

struc POINT
    .x resd 1
    .y resd 1
endstruc

struc RECT
    .left   resd 1
    .top    resd 1
    .right  resd 1
    .bottom resd 1
endstruc

; LOGPEN { u32 style; POINT width; u32 color } = 16 bytes
struc LOGPEN
    .lopnStyle resd 1
    .lopnWidth resb POINT_size
    .lopnColor resd 1
endstruc

; TEXTMETRICA: 11 int32s, 4 uint16s, 4 bytes, then tmCharSet
struc TEXTMETRICA
    .tmHeight           resd 1    ; 0
    .tmAscent           resd 1    ; 4
    .tmDescent          resd 1    ; 8
    .tmInternalLeading  resd 1    ; 12
    .tmExternalLeading  resd 1    ; 16
    .tmAveCharWidth     resd 1    ; 20
    .tmMaxCharWidth     resd 1    ; 24
    .tmWeight           resd 1    ; 28
    .tmOverhang         resd 1    ; 32
    .tmDigitizedAspectX resd 1    ; 36
    .tmDigitizedAspectY resd 1    ; 40
    .tmFirstChar        resw 1    ; 44
    .tmLastChar         resw 1
    .tmDefaultChar      resw 1
    .tmBreakChar        resw 1
    .tmItalic           resb 1
    .tmUnderlined       resb 1
    .tmStruckOut        resb 1
    .tmPitchAndFamily   resb 1
    .tmCharSet          resb 1
endstruc

; BITMAPINFOHEADER = 40 bytes
struc BITMAPINFOHEADER
    .biSize          resd 1
    .biWidth         resd 1
    .biHeight        resd 1
    .biPlanes        resw 1
    .biBitCount      resw 1
    .biCompression   resd 1
    .biSizeImage     resd 1
    .biXPelsPerMeter resd 1
    .biYPelsPerMeter resd 1
    .biClrUsed       resd 1
    .biClrImportant  resd 1
endstruc

; LOGPALETTE { u16 version; u16 count; PALETTEENTRY entries[] }
struc LOGPALETTE
    .palVersion   resw 1
    .palNumEntries resw 1
endstruc
; PALETTEENTRY = 4 bytes: red, green, blue, flags

; ---- data ---------------------------------------------------------------------
section .data align=8

g_hInst      dq 0
g_stdout     dq 0
g_hwnd       dq 0
g_dc         dq 0                  ; the window DC
g_memdc      dq 0                  ; the memory DC everything else uses
g_hbmp       dq 0                  ; 64x48 compatible bitmap
g_hbmpOld    dq 0
g_hdib       dq 0                  ; 8x4 32bpp DIB section
g_hdibOld    dq 0
g_dibBits    dq 0
g_hpal       dq 0
g_hpalOld    dq 0
g_hpen       dq 0
g_hpenOld    dq 0
g_hbr        dq 0
g_rgn1       dq 0
g_rgn2       dq 0
g_rgndst     dq 0
g_rgnclip    dq 0

g_classHost  dw __utf16__('A7Gdi'),0
g_winTitle   dw __utf16__('W32A7 GDI'),0

g_txt        db 'W32A7gdi',0        ; TextOutA payload (len passed as 8)

; a triangle for Polygon
g_poly       dd 4,4, 20,4, 12,18

; 32bpp top-down DIB header: 8x4
align 8
g_bmi32:
    istruc BITMAPINFOHEADER
    at BITMAPINFOHEADER.biSize,     dd 40
    at BITMAPINFOHEADER.biWidth,    dd 8
    at BITMAPINFOHEADER.biHeight,   dd -4          ; top-down
    at BITMAPINFOHEADER.biPlanes,   dw 1
    at BITMAPINFOHEADER.biBitCount, dw 32
    iend
; GetDIBits fills this with biHeight = +4 (bottom-up readback)
align 8
g_bmi32rd:
    istruc BITMAPINFOHEADER
    at BITMAPINFOHEADER.biSize,     dd 40
    at BITMAPINFOHEADER.biWidth,    dd 8
    at BITMAPINFOHEADER.biHeight,   dd 4           ; positive = flip rows
    at BITMAPINFOHEADER.biPlanes,   dw 1
    at BITMAPINFOHEADER.biBitCount, dw 32
    iend

; 8bpp DIB header for the palette section: 4x4
align 8
g_bmi8:
    istruc BITMAPINFOHEADER
    at BITMAPINFOHEADER.biSize,     dd 40
    at BITMAPINFOHEADER.biWidth,    dd 4
    at BITMAPINFOHEADER.biHeight,   dd -4
    at BITMAPINFOHEADER.biPlanes,   dw 1
    at BITMAPINFOHEADER.biBitCount, dw 8
    iend

; LOGPALETTE + 4 entries: red, green, blue, black
align 4
g_logpal:
    dw 0x0300, 4
    db 255,0,0,0
    db 0,255,0,0
    db 0,0,255,0
    db 0,0,0,0
g_logpal_end:
LOGPAL_SIZE equ g_logpal_end - g_logpal

; one replacement entry for SetPaletteEntries(index 2): yellow
g_palent     db 255,255,0,0

; ---- bss -----------------------------------------------------------------------
section .bss align=8
g_wcx        resb WNDCLASSEXW_size
g_lp         resb LOGPEN_size
g_tm         resb TEXTMETRICA_size
g_extent     resd 2                  ; SIZE { cx, cy }
g_rect       resb RECT_size
g_rectvis    resb RECT_size
g_dibbuf     resd 32                 ; GetDIBits readback (8x4 dwords)
g_pt         resb POINT_size
g_written    resq 1

; ---- console helpers -------------------------------------------------------------
section .text

; p_ok: rsi = marker bytes, edx = length.
p_ok:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x40
    mov  rcx, [g_stdout]
    mov  r8d, edx
    mov  rdx, rsi
    lea  r9, [g_written]
    mov  qword [rsp+0x20], 0
    call WriteFile
    add  rsp, 0x40
    pop  rbp
    ret

; p_fail: print the marker, then exit 79.  The sub 8 is not optional:
; p_fail is entered with rsp%16==8 like any callee, and an unadjusted
; call p_ok would enter p_ok with rsp%16==0 -- p_ok's own sub 0x40 keeps
; it there, and WriteFile's movaps prologue then faults on a misaligned
; store.  (Same latent bug shipped in w32a6_dlg.asm, whose gate never
; ran far enough to reach it.)
p_fail:
    sub  rsp, 8
    call p_ok
    mov  ecx, 79
    call ExitProcess                ; never returns; the 8 stays

%macro OK 2
    lea  rsi, [%1]
    mov  edx, %2
    call p_ok
%endmacro

%macro FAIL 2
    lea  rsi, [%1]
    mov  edx, %2
    call p_fail
%endmacro

; ---- window procedure -------------------------------------------------------------
; rcx = hwnd, edx = msg, r8 = wParam, r9 = lParam.  Must not touch
; r12-r15/rbx (main's).
a7_wndproc:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x20
    cmp  edx, WM_DESTROY
    je   .wm_destroy
    call DefWindowProcW
    leave
    ret
.wm_destroy:
    xor  ecx, ecx                    ; PostQuitMessage(0); rcx was hwnd
    call PostQuitMessage
    xor  eax, eax
    leave
    ret

; ---- main ---------------------------------------------------------------------------
global mainCRTStartup
mainCRTStartup:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0xB0

    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    test rax, rax
    jz   .f_stdout
    mov  [g_stdout], rax

    xor  ecx, ecx
    call GetModuleHandleW
    test rax, rax
    jz   .f_getmodule
    mov  [g_hInst], rax

    ; ---- section 1: the window DC and its caps ------------------------------
    ; register the class
    lea  rdi, [g_wcx]
    xor  eax, eax
    mov  ecx, WNDCLASSEXW_size/8
.zl:
    mov  [rdi], rax
    add  rdi, 8
    dec  ecx
    jnz  .zl
    mov  dword [g_wcx+WNDCLASSEXW.cbSize], WNDCLASSEXW_size
    lea  rax, [a7_wndproc]
    mov  [g_wcx+WNDCLASSEXW.lpfnWndProc], rax
    mov  rax, [g_hInst]
    mov  [g_wcx+WNDCLASSEXW.hInstance], rax
    mov  qword [g_wcx+WNDCLASSEXW.hbrBackground], 0x00FFFFFF
    lea  rax, [g_classHost]
    mov  [g_wcx+WNDCLASSEXW.lpszClassName], rax
    lea  rcx, [g_wcx]
    call RegisterClassExW
    test eax, eax
    jz   .f_regclass

    ; create the host window (same shape as A6's)
    xor  ecx, ecx                      ; exStyle
    lea  rdx, [g_classHost]
    lea  r8, [g_winTitle]
    mov  r9d, WS_CAPTION | WS_SYSMENU | WS_VISIBLE
    mov  dword [rsp+0x20], 40          ; x
    mov  dword [rsp+0x28], 40          ; y
    mov  dword [rsp+0x30], 260         ; w
    mov  dword [rsp+0x38], 160         ; h
    mov  qword [rsp+0x40], 0           ; parent
    mov  qword [rsp+0x48], 0           ; menu
    mov  rax, [g_hInst]
    mov  [rsp+0x50], rax               ; instance
    mov  qword [rsp+0x58], 0           ; param
    call CreateWindowExW
    test rax, rax
    jz   .f_createwin
    mov  [g_hwnd], rax
    mov  rcx, [g_hwnd]
    mov  edx, SW_SHOW
    call ShowWindow

    mov  rcx, [g_hwnd]
    call GetDC
    test rax, rax
    jz   .f_getdc
    mov  [g_dc], rax

    ; caps: the screen is real, 32bpp, and this unaware image sees 96 DPI
    mov  rcx, [g_dc]
    mov  edx, HORZRES
    call GetDeviceCaps
    test eax, eax
    jle  .f_caps
    mov  [g_extent], eax               ; remember HORZRES for VERTRES sanity
    mov  rcx, [g_dc]
    mov  edx, VERTRES
    call GetDeviceCaps
    test eax, eax
    jle  .f_caps
    mov  rcx, [g_dc]
    mov  edx, BITSPIXEL
    call GetDeviceCaps
    cmp  eax, 32
    jne  .f_caps
    mov  rcx, [g_dc]
    mov  edx, LOGPIXELSX
    call GetDeviceCaps
    cmp  eax, 96                       ; no manifest -> unaware -> 96
    jne  .f_caps

    ; the memory DC the object model runs on
    mov  rcx, [g_dc]
    call CreateCompatibleDC
    test rax, rax
    jz   .f_memdc
    mov  [g_memdc], rax
    OK s_dc_ok, s_dc_ok_l

    ; ---- section 2: objects --------------------------------------------------
    ; every stock object this phase consumes mints a handle
    mov  ecx, WHITE_BRUSH
    call GetStockObject
    test rax, rax
    jz   .f_object
    mov  ecx, BLACK_PEN
    call GetStockObject
    test rax, rax
    jz   .f_object
    mov  ecx, NULL_PEN
    call GetStockObject
    test rax, rax
    jz   .f_object

    ; pen + brush, select round-trip, GetObjectW LOGPEN, delete twice
    mov  ecx, RGB_RED
    call CreateSolidBrush
    test rax, rax
    jz   .f_object
    mov  [g_hbr], rax

    xor  ecx, ecx                      ; PS_SOLID
    mov  edx, 1                        ; width
    mov  r8d, RGB_RED
    call CreatePen
    test rax, rax
    jz   .f_object
    mov  [g_hpen], rax

    ; a fresh memory DC has no pen selected: the FIRST SelectObject
    ; returns NULL (the host suite pins this), the second returns the
    ; pen we put in -- the round-trip the gate wants.
    mov  rcx, [g_memdc]
    mov  rdx, [g_hpen]
    call SelectObject
    test rax, rax                      ; fresh DC: previous pen is NULL
    jnz  .f_object
    mov  rcx, [g_memdc]
    mov  rdx, [g_hpen]
    call SelectObject
    cmp  rax, [g_hpen]                 ; now the previous is our pen
    jne  .f_object

    mov  rcx, [g_hpen]
    mov  edx, LOGPEN_size
    lea  r8, [g_lp]
    call GetObjectW
    cmp  eax, LOGPEN_size
    jne  .f_object
    mov  eax, [g_lp+LOGPEN.lopnColor]
    cmp  eax, RGB_RED
    jne  .f_object

    ; put a stock pen back before deleting ours, then delete twice:
    ; the engine's A-5 contract is that a second delete is a no-op
    ; SUCCESS (same assertion as the host suite).
    mov  ecx, BLACK_PEN
    call GetStockObject
    test rax, rax
    jz   .f_object
    mov  rcx, [g_memdc]
    mov  rdx, rax
    call SelectObject
    cmp  rax, [g_hpen]                 ; it replaced our pen
    jne  .f_object
    mov  rcx, [g_hpen]
    call DeleteObject
    test eax, eax
    jz   .f_object
    mov  rcx, [g_hpen]
    call DeleteObject
    test eax, eax
    jz   .f_object                     ; no-op success, still TRUE
    OK s_object_ok, s_object_ok_l

    ; ---- section 3: the bitmap and the drawing ops -----------------------------
    mov  rcx, [g_dc]
    mov  edx, 64
    mov  r8d, 48
    call CreateCompatibleBitmap
    test rax, rax
    jz   .f_draw
    mov  [g_hbmp], rax
    mov  rcx, [g_memdc]
    mov  rdx, [g_hbmp]
    call SelectObject
    test rax, rax
    jz   .f_draw
    mov  [g_hbmpOld], rax

    ; start from white
    mov  rcx, [g_memdc]
    xor  edx, edx
    xor  r8d, r8d
    mov  r9d, 64
    mov  qword [rsp+0x20], 48          ; PatBlt's 5th/6th args are on the stack
    mov  qword [rsp+0x28], WHITENESS
    call PatBlt
    test eax, eax
    jz   .f_draw

    ; SetPixel / GetPixel round-trip
    mov  rcx, [g_memdc]
    mov  edx, 8
    mov  r8d, 8
    mov  r9d, RGB_RED
    call SetPixel
    mov  rcx, [g_memdc]
    mov  edx, 8
    mov  r8d, 8
    call GetPixel
    cmp  eax, RGB_RED
    jne  .f_draw

    ; a line: MoveToEx + LineTo (the stock pen is selected, black)
    mov  rcx, [g_memdc]
    mov  edx, 2
    mov  r8d, 2
    xor  r9d, r9d                      ; no old point
    call MoveToEx
    test eax, eax
    jz   .f_draw
    mov  rcx, [g_memdc]
    mov  edx, 30
    mov  r8d, 2
    call LineTo
    test eax, eax
    jz   .f_draw

    ; shapes
    mov  rcx, [g_memdc]
    mov  edx, 5
    mov  r8d, 5
    mov  r9d, 25
    mov  qword [rsp+0x20], 20
    call Rectangle
    test eax, eax
    jz   .f_draw
    mov  rcx, [g_memdc]
    mov  edx, 30
    mov  r8d, 5
    mov  r9d, 50
    mov  qword [rsp+0x20], 25
    call Ellipse
    test eax, eax
    jz   .f_draw
    mov  rcx, [g_memdc]
    lea  rdx, [g_poly]
    mov  r8d, 3
    call Polygon
    test eax, eax
    jz   .f_draw

    ; ROP2: default is R2_COPYPEN and SetROP2 returns the previous
    mov  rcx, [g_memdc]
    call GetROP2
    cmp  eax, R2_COPYPEN
    jne  .f_draw

    ; SaveDC / RestoreDC with the text colour observable
    mov  rcx, [g_memdc]
    mov  edx, 0x00111111
    call SetTextColor                  ; returns the previous (default 0)
    mov  rcx, [g_memdc]
    call SaveDC
    cmp  eax, 1
    jl   .f_draw
    mov  rcx, [g_memdc]
    mov  edx, 0x00222222
    call SetTextColor
    mov  rcx, [g_memdc]
    mov  edx, 1
    call RestoreDC
    test eax, eax
    jz   .f_draw
    mov  rcx, [g_memdc]
    xor  edx, edx
    call SetTextColor
    cmp  eax, 0x00111111               ; restored
    jne  .f_draw
    OK s_draw_ok, s_draw_ok_l

    ; ---- section 4: text through the shipped PSF2 font ---------------------------
    ; SetTextColor/SetBkMode return the previous value
    mov  rcx, [g_memdc]
    mov  edx, RGB_RED
    call SetTextColor
    cmp  eax, 0                        ; we zeroed it above
    jne  .f_text
    mov  rcx, [g_memdc]
    mov  edx, OPAQUE
    call SetBkMode
    cmp  eax, OPAQUE                   ; default is OPAQUE
    jne  .f_text

    mov  rcx, [g_memdc]
    mov  edx, 2
    mov  r8d, 2
    lea  r9, [g_txt]
    mov  dword [rsp+0x20], 8           ; len
    call TextOutA
    test eax, eax
    jz   .f_text

    ; the shipped font is 8x16: tmHeight 16, tmAveCharWidth 8
    mov  rcx, [g_memdc]
    lea  rdx, [g_tm]
    call GetTextMetricsA
    test eax, eax
    jz   .f_text
    mov  eax, [g_tm+TEXTMETRICA.tmHeight]
    cmp  eax, 16
    jne  .f_text
    mov  eax, [g_tm+TEXTMETRICA.tmAveCharWidth]
    cmp  eax, 8
    jne  .f_text

    ; a 5-char extent is 40x16
    mov  rcx, [g_memdc]
    lea  rdx, [g_txt]
    mov  r8d, 5                        ; "W32A7"
    lea  r9, [g_extent]
    call GetTextExtentPoint32A
    test eax, eax
    jz   .f_text
    mov  eax, [g_extent+0]             ; cx
    cmp  eax, 40
    jne  .f_text
    mov  eax, [g_extent+4]             ; cy
    cmp  eax, 16
    jne  .f_text
    OK s_text_ok, s_text_ok_l

    ; ---- section 5: regions ------------------------------------------------------
    mov  ecx, 0
    mov  edx, 0
    mov  r8d, 30
    mov  r9d, 20
    call CreateRectRgn
    test rax, rax
    jz   .f_rgn
    mov  [g_rgn1], rax
    mov  ecx, 10
    mov  edx, 0
    mov  r8d, 40
    mov  r9d, 20
    call CreateRectRgn
    test rax, rax
    jz   .f_rgn
    mov  [g_rgn2], rax
    mov  ecx, 0
    mov  edx, 0
    mov  r8d, 64
    mov  r9d, 48
    call CreateRectRgn
    test rax, rax
    jz   .f_rgn
    mov  [g_rgndst], rax

    mov  rcx, [g_rgndst]
    mov  rdx, [g_rgn1]
    mov  r8, [g_rgn2]
    mov  r9d, RGN_AND
    call CombineRgn
    cmp  eax, SIMPLEREGION             ; 10..30 x 0..20
    jne  .f_rgn

    mov  rcx, [g_memdc]
    mov  rdx, [g_rgn1]
    call SelectClipRgn
    cmp  eax, SIMPLEREGION
    jne  .f_rgn

    ; GetClipRgn copies the selected region out again
    mov  ecx, 0
    mov  edx, 0
    mov  r8d, 1
    mov  r9d, 1
    call CreateRectRgn
    test rax, rax
    jz   .f_rgn
    mov  [g_rgnclip], rax
    mov  rcx, [g_memdc]
    mov  rdx, [g_rgnclip]
    call GetClipRgn
    cmp  eax, 1
    jne  .f_rgn

    ; a rect inside the clip is visible; after excluding everything, not
    mov  dword [g_rectvis+RECT.left],   0
    mov  dword [g_rectvis+RECT.top],    0
    mov  dword [g_rectvis+RECT.right],  5
    mov  dword [g_rectvis+RECT.bottom], 5
    mov  rcx, [g_memdc]
    lea  rdx, [g_rectvis]
    call RectVisible
    test eax, eax
    jz   .f_rgn
    mov  rcx, [g_memdc]
    xor  edx, edx
    xor  r8d, r8d
    mov  r9d, 64
    mov  qword [rsp+0x20], 48
    call ExcludeClipRect
    cmp  eax, NULLREGION                ; everything excluded
    jne  .f_rgn
    mov  rcx, [g_memdc]
    lea  rdx, [g_rectvis]
    call RectVisible
    test eax, eax
    jnz  .f_rgn
    mov  rcx, [g_memdc]
    xor  edx, edx
    call SelectClipRgn                  ; NULL resets the clip
    cmp  eax, SIMPLEREGION
    jne  .f_rgn
    mov  rcx, [g_memdc]
    lea  rdx, [g_rectvis]
    call RectVisible
    test eax, eax
    jz   .f_rgn

    mov  rcx, [g_rgn1]
    call DeleteObject
    test eax, eax
    jz   .f_rgn
    mov  rcx, [g_rgn2]
    call DeleteObject
    test eax, eax
    jz   .f_rgn
    mov  rcx, [g_rgndst]
    call DeleteObject
    test eax, eax
    jz   .f_rgn
    mov  rcx, [g_rgnclip]
    call DeleteObject
    test eax, eax
    jz   .f_rgn
    OK s_rgn_ok, s_rgn_ok_l

    ; ---- section 6: DIBs -----------------------------------------------------------
    ; an 8x4 32bpp top-down section
    mov  rcx, [g_memdc]
    lea  rdx, [g_bmi32]
    xor  r8d, r8d                       ; DIB_RGB_COLORS
    lea  r9, [g_dibBits]
    mov  qword [rsp+0x20], 0
    mov  qword [rsp+0x28], 0
    call CreateDIBSection
    test rax, rax
    jz   .f_dib
    mov  [g_hdib], rax
    cmp  qword [g_dibBits], 0
    je   .f_dib
    mov  rcx, [g_memdc]
    mov  rdx, [g_hdib]
    call SelectObject
    test rax, rax
    jz   .f_dib
    mov  [g_hdibOld], rax

    ; SetPixel then read the same word through GetPixel and through bits
    mov  rcx, [g_memdc]
    mov  edx, 3
    mov  r8d, 2
    mov  r9d, RGB_BLUE
    call SetPixel
    mov  rcx, [g_memdc]
    mov  edx, 3
    mov  r8d, 2
    call GetPixel
    cmp  eax, RGB_BLUE
    jne  .f_dib
    ; the engine writes BGRA with A=255: blue at (3,2), top-down row 2
    mov  rax, [g_dibBits]
    mov  eax, [rax + (2*8+3)*4]
    cmp  eax, 0xFF0000FF                ; B=FF G=00 R=00 A=FF
    jne  .f_dib

    ; GetDIBits with a positive-height header flips the rows: the word
    ; we wrote at top-down y=2 lands at bottom-up y = 4-1-2 = 1
    mov  rcx, [g_memdc]
    mov  rdx, [g_hdib]
    xor  r8d, r8d                       ; start
    mov  r9d, 4                         ; lines
    lea  rax, [g_dibbuf]
    mov  [rsp+0x20], rax                ; bits out
    lea  rax, [g_bmi32rd]
    mov  [rsp+0x28], rax                ; header (biHeight = +4)
    mov  qword [rsp+0x30], 0            ; DIB_RGB_COLORS
    call GetDIBits
    cmp  eax, 4
    jne  .f_dib
    mov  eax, [g_dibbuf + (1*8+3)*4]
    cmp  eax, 0xFF0000FF
    jne  .f_dib

    ; SetDIBits puts it back: the blue pixel survives
    mov  rcx, [g_memdc]
    mov  rdx, [g_hdib]
    xor  r8d, r8d
    mov  r9d, 4
    lea  rax, [g_dibbuf]
    mov  [rsp+0x20], rax
    lea  rax, [g_bmi32rd]
    mov  [rsp+0x28], rax
    mov  qword [rsp+0x30], 0
    call SetDIBits
    cmp  eax, 4
    jne  .f_dib
    mov  rcx, [g_memdc]
    mov  edx, 3
    mov  r8d, 2
    call GetPixel
    cmp  eax, RGB_BLUE
    jne  .f_dib
    OK s_dib_ok, s_dib_ok_l

    ; ---- section 7: palettes --------------------------------------------------------
    lea  rcx, [g_logpal]
    call CreatePalette
    test rax, rax
    jz   .f_pal
    mov  [g_hpal], rax

    ; an 8bpp section realises the palette into its colour table
    mov  rcx, [g_memdc]
    lea  rdx, [g_bmi8]
    xor  r8d, r8d
    lea  r9, [g_dibBits]                ; reuse the pointer slot
    mov  qword [rsp+0x20], 0
    mov  qword [rsp+0x28], 0
    call CreateDIBSection
    test rax, rax
    jz   .f_pal
    mov  rcx, [g_memdc]
    mov  rdx, rax
    call SelectObject
    test rax, rax
    jz   .f_pal
    ; top-left pixel = palette index 2 (blue)
    mov  rax, [g_dibBits]
    mov  byte [rax], 2

    mov  rcx, [g_memdc]
    mov  rdx, [g_hpal]
    xor  r8d, r8d
    call SelectPalette
    test rax, rax                       ; previous palette back
    jz   .f_pal
    mov  [g_hpalOld], rax
    mov  rcx, [g_memdc]
    call RealizePalette
    cmp  eax, 4                         ; 4 entries realised
    jne  .f_pal
    mov  rcx, [g_memdc]
    xor  edx, edx
    xor  r8d, r8d
    call GetPixel
    cmp  eax, RGB_BLUE                  ; index 2 reads blue
    jne  .f_pal

    ; replace entry 2 with yellow, re-realise, the pixel changes
    mov  rcx, [g_hpal]
    mov  edx, 2
    mov  r8d, 1
    lea  r9, [g_palent]
    call SetPaletteEntries
    cmp  eax, 1
    jne  .f_pal
    mov  rcx, [g_memdc]
    call RealizePalette
    mov  rcx, [g_memdc]
    xor  edx, edx
    xor  r8d, r8d
    call GetPixel
    cmp  eax, RGB_YELLOW
    jne  .f_pal
    mov  rcx, [g_memdc]
    call UpdateColors
    test eax, eax
    jz   .f_pal
    mov  rcx, [g_hpal]
    call UnrealizeObject
    test eax, eax
    jz   .f_pal
    OK s_pal_ok, s_pal_ok_l

    ; ---- section 8: the window-DC blit path ---------------------------------------
    ; re-select the 64x48 bitmap: a blue 4x4 patch, BitBlt onto the window
    mov  rcx, [g_memdc]
    mov  rdx, [g_hbmp]
    call SelectObject
    test rax, rax
    jz   .f_blit
    mov  dword [g_rect+RECT.left],   0
    mov  dword [g_rect+RECT.top],    0
    mov  dword [g_rect+RECT.right],  4
    mov  dword [g_rect+RECT.bottom], 4
    mov  rcx, [g_memdc]
    lea  rdx, [g_rect]
    mov  r8, [g_hbr]                    ; the red brush from section 2
    call FillRect
    test eax, eax
    jz   .f_blit

    ; BitBlt SRCCOPY: memory -> window at (10, 4)
    mov  rcx, [g_dc]
    mov  edx, 10
    mov  r8d, 4
    mov  r9d, 4
    mov  qword [rsp+0x20], 4            ; h   (w already in r9d)
    mov  rax, [g_memdc]
    mov  [rsp+0x28], rax                ; src DC
    mov  qword [rsp+0x30], 0            ; sx
    mov  qword [rsp+0x38], 0            ; sy
    mov  qword [rsp+0x40], SRCCOPY      ; rop
    call BitBlt
    test eax, eax
    jz   .f_blit
    mov  rcx, [g_dc]
    mov  edx, 11
    mov  r8d, 5
    call GetPixel
    cmp  eax, RGB_RED
    jne  .f_blit

    ; FillRect red straight onto the window DC, read back (the A-5 path)
    mov  dword [g_rect+RECT.left],   5
    mov  dword [g_rect+RECT.top],    5
    mov  dword [g_rect+RECT.right],  9
    mov  dword [g_rect+RECT.bottom], 9
    mov  rcx, [g_dc]
    lea  rdx, [g_rect]
    mov  r8, [g_hbr]
    call FillRect
    test eax, eax
    jz   .f_blit
    mov  rcx, [g_dc]
    mov  edx, 6
    mov  r8d, 6
    call GetPixel
    cmp  eax, RGB_RED
    jne  .f_blit
    OK s_blit_ok, s_blit_ok_l

    ; ---- cleanup ----------------------------------------------------------------------
    mov  rcx, [g_hbr]
    call DeleteObject
    mov  rcx, [g_hbmp]
    call DeleteObject
    mov  rcx, [g_hdib]
    call DeleteObject
    mov  rcx, [g_hpal]
    call DeleteObject
    mov  rcx, [g_memdc]
    call DeleteDC
    mov  rcx, [g_hwnd]
    call DestroyWindow
    test eax, eax
    jz   .f_cleanup

    OK s_done, s_done_l
    mov  ecx, 78
    call ExitProcess

; ---- failure paths --------------------------------------------------------------
.f_stdout:
    mov  ecx, 79
    call ExitProcess
.f_getmodule:
    FAIL f_getmodule, f_getmodule_l
.f_regclass:
    FAIL f_regclass, f_regclass_l
.f_createwin:
    FAIL f_createwin, f_createwin_l
.f_getdc:
    FAIL f_getdc, f_getdc_l
.f_caps:
    FAIL f_caps, f_caps_l
.f_memdc:
    FAIL f_memdc, f_memdc_l
.f_object:
    FAIL f_object, f_object_l
.f_draw:
    FAIL f_draw, f_draw_l
.f_text:
    FAIL f_text, f_text_l
.f_rgn:
    FAIL f_rgn, f_rgn_l
.f_dib:
    FAIL f_dib, f_dib_l
.f_pal:
    FAIL f_pal, f_pal_l
.f_blit:
    FAIL f_blit, f_blit_l
.f_cleanup:
    FAIL f_cleanup, f_cleanup_l

; ---- marker strings ---------------------------------------------------------------
section .rdata

s_dc_ok      db `A7-DC-OK\n`
s_dc_ok_l    equ $-s_dc_ok
s_object_ok  db `A7-OBJECT-OK\n`
s_object_ok_l equ $-s_object_ok
s_draw_ok    db `A7-DRAW-OK\n`
s_draw_ok_l  equ $-s_draw_ok
s_text_ok    db `A7-TEXT-OK\n`
s_text_ok_l  equ $-s_text_ok
s_rgn_ok     db `A7-RGN-OK\n`
s_rgn_ok_l   equ $-s_rgn_ok
s_dib_ok     db `A7-DIB-OK\n`
s_dib_ok_l   equ $-s_dib_ok
s_pal_ok     db `A7-PALETTE-OK\n`
s_pal_ok_l   equ $-s_pal_ok
s_blit_ok    db `A7-BLIT-OK\n`
s_blit_ok_l  equ $-s_blit_ok
s_done       db `W32A7-GDI-OK\n`
s_done_l     equ $-s_done

f_getmodule  db `A7-GETMODULE-FAIL\n`
f_getmodule_l equ $-f_getmodule
f_regclass   db `A7-REGISTER-FAIL\n`
f_regclass_l equ $-f_regclass
f_createwin  db `A7-CREATEWIN-FAIL\n`
f_createwin_l equ $-f_createwin
f_getdc      db `A7-GETDC-FAIL\n`
f_getdc_l    equ $-f_getdc
f_caps       db `A7-CAPS-FAIL\n`
f_caps_l     equ $-f_caps
f_memdc      db `A7-MEMDC-FAIL\n`
f_memdc_l    equ $-f_memdc
f_object     db `A7-OBJECT-FAIL\n`
f_object_l   equ $-f_object
f_draw       db `A7-DRAW-FAIL\n`
f_draw_l     equ $-f_draw
f_text       db `A7-TEXT-FAIL\n`
f_text_l     equ $-f_text
f_rgn        db `A7-RGN-FAIL\n`
f_rgn_l      equ $-f_rgn
f_dib        db `A7-DIB-FAIL\n`
f_dib_l      equ $-f_dib
f_pal        db `A7-PALETTE-FAIL\n`
f_pal_l      equ $-f_pal
f_blit       db `A7-BLIT-FAIL`, 10
f_blit_l     equ $-f_blit
f_cleanup    db `A7-CLEANUP-FAIL`, 10
f_cleanup_l  equ $-f_cleanup
