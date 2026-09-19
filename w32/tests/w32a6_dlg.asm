; w32a6_dlg.asm — W32APP_PLAN.md phase W32A-6 guest gate.
;
; One dialog-template-driven modal dialog (RT_DIALOG / 1) sourced from
; this image's own resource section (FindResourceW / LoadResource /
; LockResource / SizeofResource), one popup menu (CreatePopupMenu /
; AppendMenuW / CheckMenuItem / GetMenuItemCount / GetMenuItemID), one
; accelerator table (CreateAcceleratorTableW / DestroyAcceleratorTable),
; a local WH_CALLWNDPROC hook (SetWindowsHookExW / CallNextHookEx /
; UnhookWindowsHookEx), one timer (SetTimer / WM_TIMER / KillTimer) whose
; second tick closes the modal loop, caret (CreateCaret / SetCaretPos /
; ShowCaret / HideCaret / GetCaretPos / DestroyCaret), clipboard
; (OpenClipboard / EmptyClipboard / GetClipboardOwner /
; GetOpenClipboardWindow / IsClipboardFormatAvailable / SetClipboardData /
; GetClipboardData round-trip / RegisterClipboardFormatW / CloseClipboard),
; a string-table entry (LoadStringW from RT_STRING / 1, id 1), and
; DrawTextW / DrawFocusRect on the host window's WM_PAINT.  Every section
; prints A6-<NAME>-OK after all its checks, or A6-<NAME>-FAIL and exits
; 79.  The final W32A6-DLG-OK plus exit 78 is what
; tests/integration/cases/test_w32a6_user32dlg.sh asserts; exit 1 means
; the personality refused to load the image at all.
;
; The embedded dialog template is assembled as raw data in a hand-emitted
; .rsrc PE resource directory.  Two details are worth calling out because
; they are contracts, not style:
;
;   * The template header is the project's W32_DLGTEMPLATE (user32.h):
;     style, exStyle, item count -- three DWORDs, the count included --
;     then x/y/cx/cy as WORDs, then the menu/class/title var-length
;     fields, and (because DS_SETFONT is set) ptsize/weight/italic+charset
;     WORDs before the typeface name.  This mirrors what w32_dlg.c's
;     dlg_parse_header walks and what tests/unit/test_w32_a6.c feeds it
;     on the host side.
;   * The "edit" item the DlgItem accessors exercise cannot be a real
;     WS_CHILD window: CreateWindowExW refuses WS_CHILD (the compositor
;     has no child embedding, user32_win.c says so by name).  The item is
;     created as a small top-level window whose *parent* link and hMenu
;     control id (GWLP_ID) are what GetDlgItem walks -- the documented
;     A-6 idiom, same as the host unit test.
;
; Resource tree shape (walked by w32_pe.c rsrc_level):
;   root -> {RT_DIALOG=5, RT_STRING=6} (integer ids, no 0x80000000 bit)
;   type dir -> name id 1 -> lang entry -> IMAGE_RESOURCE_DATA_ENTRY
;   Leaf offsets carry NO directory bit; directory pointers DO carry it.
;   DataEntry.OffsetToData is a true RVA (label wrt ..imagebase).

bits 64
default rel

; ---- kernel32 imports ------------------------------------------------------
extern GetStdHandle
extern WriteFile
extern ExitProcess
extern GetModuleHandleW

; ---- user32: window core (A-5) ----------------------------------------------
extern RegisterClassExW
extern CreateWindowExW
extern DestroyWindow
extern DefWindowProcW
extern PostQuitMessage
extern BeginPaint
extern EndPaint

; ---- user32: dialogs (A-6) ---------------------------------------------------
extern DialogBoxIndirectParamW
extern DialogBoxParamW
extern EndDialog
extern GetDlgItem
extern GetDlgItemTextW
extern GetDlgItemInt
extern SetDlgItemTextW
extern SetDlgItemInt
extern CheckDlgButton
extern CheckRadioButton
extern IsDlgButtonChecked
extern IsDialogMessageW
extern MapDialogRect
extern GetDialogBaseUnits

; ---- user32: resources (A-6) -------------------------------------------------
extern FindResourceW
extern FindResourceExW
extern LoadResource
extern LockResource
extern SizeofResource
extern FreeResource
extern LoadStringW
extern LoadIconW
extern LoadCursorW
extern LoadImageW
extern DestroyIcon
extern DestroyCursor
extern EnumResourceNamesW

; ---- user32: menus (A-6) -----------------------------------------------------
extern CreateMenu
extern CreatePopupMenu
extern DestroyMenu
extern AppendMenuW
extern InsertMenuW
extern CheckMenuItem
extern GetMenu
extern SetMenu
extern GetSubMenu
extern GetSystemMenu
extern RemoveMenu
extern DeleteMenu
extern TrackPopupMenu
extern GetMenuItemCount
extern GetMenuItemID
extern EnableMenuItem
extern DrawMenuBar
extern LoadMenuW

; ---- user32: timers / caret / accelerators (A-6) -----------------------------
extern SetTimer
extern KillTimer
extern CreateCaret
extern DestroyCaret
extern SetCaretPos
extern GetCaretPos
extern ShowCaret
extern HideCaret
extern CreateAcceleratorTableW
extern DestroyAcceleratorTable
extern CopyAcceleratorTableW
extern LoadAcceleratorsW
extern TranslateAcceleratorW

; ---- user32: clipboard (A-6) -------------------------------------------------
extern OpenClipboard
extern CloseClipboard
extern EmptyClipboard
extern SetClipboardData
extern GetClipboardData
extern IsClipboardFormatAvailable
extern SetClipboardViewer
extern ChangeClipboardChain
extern GetClipboardOwner
extern GetOpenClipboardWindow
extern GetClipboardViewer
extern CountClipboardFormats
extern EnumClipboardFormats
extern RegisterClipboardFormatW

; ---- user32: hooks (A-6) -----------------------------------------------------
extern SetWindowsHookExW
extern UnhookWindowsHookEx
extern CallNextHookEx
extern NotifyWinEvent

; ---- user32: draw helpers (A-6) ----------------------------------------------
extern DrawTextW
extern DrawTextA
extern DrawFocusRect
extern DrawEdge
extern DrawFrameControl
extern DrawIcon
extern DrawIconEx

; ---- constants ---------------------------------------------------------------
%define STD_OUTPUT_HANDLE -11

%define WS_POPUP        0x80000000
%define WS_VISIBLE      0x10000000
%define WS_CAPTION      0x00C00000
%define WS_SYSMENU      0x00080000
%define WS_CHILD        0x40000000
%define WS_BORDER       0x00800000
%define WS_CLIPCHILDREN 0x02000000
%define WS_TABSTOP      0x00010000

%define DS_MODALFRAME   0x00000080
%define DS_CENTER       0x00000800
%define DS_SETFONT      0x00000040

%define WM_SETFOCUS     0x0007
%define WM_CLOSE        0x0010
%define WM_PAINT        0x000F
%define WM_DESTROY      0x0002
%define WM_INITDIALOG   0x0110
%define WM_COMMAND      0x0111
%define WM_TIMER        0x0113

%define IDOK            1
%define IDCANCEL        2
%define ID_EDIT         101
%define ID_CHECK        102
%define ID_RADIO1       103
%define ID_RADIO2       104

%define WH_CALLWNDPROC  4

%define CF_TEXT         1
%define CF_UNICODETEXT  13

%define IDT_TIMER1      42
%define IDM_POPUP1      1001

%define RT_CURSOR       1
%define RT_ICON         3
%define RT_DIALOG       5
%define RT_STRING       6

%define MF_STRING       0x0000
%define MF_POPUP        0x0010
%define MF_CHECKED      0x0008
%define MF_GRAYED       0x0001

%define BST_CHECKED     1

%define FVIRTKEY        0x01
%define FALT            0x10

; ---- structures (must mirror w32/include/w32/user32.h byte for byte) --------
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

struc PAINTSTRUCT
    .hdc         resq 1
    .fErase      resd 1
    .rcPaint     resb RECT_size
    .fRestore    resd 1
    .fIncUpdate  resd 1
    .rgbReserved resb 32
endstruc

; ---- data --------------------------------------------------------------------
section .data align=8

g_hInst         dq 0
g_stdout        dq 0
g_hwndHost      dq 0
g_hwndDlg       dq 0
g_hwndEdit      dq 0
g_hMenu         dq 0
g_hHook         dq 0
g_haccel        dq 0
g_gotInitDialog dd 0
g_timerTicks    dd 0
g_hookFired     dd 0
g_itemOk        dd 0

g_classHost     dw __utf16__('A6Host'),0
g_classDlg      dw __utf16__('#32770'),0    ; the class the dialog engine creates
g_classItem     dw __utf16__('A6Item'),0    ; logical-child control class
g_winTitle      dw __utf16__('W32A6 Host'),0
g_popupTxt      dw __utf16__('A6-Popup'),0
g_clipFmtName   dw __utf16__('A6ClipFmt'),0
g_editTxt       dw __utf16__('A6edit'),0
g_clipTxtA      db 'A6clip',0               ; CF_TEXT round-trip payload

; One ACCEL: W32_ACCEL { u8 flags; u16 key; u16 cmd; } -- laid out as
; word pairs so the 6-byte record reads fVirt|pad, key, cmd.
g_accelEnt      dw FVIRTKEY|FALT, 'A', IDM_POPUP1, 0

section .bss align=8
g_wcx           resb WNDCLASSEXW_size
g_ps            resb PAINTSTRUCT_size
g_rect          resb RECT_size
g_pt            resb POINT_size
g_mapRect       resb RECT_size
g_itemText      resw 32
g_strLoaded     resw 64
g_written       resq 1

; ---- console helpers ---------------------------------------------------------
section .text

; p_ok: rsi = marker bytes, edx = length.  Same shape as a5's puts_raw.
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

; p_fail: print the marker, then exit 79.
p_fail:
    call p_ok
    mov  ecx, 79
    call ExitProcess

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

; reg_class: rdx = UTF-16 class name, r8 = wndproc (used verbatim).
; Fills g_wcx and registers it; returns the atom in ax.
reg_class:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x40
    lea  rdi, [g_wcx]
    xor  eax, eax
    mov  ecx, WNDCLASSEXW_size/8
.zl:
    mov  [rdi], rax
    add  rdi, 8
    dec  ecx
    jnz  .zl
    mov  dword [g_wcx+WNDCLASSEXW.cbSize], WNDCLASSEXW_size
    mov  [g_wcx+WNDCLASSEXW.lpfnWndProc], r8
    mov  rax, [g_hInst]
    mov  [g_wcx+WNDCLASSEXW.hInstance], rax
    mov  qword [g_wcx+WNDCLASSEXW.hbrBackground], 0x00FFFFFF
    mov  [g_wcx+WNDCLASSEXW.lpszClassName], rdx
    lea  rcx, [g_wcx]
    call RegisterClassExW
    add  rsp, 0x40
    pop  rbp
    ret

; ---- main --------------------------------------------------------------------
global mainCRTStartup
mainCRTStartup:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0xB0               ; window-creation stack args + shadow room

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

    ; ---- Phase 1: resource walk --------------------------------------------
    ; FindResourceW(hInst, MAKEINTRESOURCE(1), RT_DIALOG)
    mov  rcx, [g_hInst]
    mov  edx, 1
    mov  r8d, RT_DIALOG
    call FindResourceW
    test rax, rax
    jz   .f_findrsrc
    mov  r12, rax                ; HRSRC (callee-saved; we are the root frame)
    mov  rcx, [g_hInst]
    mov  rdx, r12
    call SizeofResource
    test eax, eax
    jz   .f_sizeof
    cmp  eax, 64                 ; the template is a few hundred bytes
    jb   .f_sizeof
    mov  rcx, [g_hInst]
    mov  rdx, r12
    call LoadResource
    test rax, rax
    jz   .f_loadrsrc
    mov  rcx, rax
    call LockResource
    test rax, rax
    jz   .f_lockrsrc
    OK s_rsrc_ok, s_rsrc_ok_l

    ; ---- Phase 2: LoadStringW RT_STRING/1, id 1 -> "A6" --------------------
    mov  rcx, [g_hInst]
    mov  edx, 1
    lea  r8, [g_strLoaded]
    mov  r9d, 64
    call LoadStringW
    cmp  eax, 2
    jne  .f_loadstr
    cmp  word [g_strLoaded], 'A'
    jne  .f_loadstr
    cmp  word [g_strLoaded+2], '6'
    jne  .f_loadstr
    OK s_str_ok, s_str_ok_l

    ; ---- Phase 3: register the three classes -------------------------------
    ; host frame class
    lea  rdx, [g_classHost]
    lea  r8, [host_wndproc]
    call reg_class
    test ax, ax
    jz   .f_regclass
    ; the dialog engine creates its frame with class "#32770"
    lea  rdx, [g_classDlg]
    lea  r8, [DefWindowProcW]
    call reg_class
    test ax, ax
    jz   .f_regclass
    ; the class the DlgItem "edit" item is created from
    lea  rdx, [g_classItem]
    lea  r8, [DefWindowProcW]
    call reg_class
    test ax, ax
    jz   .f_regclass
    OK s_reg_ok, s_reg_ok_l

    ; ---- Phase 4: popup menu -------------------------------------------------
    call CreatePopupMenu
    test rax, rax
    jz   .f_cpmenu
    mov  [g_hMenu], rax
    mov  rcx, rax
    mov  edx, MF_STRING
    mov  r8d, IDM_POPUP1
    lea  r9, [g_popupTxt]
    call AppendMenuW
    test eax, eax
    jz   .f_appmenu
    mov  rcx, [g_hMenu]
    call GetMenuItemCount
    cmp  eax, 1
    jne  .f_countmenu
    mov  rcx, [g_hMenu]
    xor  edx, edx
    call GetMenuItemID
    cmp  eax, IDM_POPUP1
    jne  .f_menuid
    ; CheckMenuItem reports the previous state: unchecked (0) here
    mov  rcx, [g_hMenu]
    mov  edx, IDM_POPUP1
    mov  r8d, MF_CHECKED
    call CheckMenuItem
    test eax, eax
    jnz  .f_menucheck
    OK s_menu_ok, s_menu_ok_l

    ; ---- Phase 5: accelerator table -------------------------------------------
    lea  rcx, [g_accelEnt]
    mov  edx, 1
    call CreateAcceleratorTableW
    test rax, rax
    jz   .f_accel
    mov  [g_haccel], rax
    OK s_accel_ok, s_accel_ok_l

    ; ---- Phase 6: thread-local WH_CALLWNDPROC hook ----------------------------
    ; SetWindowsHookExW(idHook, proc, hMod, tid): mod=0 keeps it thread-local,
    ; which is the only form the A-6 surface accepts (globals refuse).
    mov  ecx, WH_CALLWNDPROC
    lea  rdx, [hook_proc]
    xor  r8d, r8d
    xor  r9d, r9d
    call SetWindowsHookExW
    test rax, rax
    jz   .f_hook
    mov  [g_hHook], rax
    OK s_hook_ok, s_hook_ok_l

    ; ---- Phase 7: host window --------------------------------------------------
    xor  ecx, ecx                         ; exStyle
    lea  rdx, [g_classHost]
    lea  r8, [g_winTitle]
    mov  r9d, WS_CAPTION | WS_SYSMENU | WS_VISIBLE
    mov  dword [rsp+0x20], 40             ; x
    mov  dword [rsp+0x28], 40             ; y
    mov  dword [rsp+0x30], 260            ; w
    mov  dword [rsp+0x38], 160            ; h
    mov  qword [rsp+0x40], 0              ; parent
    mov  qword [rsp+0x48], 0              ; menu
    mov  rax, [g_hInst]
    mov  [rsp+0x50], rax                  ; instance
    mov  qword [rsp+0x58], 0              ; param
    call CreateWindowExW
    test rax, rax
    jz   .f_chost
    mov  [g_hwndHost], rax
    OK s_host_ok, s_host_ok_l

    ; ---- Phase 8: timer ----------------------------------------------------------
    ; SetTimer returns the timer id on success (A-6 contract).
    mov  rcx, [g_hwndHost]
    mov  edx, IDT_TIMER1
    mov  r8d, 60                          ; 60 ms
    xor  r9d, r9d
    call SetTimer
    cmp  eax, IDT_TIMER1
    jne  .f_settimer
    OK s_timer_ok, s_timer_ok_l

    ; ---- Phase 9: caret ------------------------------------------------------------
    mov  rcx, [g_hwndHost]
    xor  edx, edx                         ; no bitmap
    mov  r8d, 2                           ; width
    mov  r9d, 10                          ; height
    call CreateCaret
    test eax, eax
    jz   .f_caret
    mov  ecx, 10
    mov  edx, 10
    call SetCaretPos
    test eax, eax
    jz   .f_caret
    mov  rcx, [g_hwndHost]
    call ShowCaret
    test eax, eax
    jz   .f_caret
    lea  rcx, [g_pt]
    call GetCaretPos
    test eax, eax
    jz   .f_caret
    cmp  dword [g_pt+POINT.x], 10
    jne  .f_caret
    cmp  dword [g_pt+POINT.y], 10
    jne  .f_caret
    OK s_caret_ok, s_caret_ok_l

    ; ---- Phase 10: clipboard --------------------------------------------------------
    mov  rcx, [g_hwndHost]
    call OpenClipboard
    test eax, eax
    jz   .f_cbopen
    call EmptyClipboard
    test eax, eax
    jz   .f_cbempty
    call GetClipboardOwner
    cmp  rax, [g_hwndHost]
    jne  .f_cbowner
    call GetOpenClipboardWindow
    cmp  rax, [g_hwndHost]
    jne  .f_cbowner
    ; CF_TEXT is always "available" in the A-6 contract: the format is
    ; bridged to the compositor's clipboard, not gated on prior data.
    mov  ecx, CF_TEXT
    call IsClipboardFormatAvailable
    test eax, eax
    jz   .f_cbavail
    ; CF_TEXT round-trip: SetClipboardData stores, GetClipboardData returns
    ; the compositor's copy (no GlobalAlloc needed for the A-6 path).
    mov  ecx, CF_TEXT
    lea  rdx, [g_clipTxtA]
    call SetClipboardData
    test rax, rax
    jz   .f_cbset
    mov  ecx, CF_TEXT
    call GetClipboardData
    test rax, rax
    jz   .f_cbget
    mov  r15, rax
    lea  rsi, [g_clipTxtA]
    xor  ecx, ecx
.cbcmp:
    mov  al, [rsi+rcx]
    cmp  al, [r15+rcx]
    jne  .f_cbget
    test al, al
    jz   .cbcmp_done
    inc  ecx
    cmp  ecx, 16
    jb   .cbcmp
    jmp  .f_cbget                         ; not NUL-terminated within 16
.cbcmp_done:
    ; custom formats register at 0xC000
    lea  rcx, [g_clipFmtName]
    call RegisterClipboardFormatW
    cmp  eax, 0xC000
    jb   .f_cbreg
    call CloseClipboard
    test eax, eax
    jz   .f_cbclose
    ; after close, CF_TEXT remains available (state lives in the compositor)
    mov  ecx, CF_TEXT
    call IsClipboardFormatAvailable
    test eax, eax
    jz   .f_cbavail2
    OK s_clip_ok, s_clip_ok_l

    ; ---- Phase 11: the modal dialog -----------------------------------------------
    ; Re-find the template (the phase-1 handle is a cache cookie, but the
    ; canonical DialogBox flow is Find -> Load -> Lock -> DialogBoxIndirect).
    mov  rcx, [g_hInst]
    mov  edx, 1
    mov  r8d, RT_DIALOG
    call FindResourceW
    test rax, rax
    jz   .f_dlgfind
    mov  rcx, [g_hInst]
    mov  rdx, rax
    call LoadResource
    test rax, rax
    jz   .f_dlgload
    mov  rcx, rax
    call LockResource
    test rax, rax
    jz   .f_dlglock
    mov  r14, rax                         ; template
    mov  rcx, [g_hInst]
    mov  rdx, r14
    mov  r8, [g_hwndHost]
    lea  r9, [dlg_proc]
    mov  qword [rsp+0x20], 0              ; dwInitParam
    call DialogBoxIndirectParamW
    cmp  eax, 77                          ; EndDialog result from the timer path
    jne  .f_dlgret
    cmp  dword [g_gotInitDialog], 1
    jne  .f_initdlg
    cmp  qword [g_hwndEdit], 0
    je   .f_edit
    ; the timer must have fired inside the modal pump (WM_TIMER -> EndDialog)
    cmp  dword [g_timerTicks], 1
    jb   .f_timerfire
    OK s_dlg_ok, s_dlg_ok_l

    ; ---- Phase 12: kill timer, destroy host window ----------------------------------
    mov  rcx, [g_hwndHost]
    mov  edx, IDT_TIMER1
    call KillTimer
    test eax, eax
    jz   .f_killtimer
    mov  rcx, [g_hwndHost]
    call DestroyWindow
    test eax, eax
    jz   .f_destroywin
    OK s_kill_ok, s_kill_ok_l

    ; ---- Phase 13: unhook / destroy accelerators, menu, caret ------------------------
    mov  rcx, [g_hHook]
    call UnhookWindowsHookEx
    test eax, eax
    jz   .f_unhook
    mov  rcx, [g_haccel]
    call DestroyAcceleratorTable
    test eax, eax
    jz   .f_destacc
    mov  rcx, [g_hMenu]
    call DestroyMenu
    test eax, eax
    jz   .f_destmenu
    mov  rcx, [g_hwndHost]
    call HideCaret
    test eax, eax
    jz   .f_destcaret
    call DestroyCaret
    test eax, eax
    jz   .f_destcaret
    OK s_cleanup_ok, s_cleanup_ok_l

    ; ---- done ---------------------------------------------------------------------
    OK s_done, s_done_l
    mov  ecx, 78
    call ExitProcess

; ---- failure paths ----------------------------------------------------------
.f_stdout:
    mov  ecx, 79
    call ExitProcess
.f_getmodule:
    FAIL f_getmodule, f_getmodule_l
.f_findrsrc:
    FAIL f_findrsrc, f_findrsrc_l
.f_sizeof:
    FAIL f_sizeof, f_sizeof_l
.f_loadrsrc:
    FAIL f_loadrsrc, f_loadrsrc_l
.f_lockrsrc:
    FAIL f_lockrsrc, f_lockrsrc_l
.f_loadstr:
    FAIL f_loadstr, f_loadstr_l
.f_regclass:
    FAIL f_regclass, f_regclass_l
.f_cpmenu:
    FAIL f_cpmenu, f_cpmenu_l
.f_appmenu:
    FAIL f_appmenu, f_appmenu_l
.f_countmenu:
    FAIL f_countmenu, f_countmenu_l
.f_menuid:
    FAIL f_menuid, f_menuid_l
.f_menucheck:
    FAIL f_menucheck, f_menucheck_l
.f_accel:
    FAIL f_accel, f_accel_l
.f_hook:
    FAIL f_hook, f_hook_l
.f_chost:
    FAIL f_chost, f_chost_l
.f_settimer:
    FAIL f_settimer, f_settimer_l
.f_caret:
    FAIL f_caret, f_caret_l
.f_cbopen:
    FAIL f_cbopen, f_cbopen_l
.f_cbempty:
    FAIL f_cbempty, f_cbempty_l
.f_cbowner:
    FAIL f_cbowner, f_cbowner_l
.f_cbavail:
    FAIL f_cbavail, f_cbavail_l
.f_cbset:
    FAIL f_cbset, f_cbset_l
.f_cbget:
    FAIL f_cbget, f_cbget_l
.f_cbreg:
    FAIL f_cbreg, f_cbreg_l
.f_cbclose:
    FAIL f_cbclose, f_cbclose_l
.f_cbavail2:
    FAIL f_cbavail2, f_cbavail2_l
.f_dlgfind:
    FAIL f_dlgfind, f_dlgfind_l
.f_dlgload:
    FAIL f_dlgload, f_dlgload_l
.f_dlglock:
    FAIL f_dlglock, f_dlglock_l
.f_dlgret:
    FAIL f_dlgret, f_dlgret_l
.f_initdlg:
    FAIL f_initdlg, f_initdlg_l
.f_edit:
    FAIL f_edit, f_edit_l
.f_timerfire:
    FAIL f_timerfire, f_timerfire_l
.f_killtimer:
    FAIL f_killtimer, f_killtimer_l
.f_destroywin:
    FAIL f_destroywin, f_destroywin_l
.f_unhook:
    FAIL f_unhook, f_unhook_l
.f_destacc:
    FAIL f_destacc, f_destacc_l
.f_destmenu:
    FAIL f_destmenu, f_destmenu_l
.f_destcaret:
    FAIL f_destcaret, f_destcaret_l

; ---- host window procedure --------------------------------------------------
; rcx = hwnd, edx = msg, r8 = wParam, r9 = lParam.  r12-r15 and rbx belong
; to mainCRTStartup; this proc must not touch them.
host_wndproc:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x40
    mov  [rbp-0x10], rcx                  ; hwnd (both paint and default need it)
    cmp  edx, WM_TIMER
    je   .wm_timer
    cmp  edx, WM_PAINT
    je   .wm_paint
    cmp  edx, WM_SETFOCUS
    je   .wm_setfocus
    cmp  edx, WM_DESTROY
    je   .wm_destroy
    call DefWindowProcW                   ; rcx/rdx/r8/r9 still the message
    leave
    ret

.wm_setfocus:
    call ShowCaret                        ; rcx = hwnd still intact
    xor  eax, eax
    leave
    ret

.wm_timer:
    inc  dword [g_timerTicks]
    cmp  dword [g_timerTicks], 2
    jb   .t_done
    ; Second tick: close the modal dialog with result 77.  EndDialog is
    ; what the dialog pump's GetMessage loop is waiting for.
    mov  rcx, [g_hwndDlg]
    mov  edx, 77
    call EndDialog
.t_done:
    xor  eax, eax
    leave
    ret

.wm_paint:
    mov  rcx, [rbp-0x10]
    lea  rdx, [g_ps]
    call BeginPaint
    mov  [rbp-0x08], rax                  ; hdc
    ; DrawTextW(hdc, g_strLoaded, -1, &rect, 0): the RECT is a stack arg
    ; area; the 5th argument (format) sits at [rsp+0x20].
    mov  dword [rsp+RECT.left], 5
    mov  dword [rsp+RECT.top], 5
    mov  dword [rsp+RECT.right], 180
    mov  dword [rsp+RECT.bottom], 20
    mov  qword [rsp+0x20], 0              ; uFormat = 0
    mov  r9, rsp                          ; pRect
    lea  rdx, [g_strLoaded]               ; lpString
    mov  r8d, -1                          ; cch = -1 (NUL-terminated)
    mov  rcx, [rbp-0x08]                  ; hdc
    call DrawTextW
    ; DrawFocusRect(hdc, &g_rect)
    mov  dword [g_rect+RECT.left], 4
    mov  dword [g_rect+RECT.top], 4
    mov  dword [g_rect+RECT.right], 100
    mov  dword [g_rect+RECT.bottom], 22
    lea  rdx, [g_rect]
    mov  rcx, [rbp-0x08]
    call DrawFocusRect
    mov  rcx, [rbp-0x10]
    lea  rdx, [g_ps]
    call EndPaint
    xor  eax, eax
    leave
    ret

.wm_destroy:
    xor  ecx, ecx
    call PostQuitMessage
    xor  eax, eax
    leave
    ret

; ---- hook procedure ----------------------------------------------------------
; nCode in ecx, wParam in rdx, lParam in r8.  Tail-jump into
; CallNextHookEx(NULL, nCode, wParam, lParam) after the shuffle.
hook_proc:
    inc  dword [g_hookFired]
    mov  r9, r8                           ; lParam
    mov  r8, rdx                          ; wParam
    mov  edx, ecx                         ; nCode
    xor  ecx, ecx                         ; hhk = NULL (chain end)
    jmp  CallNextHookEx

; ---- dialog procedure ---------------------------------------------------------
; rcx = hDlg, edx = msg, r8 = wParam, r9 = lParam.
dlg_proc:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x60
    cmp  edx, WM_INITDIALOG
    je   .wm_initdialog
    cmp  edx, WM_COMMAND
    je   .wm_command
    cmp  edx, WM_CLOSE
    je   .wm_close
    xor  eax, eax
    leave
    ret

.wm_initdialog:
    mov  [g_hwndDlg], rcx
    mov  dword [g_gotInitDialog], 1
    ; The item window: NOT WS_CHILD (refused by name in user32_win.c) --
    ; a small top-level window parented on the dialog whose hMenu slot is
    ; the control id GetDlgItem matches (GWLP_ID).
    xor  ecx, ecx                         ; exStyle
    lea  rdx, [g_classItem]
    xor  r8d, r8d                         ; no title
    mov  r9d, WS_POPUP | WS_BORDER | WS_VISIBLE
    mov  dword [rsp+0x20], 12             ; x
    mov  dword [rsp+0x28], 12             ; y
    mov  dword [rsp+0x30], 120            ; w
    mov  dword [rsp+0x38], 24             ; h
    mov  rax, [g_hwndDlg]
    mov  [rsp+0x40], rax                  ; parent (the dialog)
    mov  eax, ID_EDIT
    mov  [rsp+0x48], rax                  ; hMenu = control id
    mov  rax, [g_hInst]
    mov  [rsp+0x50], rax                  ; instance
    mov  qword [rsp+0x58], 0              ; param
    call CreateWindowExW
    test rax, rax
    jz   .init_bad
    mov  [g_hwndEdit], rax
    ; SetDlgItemTextW(hDlg, ID_EDIT, L"A6edit")
    mov  rcx, [g_hwndDlg]
    mov  edx, ID_EDIT
    lea  r8, [g_editTxt]
    call SetDlgItemTextW
    test eax, eax
    jz   .init_bad
    ; SetDlgItemInt(hDlg, ID_EDIT, 0x1234, FALSE) -> text becomes "4660"
    mov  rcx, [g_hwndDlg]
    mov  edx, ID_EDIT
    mov  r8d, 0x1234
    xor  r9d, r9d
    call SetDlgItemInt
    test eax, eax
    jz   .init_bad
    ; GetDlgItemTextW reads "4660" back
    mov  rcx, [g_hwndDlg]
    mov  edx, ID_EDIT
    lea  r8, [g_itemText]
    mov  r9d, 32
    call GetDlgItemTextW
    cmp  eax, 4
    jne  .init_bad
    cmp  word [g_itemText], '4'
    jne  .init_bad
    cmp  word [g_itemText+2], '6'
    jne  .init_bad
    cmp  word [g_itemText+4], '6'
    jne  .init_bad
    cmp  word [g_itemText+6], '0'
    jne  .init_bad
    ; GetDlgItemInt round-trips to 0x1234 with *translated = TRUE
    mov  rcx, [g_hwndDlg]
    mov  edx, ID_EDIT
    lea  r8, [g_itemOk]
    xor  r9d, r9d
    call GetDlgItemInt
    cmp  eax, 0x1234
    jne  .init_bad
    cmp  dword [g_itemOk], 1
    jne  .init_bad
    ; CheckDlgButton / CheckRadioButton succeed
    mov  rcx, [g_hwndDlg]
    mov  edx, ID_CHECK
    mov  r8d, BST_CHECKED
    call CheckDlgButton
    test eax, eax
    jz   .init_bad
    mov  rcx, [g_hwndDlg]
    mov  edx, ID_RADIO1
    mov  r8d, ID_RADIO2
    mov  r9d, ID_RADIO1
    call CheckRadioButton
    test eax, eax
    jz   .init_bad
    ; IsDlgButtonChecked: the A-6 contract keeps no visual state, so the
    ; documented answer is 0 (unchecked) even right after CheckDlgButton.
    mov  rcx, [g_hwndDlg]
    mov  edx, ID_CHECK
    call IsDlgButtonChecked
    test eax, eax
    jnz  .init_bad
    ; MapDialogRect: 4x8 DLU -> 8x16 px at the A-6 base units (8x16 DLU)
    mov  dword [g_mapRect+RECT.left], 0
    mov  dword [g_mapRect+RECT.top], 0
    mov  dword [g_mapRect+RECT.right], 4
    mov  dword [g_mapRect+RECT.bottom], 8
    mov  rcx, [g_hwndDlg]
    lea  rdx, [g_mapRect]
    call MapDialogRect
    test eax, eax
    jz   .init_bad
    cmp  dword [g_mapRect+RECT.right], 8
    jne  .init_bad
    cmp  dword [g_mapRect+RECT.bottom], 16
    jne  .init_bad
    ; GetDialogBaseUnits == (16 << 16) | 8
    call GetDialogBaseUnits
    cmp  eax, 0x00100008
    jne  .init_bad
    mov  eax, 1
    leave
    ret

.init_bad:
    mov  rcx, [g_hwndDlg]
    mov  edx, 76                          ; failure result (not 77)
    call EndDialog
    mov  eax, 1
    leave
    ret

.wm_command:
    movzx eax, r8w                        ; LOWORD(wParam) = control id
    cmp  eax, IDOK
    je   .end77
    cmp  eax, IDCANCEL
    je   .end77
    xor  eax, eax
    leave
    ret

.end77:
.wm_close:
    mov  rcx, [g_hwndDlg]
    mov  edx, 77
    call EndDialog
    mov  eax, 1
    leave
    ret

; =============================================================================
; Resource section: RT_DIALOG/1 (the dialog template) and RT_STRING/1
; (one string block covering ids 0..15, with id 1 = "A6").
;
; IMAGE_RESOURCE_DIRECTORY: { Characteristics, TimeDateStamp (dd);
; Major/MinorVersion (dw,dw); NumberOfNamedEntries, NumberOfIdEntries
; (dw,dw) } then 8-byte entries { Name/Id (dd), OffsetToData (dd) }.
; Entry name high bit set => string name (we never emit those); entry
; offset high bit set => points at another directory; clear => leaf
; IMAGE_RESOURCE_DATA_ENTRY { OffsetToData RVA (dd), Size (dd),
; CodePage (dd), Reserved (dd) }.
; =============================================================================
section .rsrc rdata align=4

rsrc_start:

dir_root:
    dd 0, 0                               ; Characteristics, TimeDateStamp
    dw 0, 0                               ; Major/MinorVersion
    dw 0                                  ; NumberOfNamedEntries
    dw 2                                  ; NumberOfIdEntries (RT_DIALOG, RT_STRING)
    dd 5                                  ; RT_DIALOG (integer id)
    dd (dir_typ_dialog - rsrc_start) | 0x80000000
    dd 6                                  ; RT_STRING (integer id)
    dd (dir_typ_string - rsrc_start) | 0x80000000

dir_typ_dialog:
    dd 0, 0
    dw 0, 0
    dw 0                                  ; named
    dw 1                                  ; ids
    dd 1                                  ; resource name id 1
    dd (dir_nam_dlg1 - rsrc_start) | 0x80000000

dir_nam_dlg1:
    dd 0, 0
    dw 0, 0
    dw 0
    dw 1
    dd 0                                  ; language neutral (integer id)
    dd (data_dlg - rsrc_start)            ; LEAF: no directory bit

data_dlg:
    dd rva_dialog wrt ..imagebase         ; OffsetToData (RVA, link-time)
    dd dialog_end - rva_dialog            ; Size
    dd 0                                  ; CodePage
    dd 0                                  ; Reserved

dir_typ_string:
    dd 0, 0
    dw 0, 0
    dw 0
    dw 1
    dd 1
    dd (dir_nam_str1 - rsrc_start) | 0x80000000

dir_nam_str1:
    dd 0, 0
    dw 0, 0
    dw 0
    dw 1
    dd 0
    dd (data_str - rsrc_start)            ; LEAF

data_str:
    dd rva_string wrt ..imagebase
    dd string_end - rva_string
    dd 0
    dd 0

; ---- the dialog template (W32_DLGTEMPLATE layout) ---------------------------
align 4
rva_dialog:
dialog_tpl:
    dd WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT
    dd 0                                  ; exStyle
    dd 6                                  ; item count (W32_DLGTEMPLATE: DWORD)
    dw 0, 0, 160, 90                      ; x, y, cx, cy
    dw 0                                  ; menu: none
    dw 0                                  ; class: default (#32770)
    dw 'W','3','2','A','6',' ','D','l','g',0
    ; DS_SETFONT: ptsize, weight, italic+charset (3 WORDs), then typeface
    dw 8, 400, 0
    dw 'T','a','h','o','m','a',0
    ; ---- item templates (layout fidelity; the engine's frame window is
    ; created from the header above, the items document the dialog) ----
    ; EDIT
    dd WS_CHILD | WS_VISIBLE | WS_BORDER
    dd 0
    dw 10, 10, 80, 12
    dw ID_EDIT
    dw 0xFFFF, 0x0081                     ; ordinal class: Edit
    dw 0                                  ; no title
    dw 0                                  ; no creation data
    ; OK button
    dd WS_CHILD | WS_VISIBLE | WS_TABSTOP
    dd 0
    dw 100, 10, 40, 14
    dw IDOK
    dw 0xFFFF, 0x0080                     ; ordinal class: Button
    dw 'O','K',0
    dw 0
    ; Cancel button
    dd WS_CHILD | WS_VISIBLE | WS_TABSTOP
    dd 0
    dw 100, 30, 40, 14
    dw IDCANCEL
    dw 0xFFFF, 0x0080
    dw 'C','a','n','c','e','l',0
    dw 0
    ; Check box
    dd WS_CHILD | WS_VISIBLE
    dd 0
    dw 10, 30, 60, 10
    dw ID_CHECK
    dw 0xFFFF, 0x0080
    dw 'C','h','k',0
    dw 0
    ; Radio 1
    dd WS_CHILD | WS_VISIBLE
    dd 0
    dw 10, 44, 40, 10
    dw ID_RADIO1
    dw 0xFFFF, 0x0080
    dw 'R','1',0
    dw 0
    ; Radio 2
    dd WS_CHILD | WS_VISIBLE
    dd 0
    dw 55, 44, 40, 10
    dw ID_RADIO2
    dw 0xFFFF, 0x0080
    dw 'R','2',0
    dw 0
dialog_end:

; ---- string table block: 16 length-prefixed slots, id 1 = "A6" ---------------
align 4
rva_string:
string_block:
    dw 0                                  ; id 0: empty
    dw 2                                  ; id 1: length 2
    dw 'A','6'                            ; id 1 characters
    dw 0,0,0,0,0,0,0,0,0,0,0,0,0,0        ; ids 2..15: empty
string_end:

; ---- marker strings ------------------------------------------------------------
section .rdata

s_rsrc_ok    db `A6-RESOURCE-OK\n`
s_rsrc_ok_l  equ $-s_rsrc_ok
s_str_ok     db `A6-LOADSTRING-OK\n`
s_str_ok_l   equ $-s_str_ok
s_reg_ok     db `A6-REGISTER-OK\n`
s_reg_ok_l   equ $-s_reg_ok
s_menu_ok    db `A6-MENU-OK\n`
s_menu_ok_l  equ $-s_menu_ok
s_accel_ok   db `A6-ACCEL-OK\n`
s_accel_ok_l equ $-s_accel_ok
s_hook_ok    db `A6-HOOK-OK\n`
s_hook_ok_l  equ $-s_hook_ok
s_host_ok    db `A6-HOSTWND-OK\n`
s_host_ok_l  equ $-s_host_ok
s_timer_ok   db `A6-TIMER-OK\n`
s_timer_ok_l equ $-s_timer_ok
s_caret_ok   db `A6-CARET-OK\n`
s_caret_ok_l equ $-s_caret_ok
s_clip_ok    db `A6-CLIPBOARD-OK\n`
s_clip_ok_l  equ $-s_clip_ok
s_dlg_ok     db `A6-DIALOG-OK\n`
s_dlg_ok_l   equ $-s_dlg_ok
s_kill_ok    db `A6-CLEANUP-WIN-OK\n`
s_kill_ok_l  equ $-s_kill_ok
s_cleanup_ok db `A6-CLEANUP-ALL-OK\n`
s_cleanup_ok_l equ $-s_cleanup_ok
s_done       db `W32A6-DLG-OK\n`
s_done_l     equ $-s_done

f_getmodule  db `A6-GETMODULE-FAIL\n`
f_getmodule_l equ $-f_getmodule
f_findrsrc   db `A6-FINDRESOURCE-FAIL\n`
f_findrsrc_l equ $-f_findrsrc
f_sizeof     db `A6-SIZEOFRESOURCE-FAIL\n`
f_sizeof_l   equ $-f_sizeof
f_loadrsrc   db `A6-LOADRESOURCE-FAIL\n`
f_loadrsrc_l equ $-f_loadrsrc
f_lockrsrc   db `A6-LOCKRESOURCE-FAIL\n`
f_lockrsrc_l equ $-f_lockrsrc
f_loadstr    db `A6-LOADSTRING-FAIL\n`
f_loadstr_l  equ $-f_loadstr
f_regclass   db `A6-REGISTERCLASS-FAIL\n`
f_regclass_l equ $-f_regclass
f_cpmenu     db `A6-CREATEPOPUPMENU-FAIL\n`
f_cpmenu_l   equ $-f_cpmenu
f_appmenu    db `A6-APPENDMENU-FAIL\n`
f_appmenu_l  equ $-f_appmenu
f_countmenu  db `A6-MENUCOUNT-FAIL\n`
f_countmenu_l equ $-f_countmenu
f_menuid     db `A6-MENUID-FAIL\n`
f_menuid_l   equ $-f_menuid
f_menucheck  db `A6-MENUCHECK-FAIL\n`
f_menucheck_l equ $-f_menucheck
f_accel      db `A6-ACCEL-FAIL\n`
f_accel_l    equ $-f_accel
f_hook       db `A6-HOOK-FAIL\n`
f_hook_l     equ $-f_hook
f_chost      db `A6-CREATEWINDOW-FAIL\n`
f_chost_l    equ $-f_chost
f_settimer   db `A6-SETTIMER-FAIL\n`
f_settimer_l equ $-f_settimer
f_caret      db `A6-CARET-FAIL\n`
f_caret_l    equ $-f_caret
f_cbopen     db `A6-CLIPBOARD-OPEN-FAIL\n`
f_cbopen_l   equ $-f_cbopen
f_cbempty    db `A6-CLIPBOARD-EMPTY-FAIL\n`
f_cbempty_l  equ $-f_cbempty
f_cbowner    db `A6-CLIPBOARD-OWNER-FAIL\n`
f_cbowner_l  equ $-f_cbowner
f_cbavail    db `A6-CLIPBOARD-AVAIL-FAIL\n`
f_cbavail_l  equ $-f_cbavail
f_cbset      db `A6-CLIPBOARD-SET-FAIL\n`
f_cbset_l    equ $-f_cbset
f_cbget      db `A6-CLIPBOARD-GET-FAIL\n`
f_cbget_l    equ $-f_cbget
f_cbreg      db `A6-CLIPBOARD-REGFMT-FAIL\n`
f_cbreg_l    equ $-f_cbreg
f_cbclose    db `A6-CLIPBOARD-CLOSE-FAIL\n`
f_cbclose_l  equ $-f_cbclose
f_cbavail2   db `A6-CLIPBOARD-AVAIL2-FAIL\n`
f_cbavail2_l equ $-f_cbavail2
f_dlgfind    db `A6-DLG-FINDRESOURCE-FAIL\n`
f_dlgfind_l  equ $-f_dlgfind
f_dlgload    db `A6-DLG-LOADRESOURCE-FAIL\n`
f_dlgload_l  equ $-f_dlgload
f_dlglock    db `A6-DLG-LOCKRESOURCE-FAIL\n`
f_dlglock_l  equ $-f_dlglock
f_dlgret     db `A6-DLG-RET-FAIL\n`
f_dlgret_l   equ $-f_dlgret
f_initdlg    db `A6-DLG-INIT-FAIL\n`
f_initdlg_l  equ $-f_initdlg
f_edit       db `A6-DLG-EDIT-FAIL\n`
f_edit_l     equ $-f_edit
f_timerfire  db `A6-DLG-TIMERFIRE-FAIL\n`
f_timerfire_l equ $-f_timerfire
f_killtimer  db `A6-KILLTIMER-FAIL\n`
f_killtimer_l equ $-f_killtimer
f_destroywin db `A6-DESTROYWINDOW-FAIL\n`
f_destroywin_l equ $-f_destroywin
f_unhook     db `A6-UNHOOK-FAIL\n`
f_unhook_l   equ $-f_unhook
f_destacc    db `A6-DESTROYACCEL-FAIL\n`
f_destacc_l  equ $-f_destacc
f_destmenu   db `A6-DESTROYMENU-FAIL\n`
f_destmenu_l equ $-f_destmenu
f_destcaret  db `A6-DESTROYCARET-FAIL\n`
f_destcaret_l equ $-f_destcaret
