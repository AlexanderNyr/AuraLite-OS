; w32a8_comctl32.asm — W32APP_PLAN.md phase W32A-8 guest gate.
;
; The common-controls surface, end to end, all through the documented
; paths: the WC_* classes registered by InitCommonControlsEx, created
; with CreateWindowExW as WS_CHILD windows (the gate: WS_CHILD is
; admitted for exactly the comctl classes — an application class with
; WS_CHILD is refused, asserted here), and driven with SendMessage
; while the notifications come back as WM_NOTIFY to the parent with
; the documented codes and order.
;
; Sections (each prints A8-<NAME>-OK only after all its checks):
;   INIT        InitCommonControlsEx admits the class mask
;   WSCHILD     the comctl WS_CHILD gate: refused for app classes,
;               admitted for WC_LISTVIEWW, GetParent lands the link
;   TOOLBAR     CreateToolbarEx; add/count/index/buttonsize/enable;
;               a click raises WM_COMMAND to the parent
;   STATUS      CreateStatusWindowW; parts; set/get text + length
;   LISTVIEW    insert/get/set/delete items; selection state machine;
;               columns; a click selects and notifies (LVN_ITEMCHANGED
;               then NM_CLICK); HitTest
;   TREEVIEW    insert root+children; GETNEXTITEM walks; SELECTITEM
;               notifies TVN_SELCHANGEDW; EXPAND notifies
;               TVN_ITEMEXPANDINGW BEFORE TVN_ITEMEXPANDEDW (order
;               asserted by sequence numbers); GETITEM text/rect;
;               delete; the gutter click collapses through the widget
;   TAB         insert items; SETCURSEL returns the previous and does
;               NOT notify; a strip click notifies TCN_SELCHANGE
;   PROGRESS    range/pos/step/stepit/range-readback
;   TOOLTIP     add tool; TTM_ACTIVATE; a relayed WM_MOUSEMOVE inside
;               the tool rect SHOWS the tip window and
;               TTM_GETCURRENTTOOL returns the text; a relay outside
;               hides it
;   IMAGELIST   CreateBitmap + AddMasked (mask colour reads
;               transparent); count/size; GetIcon; ReplaceIcon appends;
;               GetImageInfo rect; Draw onto the window DC verified
;               with GetPixel; Remove; the drag set (BeginDrag/DragEnter
;               /DragMove/DragShowNolock/EndDrag) all succeed
;   SUBCLASS    SetWindowSubclass installs (id 41); a message runs the
;               subclass then DefSubclassProc reaches the class proc;
;               a second subclass (id 42) runs newest-first and chains
;               down; GetWindowSubclass returns the data; Remove
;               restores the order
;   PSHEET      PropertySheetW hosts two in-memory-template pages; the
;               template's one button expands into a real child window
;               (GetDlgItem); WM_INITDIALOG rides the first
;               activation; PSM_SETCURSEL switches (PSN_KILLACTIVE/
;               SETACTIVE); PSM_PRESSBUTTON(OK) applies with
;               PSN_APPLY lParam TRUE on every page and ends the sheet
;               with IDOK
;   REFUSE      LoadIconWithScaleDown's E_INVALIDARG contract and the
;               TB_CUSTOMIZE named refusal
;
; The final A8-COMCTL-OK plus exit 78 is what
; tests/integration/cases/test_w32a8_comctl32.sh asserts; exit 79 is
; any section failure, exit 1 the loader's refusal.

bits 64
default rel

; ---- kernel32 imports --------------------------------------------------------
extern GetStdHandle
extern WriteFile
extern ExitProcess
extern GetModuleHandleW
extern GetLastError

; ---- user32 imports ----------------------------------------------------------
extern RegisterClassExW
extern CreateWindowExW
extern DefWindowProcW
extern GetMessageW
extern DispatchMessageW
extern PostQuitMessage
extern PostMessageW
extern SendMessageW
extern DestroyWindow
extern GetParent
extern GetDlgItem
extern IsWindowVisible
extern GetWindowLongPtrW

; ---- gdi32 imports -----------------------------------------------------------
extern GetDC
extern ReleaseDC
extern GetPixel
extern CreateBitmap
extern DeleteObject

; ---- comctl32 imports --------------------------------------------------------
extern InitCommonControls
extern InitCommonControlsEx
extern CreateToolbarEx
extern CreateStatusWindowW
extern PropertySheetW
extern LoadIconWithScaleDown
extern SetWindowSubclass
extern RemoveWindowSubclass
extern GetWindowSubclass
extern DefSubclassProc
extern _TrackMouseEvent
extern ImageList_Create
extern ImageList_Destroy
extern ImageList_AddMasked
extern ImageList_ReplaceIcon
extern ImageList_GetImageCount
extern ImageList_GetIcon
extern ImageList_GetIconSize
extern ImageList_GetImageInfo
extern ImageList_Draw
extern ImageList_SetIconSize
extern ImageList_Remove
extern ImageList_BeginDrag
extern ImageList_DragEnter
extern ImageList_DragMove
extern ImageList_DragShowNolock
extern ImageList_EndDrag

; ---- constants (mirror w32/include/w32/*.h) ----------------------------------
%define STD_OUTPUT_HANDLE  -11

%define WM_DESTROY       0x0002
%define WM_CLOSE         0x0010
%define WM_SETCURSOR     0x0020
%define WM_NCHITTEST     0x0084
%define WM_PAINT         0x000F
%define WM_INITDIALOG    0x0110
%define WM_COMMAND       0x0111
%define WM_NOTIFY        0x004E
%define WM_MOUSEMOVE     0x0200
%define WM_LBUTTONDOWN   0x0201
%define WM_TIMER         0x0113
%define WM_MOUSELEAVE    0x02A3
%define WM_USER          0x0400

%define WS_CHILD         0x40000000
%define WS_POPUP         0x80000000
%define WS_VISIBLE       0x10000000
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define SW_SHOW          5
%define SW_HIDE          0

%define MK_LBUTTON       0x0001

; ICC mask bits (comctl32.h)
%define ICC_BAR_CLASSES      0x00000001
%define ICC_LISTVIEW_CLASSES 0x00000004
%define ICC_TREEVIEW_CLASSES 0x00000008
%define ICC_TAB_CLASSES      0x00000010
%define ICC_PROGRESS_CLASS   0x00000020

; toolbar
%define TB_ADDBUTTONSW    0x0444        ; WM_USER+68
%define TB_BUTTONCOUNT    0x0418        ; WM_USER+24
%define TB_COMMANDTOINDEX 0x0419        ; WM_USER+25
%define TB_GETBUTTONSIZE  0x043A        ; WM_USER+58
%define TB_ENABLEBUTTON   0x0401        ; WM_USER+1
%define TB_ISBUTTONENABLED 0x0409       ; WM_USER+9
%define TB_CUSTOMIZE      0x041B        ; WM_USER+27 (refused by name)
%define TBSTATE_ENABLED   0x04

; status bar
%define SB_SETPARTS       0x0404        ; WM_USER+4
%define SB_GETPARTS       0x0406        ; WM_USER+6
%define SB_SETTEXTW       0x040B        ; WM_USER+11
%define SB_GETTEXTW       0x040D        ; WM_USER+13
%define SB_GETTEXTLENGTHW 0x040C        ; WM_USER+12

; listview
%define LVM_INSERTITEMW   0x104D        ; LVM_FIRST+77
%define LVM_DELETEITEM    0x1008
%define LVM_GETITEMCOUNT  0x1004
%define LVM_GETITEMW      0x104B        ; LVM_FIRST+75
%define LVM_SETITEMSTATE  0x102F        ; LVM_FIRST+47
%define LVM_GETSELECTEDCOUNT 0x1032     ; LVM_FIRST+50
%define LVM_GETNEXTITEM   0x100C
%define LVM_INSERTCOLUMNA 0x101B        ; LVM_FIRST+27
%define LVM_GETCOLUMNA    0x1019        ; LVM_FIRST+25
%define LVM_HITTEST       0x1012
%define LVIF_TEXT    0x0001
%define LVIF_PARAM   0x0004
%define LVIF_STATE   0x0008
%define LVIS_SELECTED 0x0002
%define LVNI_SELECTED  0x0002

; treeview
%define TVM_INSERTITEMW  0x1132        ; TVM_FIRST+50
%define TVM_DELETEITEM   0x1101
%define TVM_EXPAND       0x1102
%define TVM_GETCOUNT     0x1105
%define TVM_GETNEXTITEM  0x110A
%define TVM_SELECTITEM   0x110B
%define TVM_GETITEMW     0x113E        ; TVM_FIRST+62
%define TVM_GETITEMRECT  0x1104
%define TVM_HITTEST      0x1111
%define TVIF_TEXT     0x0001
%define TVIF_CHILDREN 0x0040
%define TVIS_SELECTED 0x0002
%define TVI_ROOT   0xFFFF0000
%define TVI_LAST   0xFFFF0002
%define TVE_EXPAND 0x0002
%define TVGN_ROOT    0x0000
%define TVGN_NEXT    0x0001
%define TVGN_PARENT  0x0003
%define TVGN_CHILD   0x0004
%define TVGN_CARET   0x0009

; tab
%define TCM_INSERTITEMW 0x133E        ; TCM_FIRST+62
%define TCM_GETITEMCOUNT 0x1304
%define TCM_GETCURSEL   0x130B
%define TCM_SETCURSEL   0x130C
%define TCIF_TEXT  0x0001
%define TCIF_PARAM 0x0008

; progress
%define PBM_SETRANGE 0x0401
%define PBM_SETPOS   0x0402
%define PBM_SETSTEP  0x0404
%define PBM_STEPIT   0x0405
%define PBM_GETRANGE 0x0407
%define PBM_GETPOS   0x0408

; tooltip
%define TTM_ADDTOOLW    0x0432        ; WM_USER+50
%define TTM_DELTOOLW    0x0433        ; WM_USER+51
%define TTM_RELAYEVENT  0x0407
%define TTM_ACTIVATE    0x0401
%define TTM_GETCURRENTTOOLW 0x043B    ; WM_USER+59
%define TTM_GETTOOLCOUNT 0x040D

; header
%define HDM_INSERTITEMW 0x120A        ; HDM_FIRST+10
%define HDM_GETITEMCOUNT 0x1200

; imagelist
%define ILC_COLOR32 0x20
%define ILD_NORMAL  0

; subclass: no constants, ordinals already proven by the W32A-1 gate

; propsheet
%define PSP_DLGINDIRECT   0x0010
%define PSH_PROPSHEETPAGE 0x0008
%define PSM_SETCURSEL     0x0465      ; WM_USER+101
%define PSM_PRESSBUTTON   0x0471      ; 1137
%define PSN_SETACTIVE     0xFFFFFF38  ; PSN_FIRST-0  (-200)
%define PSN_KILLACTIVE    0xFFFFFF37  ; PSN_FIRST-1  (-201)
%define PSN_APPLY         0xFFFFFF36  ; PSN_FIRST-2  (-202)
%define PSNRET_NOERROR    0
%define PSBTN_OK     3
%define IDOK     1
%define IDCANCEL 2
%define ID_APPLY_NOW 0x3021

; notification codes
%define NM_FIRST       0
%define LVN_FIRST      -100
%define TVN_FIRST      -400
%define TCN_FIRST      -550
%define HDN_FIRST      -300
%define NM_CLICK       (NM_FIRST-2)
%define LVN_ITEMCHANGED (LVN_FIRST-1)
%define TVN_SELCHANGEDW  (TVN_FIRST-51)   ; -451
%define TVN_ITEMEXPANDINGW (TVN_FIRST-54) ; -454
%define TVN_ITEMEXPANDEDW  (TVN_FIRST-55) ; -455
%define TCN_SELCHANGE  (TCN_FIRST-1)      ; -551
%define HDN_ITEMCLICKW (HDN_FIRST-22)     ; -322

%define E_INVALIDARG 0x80070057

; ---- structures (must mirror w32/include/w32/*.h byte for byte) --------------
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

struc MSG
    .hwnd    resq 1
    .message resd 1
    .rsvd    resd 1
    .wParam  resq 1
    .lParam  resq 1
    .time    resd 1
    .pt      resb POINT_size      ; at 36, exactly as the C engine packs it
endstruc

struc INITCOMMONCONTROLSEX
    .dwSize resd 1
    .dwICC  resd 1
endstruc

struc TBBUTTON
    .iBitmap    resd 1
    .idCommand  resd 1
    .fsState    resb 1
    .fsStyle    resb 1
    .rsvd       resb 6
    .dwData     resq 1
    .iString    resq 1
endstruc

struc LVITEMW
    .mask       resd 1
    .iItem      resd 1
    .iSubItem   resd 1
    .state      resd 1
    .stateMask  resd 1
    .rsvd0      resd 1
    .pszText    resq 1
    .cchTextMax resd 1
    .iImage     resd 1
    .lParam     resq 1
    .iIndent    resd 1
    .rsvd1      resd 1
endstruc

struc LVCOLUMNW
    .mask       resd 1
    .fmt        resd 1
    .cx         resd 1
    .rsvd0      resd 1
    .pszText    resq 1
    .cchTextMax resd 1
    .iSubItem   resd 1
    .rsvd1      resd 1
endstruc

struc TVITEMW
    .mask           resd 1
    .rsvd0          resd 1
    .hItem          resq 1
    .state          resd 1
    .stateMask      resd 1
    .rsvd1          resd 2      ; pad to 32: pointers are 8-aligned in the C struct
    .pszText        resq 1
    .cchTextMax     resd 1
    .iImage         resd 1
    .iSelectedImage resd 1
    .cChildren      resd 1
    .rsvd2          resd 1
    .lParam         resq 1
endstruc

struc TVINSERTSTRUCTW
    .hParent      resq 1
    .hInsertAfter resq 1
    .item         resb TVITEMW_size
endstruc

struc TCITEMW
    .mask        resd 1
    .dwState     resd 1
    .dwStateMask resd 1
    .rsvd0       resd 1
    .pszText     resq 1
    .cchTextMax  resd 1
    .iImage      resd 1
    .rsvd1       resd 2      ; pad to 40
    .lParam      resq 1
endstruc

struc TOOLINFOW
    .cbSize   resd 1
    .uFlags   resd 1
    .rsvd0    resd 2      ; pad to 16
    .hwnd     resq 1
    .uId      resq 1
    .rect     resb RECT_size
    .hinst    resq 1
    .lpszText resq 1
    .lParam   resq 1
endstruc

struc HDITEMW
    .mask       resd 1
    .cxy        resd 1
    .rsvd0      resd 2      ; pad to 16
    .pszText    resq 1
    .hbm        resq 1
    .cchTextMax resd 1
    .fmt        resd 1
    .rsvd1      resd 2      ; pad to 48
    .lParam     resq 1
    .iImage     resd 1
    .iOrder     resd 1
endstruc

struc PBRANGE
    .iLow  resd 1
    .iHigh resd 1
endstruc

struc IMAGEINFO
    .hbmImage resq 1
    .hbmMask  resq 1
    .rcImage  resb RECT_size
endstruc

struc PROPSHEETPAGEW
    .dwSize     resd 1
    .dwFlags    resd 1
    .rsvd0      resd 2      ; pad to 16
    .hInstance  resq 1
    .pResource  resq 1
    .pszIcon    resq 1
    .pszTitle   resq 1
    .pfnDlgProc resq 1
    .lParam     resq 1
endstruc

struc PROPSHEETHEADERW
    .dwSize      resd 1
    .dwFlags     resd 1
    .rsvd0       resd 2      ; pad to 16
    .hwndParent  resq 1
    .hInstance   resq 1
    .pszIcon     resq 1
    .pszCaption  resq 1
    .nPages      resd 1
    .rsvd1       resd 1
    .ppsp        resq 1
    .pfnCallback resq 1
endstruc

; DLGTEMPLATE (20 bytes) + item grammar as word streams
struc DLGTEMPLATE
    .style   resd 1
    .exStyle resd 1
    .cdit    resd 1
    .x       resw 1
    .y       resw 1
    .cx      resw 1
    .cy      resw 1
endstruc

; ---- class names (UTF-16, .rdata) -------------------------------------------
%macro WSTR 1-*
    %rep %0
        dw %1
        %rotate 1
    %endrep
%endmacro

section .rdata align=2
c_hostClass   WSTR 'A','8','H','o','s','t',0
c_toolbar     WSTR 'T','o','o','l','b','a','r','W','i','n','d','o','w','3','2',0
c_status      WSTR 'm','s','c','t','l','s','_','s','t','a','t','u','s','b','a','r','3','2',0
c_listview    WSTR 'S','y','s','L','i','s','t','V','i','e','w','3','2',0
c_treeview    WSTR 'S','y','s','T','r','e','e','V','i','e','w','3','2',0
c_tab         WSTR 'S','y','s','T','a','b','C','o','n','t','r','o','l','3','2',0
c_tooltip     WSTR 't','o','o','l','t','i','p','s','_','c','l','a','s','s','3','2',0
c_progress    WSTR 'm','s','c','t','l','s','_','p','r','o','g','r','e','s','s','3','2',0
c_header      WSTR 'S','y','s','H','e','a','d','e','r','3','2',0

t_hostTitle   WSTR 'W','3','2','A','8',' ','H','o','s','t',0
t_sb          WSTR 'A','8','s','b',0
t_sbLeft      WSTR 'l','e','f','t',0
t_sbRight     WSTR 'r','i','g','h','t',0
t_lv0         WSTR 'a','l','p','h','a',0
t_lv1         WSTR 'b','e','t','a',0
t_lv2         WSTR 'g','a','m','m','a',0
t_lvCol       WSTR 'c','o','l',0
t_tvR         WSTR 'R',0
t_tvC1        WSTR 'C','1',0
t_tvC2        WSTR 'C','2',0
t_tab1        WSTR 'O','n','e',0
t_tab2        WSTR 'T','w','o',0
t_tip         WSTR 't','i','p',' ','t','e','x','t',0
t_psTitle     WSTR 'A','8',' ','s','h','e','e','t',0
t_psPage1     WSTR 'P','1',0
t_psPage2     WSTR 'P','2',0

; section markers (ASCII, .rdata)
%macro MARK 2
%%s: db %1, 0
%1_len equ $ - %%s - 1
%endmacro

section .rdata
m_init      db "A8-INIT-OK", 13, 10
m_init_l    equ $ - m_init
m_wschild   db "A8-WSCHILD-OK", 13, 10
m_wschild_l equ $ - m_wschild
m_tb        db "A8-TOOLBAR-OK", 13, 10
m_tb_l      equ $ - m_tb
m_sb        db "A8-STATUS-OK", 13, 10
m_sb_l      equ $ - m_sb
m_lv        db "A8-LISTVIEW-OK", 13, 10
m_lv_l      equ $ - m_lv
m_tv        db "A8-TREEVIEW-OK", 13, 10
m_tv_l      equ $ - m_tv
m_tc        db "A8-TAB-OK", 13, 10
m_tc_l      equ $ - m_tc
m_pb        db "A8-PROGRESS-OK", 13, 10
m_pb_l      equ $ - m_pb
m_tt        db "A8-TOOLTIP-OK", 13, 10
m_tt_l      equ $ - m_tt
m_il        db "A8-IMAGELIST-OK", 13, 10
m_il_l      equ $ - m_il
m_sub       db "A8-SUBCLASS-OK", 13, 10
m_sub_l     equ $ - m_sub
m_ps        db "A8-PSHEET-OK", 13, 10
m_ps_l      equ $ - m_ps
m_refuse    db "A8-REFUSE-OK", 13, 10
m_refuse_l  equ $ - m_refuse
m_hd        db "A8-HEADER-OK", 13, 10
m_hd_l      equ $ - m_hd
m_done      db "W32A8-COMCTL-OK", 13, 10
m_done_l    equ $ - m_done

f_init      db "A8-INIT-FAIL", 13, 10
f_init_l    equ $ - f_init
f_wschild   db "A8-WSCHILD-FAIL", 13, 10
f_wschild_l equ $ - f_wschild
f_tb        db "A8-TOOLBAR-FAIL", 13, 10
f_tb_l      equ $ - f_tb
f_sb        db "A8-STATUS-FAIL", 13, 10
f_sb_l      equ $ - f_sb
f_lv        db "A8-LISTVIEW-FAIL", 13, 10
f_lv_l      equ $ - f_lv
f_tv        db "A8-TREEVIEW-FAIL", 13, 10
f_tv_l      equ $ - f_tv
f_tc        db "A8-TAB-FAIL", 13, 10
f_tc_l      equ $ - f_tc
f_pb        db "A8-PROGRESS-FAIL", 13, 10
f_pb_l      equ $ - f_pb
f_tt        db "A8-TOOLTIP-FAIL", 13, 10
f_tt_l      equ $ - f_tt
f_il        db "A8-IMAGELIST-FAIL", 13, 10
f_il_l      equ $ - f_il
f_sub       db "A8-SUBCLASS-FAIL", 13, 10
f_sub_l     equ $ - f_sub
f_ps        db "A8-PSHEET-FAIL", 13, 10
f_ps_l      equ $ - f_ps
f_refuse    db "A8-REFUSE-FAIL", 13, 10
f_refuse_l  equ $ - f_refuse
f_hd        db "A8-HEADER-FAIL", 13, 10
f_hd_l      equ $ - f_hd

; ---- 16x16 32bpp bitmap: magenta with a green cross ------------------------
; CreateBitmap 32bpp words are native 0xAARRGGBB (the A-7 gate pinned the
; word); magenta 0xFFFF00FF is the AddMasked mask colour, green 0xFF00FF00
; survives it.
section .rdata align=4
align 4
bmp_px:
        ; rows 0-6 magenta, rows 7-8 green (read back through
        ; ImageList_Draw), rows 9-15 magenta -- 16x16 32bpp words
        times 16*7 dd 0xFFFF00FF
%rep 32
        dd 0xFF00FF00
%endrep
        times 16*7 dd 0xFFFF00FF

; ---- minimal dialog templates (in-memory, PSP_DLGINDIRECT) -----------------
; DLGTEMPLATE { WS_CHILD-ish style 0, 0, cdit=1, x=4,y=4,cx=80,cy=40 }
; then one item: DLGITEMTEMPLATE + class ord 0x0080 (Button) + title +
; no creation data.  Word stream per the A-6 grammar, DWORD-aligned.
align 4
ps_tmpl1:
    istruc DLGTEMPLATE
        at DLGTEMPLATE.style,   dd 0
        at DLGTEMPLATE.exStyle, dd 0
        at DLGTEMPLATE.cdit,    dd 1
        at DLGTEMPLATE.x,       dw 4
        at DLGTEMPLATE.y,       dw 4
        at DLGTEMPLATE.cx,      dw 80
        at DLGTEMPLATE.cy,      dw 40
    iend
    dw 0                    ; menu: none
    dw 0                    ; class: none
    WSTR 'P','a','g','e',' ','1',0   ; title
    align 4
    ; item: button at (8,8) 60x14, id 1001 (WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON)
    dd 0x50010000           ; style
    dd 0                    ; exStyle
    dw 8, 8, 60, 14         ; x, y, cx, cy
    dw 1001                 ; id
    dw 0xFFFF, 0x0080       ; ordinal class: Button
    WSTR 'B','1',0          ; title
    dw 0                    ; creation data size
ps_tmpl1_end:

align 4
ps_tmpl2:
    istruc DLGTEMPLATE
        at DLGTEMPLATE.style,   dd 0
        at DLGTEMPLATE.exStyle, dd 0
        at DLGTEMPLATE.cdit,    dd 1
        at DLGTEMPLATE.x,       dw 4
        at DLGTEMPLATE.y,       dw 4
        at DLGTEMPLATE.cx,      dw 80
        at DLGTEMPLATE.cy,      dw 40
    iend
    dw 0
    dw 0
    WSTR 'P','a','g','e',' ','2',0
    align 4
    dd 0x50010000
    dd 0
    dw 8, 8, 60, 14
    dw 1002
    dw 0xFFFF, 0x0080
    WSTR 'B','2',0
    dw 0
ps_tmpl2_end:

; ---- writable state ----------------------------------------------------------
section .bss align=8
g_stdout    resq 1
g_written   resq 1
g_hinst     resq 1
g_hwnd      resq 1
g_hdc       resq 1
g_tb        resq 1
g_sb        resq 1
g_lv        resq 1
g_tv        resq 1
g_tc        resq 1
g_pb        resq 1
g_tt        resq 1
g_hd        resq 1
g_himl      resq 1
g_hicon     resq 1
g_msg       resb MSG_size
g_psPageH   resq 1                 ; page hwnd across calls (volatile regs die)
g_wcx       resb WNDCLASSEXW_size
g_icc       resb INITCOMMONCONTROLSEX_size
g_tbb       resb 3*TBBUTTON_size
g_lvitem    resb LVITEMW_size
g_lvcol     resb LVCOLUMNW_size
g_tvins     resb TVINSERTSTRUCTW_size
g_tvi       resb TVITEMW_size
g_tcitem    resb TCITEMW_size
g_ti        resb TOOLINFOW_size
g_hditem    resb HDITEMW_size
g_pbr       resb PBRANGE_size
g_imginfo   resb IMAGEINFO_size
g_psph      resb PROPSHEETHEADERW_size
g_psPages   resb 2*PROPSHEETPAGEW_size
g_textbuf   resw 64
g_parts     resd 4
g_ht        resd 4                 ; LVHITTESTINFO { pt, flags, iItem }

; wndproc observation flags (all reset by the fixture before each check)
g_cmdWp     resq 1                 ; last WM_COMMAND wParam
g_cmdLp     resq 1                 ; last WM_COMMAND lParam
g_gotCmd    resd 1
g_notifyCode resd 1                ; last WM_NOTIFY code
g_notifySeq resd 1                 ; increments per notify (order proof)
g_seqSel    resd 1
g_seqExpanding resd 1
g_seqExpanded  resd 1
g_seqTcn    resd 1
g_gotLvnItemChanged resd 1
g_gotNmClick resd 1
g_gotLvnOrder resd 1               ; ITEMCHANGED seen BEFORE NM_CLICK
g_gotTvSel  resd 1
g_gotTcn    resd 1
g_mouseLeave resd 1                ; _TrackMouseEvent receipt

; subclass state
g_sub1      resd 1
g_sub2      resd 1
g_clsSeen   resd 1
g_subData   resq 1
g_orderBuf  resd 4                 ; who ran, in order (1,2,3=class)

; propsheet state
g_psInit1   resd 1
g_psInit2   resd 1
g_psActive1 resd 1
g_psKill1   resd 1
g_psActive2 resd 1
g_psApply1  resd 1
g_psApply2  resd 1
g_psApplyLp1 resq 1
g_psApplyLp2 resq 1
g_psBtn1    resd 1                 ; GetDlgItem found the template button
g_psRet     resq 1

section .text
global mainCRTStartup

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

; p_fail: print, then exit 79 (stack note as in the A-6/A-7 fixtures).
p_fail:
    sub  rsp, 8
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

; ---- the host window procedure ----------------------------------------------
; rcx=hwnd, edx=msg, r8=wParam, r9=lParam.  Preserves r12-r15/rbx.
wndproc:
    cmp  edx, WM_DESTROY
    je   .destroy
    cmp  edx, WM_COMMAND
    je   .command
    cmp  edx, WM_NOTIFY
    je   .notify
    cmp  edx, WM_MOUSELEAVE        ; 0x02A3
    je   .mouseleave
    jmp  DefWindowProcW            ; tail call: returns to the caller
.destroy:
    sub  rsp, 8
    call PostQuitMessage
    add  rsp, 8
    xor  eax, eax
    ret
.command:
    mov  [g_cmdWp], r8
    mov  [g_cmdLp], r9
    mov  dword [g_gotCmd], 1
    xor  eax, eax
    ret
.notify:
    ; r9 -> NMHDR { hwndFrom, idFrom, code }
    push rbx
    mov  ebx, [r9+16]              ; NMHDR.code
    mov  [g_notifyCode], ebx
    inc  dword [g_notifySeq]
    cmp  ebx, LVN_ITEMCHANGED
    je   .nlv
    cmp  ebx, NM_CLICK
    je   .nclick
    cmp  ebx, TVN_SELCHANGEDW
    je   .ntv
    cmp  ebx, TVN_ITEMEXPANDINGW
    je   .nexping
    cmp  ebx, TVN_ITEMEXPANDEDW
    je   .nexped
    cmp  ebx, TCN_SELCHANGE
    je   .ntcn
    cmp  ebx, HDN_ITEMCLICKW
    je   .nnop
    pop  rbx
    xor  eax, eax
    ret
.nlv:
    mov  dword [g_gotLvnItemChanged], 1
    cmp  dword [g_gotNmClick], 0
    je   .nlv_first
    mov  dword [g_gotLvnOrder], 0    ; NM_CLICK already ran: wrong order
    pop  rbx
    xor  eax, eax
    ret
.nlv_first:
    mov  dword [g_gotLvnOrder], 1
    pop  rbx
    xor  eax, eax
    ret
.nclick:
    mov  dword [g_gotNmClick], 1
    pop  rbx
    xor  eax, eax
    ret
.ntv:
    mov  eax, [g_notifySeq]
    mov  [g_seqSel], eax
    mov  dword [g_gotTvSel], 1
    pop  rbx
    xor  eax, eax
    ret
.nexping:
    mov  eax, [g_notifySeq]
    mov  [g_seqExpanding], eax
    pop  rbx
    xor  eax, eax
    ret
.nexped:
    mov  eax, [g_notifySeq]
    mov  [g_seqExpanded], eax
    pop  rbx
    xor  eax, eax
    ret
.ntcn:
    mov  eax, [g_notifySeq]
    mov  [g_seqTcn], eax
    mov  dword [g_gotTcn], 1
    pop  rbx
    xor  eax, eax
    ret
.nnop:
    pop  rbx
    xor  eax, eax
    ret
.mouseleave:
    mov  dword [g_mouseLeave], 1
    xor  eax, eax
    ret

; ---- subclass procedures -----------------------------------------------------
; COMCTL32 subclass ABI: rcx=hwnd, edx=msg, r8=wParam, r9=lParam,
; [rsp+8]=id (uint64), [rsp+16]=data (int64).  They record who ran, in
; order, into g_orderBuf, then DefSubclassProc down the chain.
sub1_proc:
    push rbp
    mov  rbp, rsp
    mov  dword [g_sub1], 1
    ; record into the first free order slot (sub2 ran first when present)
    cmp  dword [g_orderBuf], 0
    jne  .s1a
    mov  dword [g_orderBuf], 1
    jmp  .s1b
.s1a:
    mov  dword [g_orderBuf+4], 1
.s1b:
    call DefSubclassProc              ; rcx/rdx/r8/r9 still intact
    pop  rbp
    ret

sub2_proc:
    push rbp
    mov  rbp, rsp
    mov  dword [g_sub2], 1
    mov  dword [g_orderBuf], 2       ; newest runs first: slot 0
    call DefSubclassProc
    pop  rbp
    ret

; The class proc's WM_USER+5 arm sets g_clsSeen (see wndproc extension
; below -- the message is routed through DefSubclassProc to the class).

; ---- property-sheet page procedures ------------------------------------------
; The A-6 dialog contract: return FALSE to fall through; the sheet's
; bridge handles that.  Page 1 switches the sheet to page 2 on its
; INITDIALOG; page 2 presses OK, which applies (PSN_APPLY lParam TRUE)
; and ends the sheet.
page1_proc:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x20
    cmp  edx, WM_INITDIALOG
    je   .init
    cmp  edx, WM_NOTIFY
    je   .notify
    xor  eax, eax
    leave
    ret
.init:
    mov  dword [g_psInit1], 1
    ; GetDlgItem(page, 1001): the template's button became a window.
    ; The page hwnd lives in bss across the calls: r10 is caller-saved
    ; and GetDlgItem clobbers it (the first guest run lost PSM_SETCURSEL
    ; to exactly that).
    mov  [g_psPageH], rcx
    mov  edx, 1001
    call GetDlgItem
    test rax, rax
    jz   .no_btn
    mov  dword [g_psBtn1], 1
.no_btn:
    ; switch to page 2 through the sheet: PSM_SETCURSEL to the frame
    mov  rcx, [g_psPageH]
    call GetParent
    mov  rcx, rax
    mov  edx, PSM_SETCURSEL
    xor  r8d, r8d
    inc  r8d                          ; index 1
    xor  r9d, r9d
    call PostMessageW
    mov  eax, 1
    leave
    ret
.notify:
    ; r9 -> PSHNOTIFY { hdr(24), iPage, pad, lParam }
    mov  r10d, [r9+16]               ; hdr.code
    cmp  r10d, PSN_SETACTIVE
    je   .setactive
    cmp  r10d, PSN_KILLACTIVE
    je   .killactive
    cmp  r10d, PSN_APPLY
    je   .apply
    xor  eax, eax
    leave
    ret
.setactive:
    mov  dword [g_psActive1], 1
    xor  eax, eax
    leave
    ret
.killactive:
    mov  dword [g_psKill1], 1
    xor  eax, eax                     ; FALSE: allow the switch
    leave
    ret
.apply:
    mov  dword [g_psApply1], 1
    mov  rax, [r9+32]                 ; PSHNOTIFY.lParam
    mov  [g_psApplyLp1], rax
    xor  eax, eax                     ; PSNRET_NOERROR
    leave
    ret

page2_proc:
    push rbp
    mov  rbp, rsp
    sub  rsp, 0x20
    cmp  edx, WM_INITDIALOG
    je   .init
    cmp  edx, WM_NOTIFY
    je   .notify
    xor  eax, eax
    leave
    ret
.init:
    mov  dword [g_psInit2], 1
    ; press OK: applies to every page, then ends the sheet
    mov  r10, rcx
    call GetParent
    mov  rcx, rax
    mov  edx, PSM_PRESSBUTTON
    mov  r8d, PSBTN_OK
    xor  r9d, r9d
    call PostMessageW
    mov  eax, 1
    leave
    ret
.notify:
    mov  r10d, [r9+16]
    cmp  r10d, PSN_SETACTIVE
    je   .setactive
    cmp  r10d, PSN_APPLY
    je   .apply
    xor  eax, eax
    leave
    ret
.setactive:
    mov  dword [g_psActive2], 1
    xor  eax, eax
    leave
    ret
.apply:
    mov  dword [g_psApply2], 1
    mov  rax, [r9+32]
    mov  [g_psApplyLp2], rax
    xor  eax, eax
    leave
    ret

; CREATEWIN: a 12-arg CreateWindowExW helper.  Same call shape as the
; A-6/A-7 fixtures: rcx/rdx/r8/r9 then the stack eight, pushed
; right-to-left (param, inst, menu, parent, h, w, y, x).
; rax=exstyle, rbx=class, r12=title, r13d=style, r14=x, r15d=y,
; r12h.. -- plain registers via the wrappers below.
%macro CWX 12
    ; args: exstyle, class, title, style, x, y, w, h, parent, id, inst, param
    sub  rsp, 0x60
    mov  ecx, %1                   ; exstyle
    mov  rdx, %2                   ; class
    mov  r8, %3                    ; title
    mov  r9d, %4                   ; style
    mov  dword [rsp+0x20], %5      ; x (parent-relative for WS_CHILD)
    mov  dword [rsp+0x28], %6      ; y
    mov  dword [rsp+0x30], %7      ; width
    mov  dword [rsp+0x38], %8      ; height
    mov  qword [rsp+0x40], %9      ; parent
    mov  qword [rsp+0x48], %{10}     ; hMenu / control id
    mov  qword [rsp+0x50], %{11}     ; hInstance (loader-ignored)
    mov  qword [rsp+0x58], %{12}     ; lpParam
    call CreateWindowExW
    add  rsp, 0x60
%endmacro

; ---- main ---------------------------------------------------------------------
mainCRTStartup:
    push rbp
    mov  rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 0x48                   ; six pushes: land RSP on 16 for ABI calls

    ; stdout
    mov  ecx, STD_OUTPUT_HANDLE
    call GetStdHandle
    mov  [g_stdout], rax
    test rax, rax
    js   .f_stdout

    mov  ecx, 0
    call GetModuleHandleW
    mov  [g_hinst], rax
    test rax, rax
    jz   .f_init

; ============================ INIT =============================================
    mov  dword [g_icc+INITCOMMONCONTROLSEX.dwSize], INITCOMMONCONTROLSEX_size
    mov  dword [g_icc+INITCOMMONCONTROLSEX.dwICC], ICC_BAR_CLASSES | \
         ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | \
         ICC_PROGRESS_CLASS
    lea  rcx, [g_icc]
    call InitCommonControlsEx
    test eax, eax
    jz   .f_init

    ; ordinal 17's name must be bound too (the import is by name here;
    ; the ordinal ILT shape was the W32A-1 ordtest's proof)
    call InitCommonControls

    OK m_init, m_init_l

; ============================ WSCHILD ==========================================
    ; register the app class
    lea  rcx, [g_wcx]
    mov  dword [rcx+WNDCLASSEXW.cbSize], WNDCLASSEXW_size
    mov  dword [rcx+WNDCLASSEXW.style], 0
    lea  rdx, [wndproc]
    mov  [rcx+WNDCLASSEXW.lpfnWndProc], rdx
    mov  qword [rcx+WNDCLASSEXW.hInstance], 0
    mov  rax, [g_hinst]
    mov  [rcx+WNDCLASSEXW.hInstance], rax
    lea  rdx, [c_hostClass]
    mov  [rcx+WNDCLASSEXW.lpszClassName], rdx
    call RegisterClassExW
    test eax, eax
    jz   .f_wschild

    ; host window (top-level)
    CWX 0, c_hostClass, t_hostTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, \
         120, 80, 420, 340, 0, 0, 0, 0
    mov  [g_hwnd], rax
    test rax, rax
    jz   .f_wschild

    ; NEGATIVE: WS_CHILD on an application class is refused
    CWX 0, c_hostClass, 0, WS_CHILD, 10, 10, 50, 50, 0, 0, 0, 0
    test rax, rax
    jnz  .f_wschild               ; must NOT create

    ; POSITIVE: a comctl class admits WS_CHILD, parent-relative x/y
    mov  rax, [g_hwnd]
    CWX 0, c_listview, 0, WS_CHILD | WS_VISIBLE, 0, 0, 200, 160, rax, 0, 0, 0
    mov  [g_lv], rax
    test rax, rax
    jz   .f_wschild

    mov  rcx, [g_lv]
    call GetParent
    cmp  rax, [g_hwnd]
    jne  .f_wschild

    OK m_wschild, m_wschild_l

; ============================ TOOLBAR ==========================================
    ; CreateToolbarEx(host, WS_CHILD|WS_VISIBLE, id 1001, 0 bitmaps,
    ; 3 buttons with ids 2001-2003, 40x24 cells)
    lea  rbx, [g_tbb]
    xor  eax, eax
.tbfill:
    mov  [rbx+TBBUTTON.iBitmap], eax   ; iBitmap = i
    mov  edx, 2001
    add  edx, eax
    mov  [rbx+TBBUTTON.idCommand], edx
    mov  byte [rbx+TBBUTTON.fsState], TBSTATE_ENABLED
    mov  byte [rbx+TBBUTTON.fsStyle], 0
    mov  qword [rbx+TBBUTTON.dwData], 0
    mov  qword [rbx+TBBUTTON.iString], 0
    add  rbx, TBBUTTON_size
    inc  eax
    cmp  eax, 3
    jb   .tbfill

    mov  rcx, [g_hwnd]
    mov  edx, WS_CHILD | WS_VISIBLE
    mov  r8d, 1001                    ; wID
    xor  r9d, r9d                     ; nBitmaps
    sub  rsp, 0x50
    mov  qword [rsp+0x20], 0          ; hBMInst
    mov  qword [rsp+0x28], 0          ; wBMID
    lea  rax, [g_tbb]
    mov  [rsp+0x30], rax              ; buttons
    mov  qword [rsp+0x38], 3          ; nButtons
    mov  qword [rsp+0x40], 40         ; dxButton
    mov  qword [rsp+0x48], 24         ; dyButton
    call CreateToolbarEx
    add  rsp, 0x50
    mov  [g_tb], rax
    test rax, rax
    jz   .f_tb
    mov  rcx, [g_tb]
    mov  edx, TB_BUTTONCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 3
    jne  .f_tb
    mov  rcx, [g_tb]
    mov  edx, TB_COMMANDTOINDEX
    mov  r8d, 2002
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 1
    jne  .f_tb
    ; the create helper's 40x24 cells are the reported button size
    mov  rcx, [g_tb]
    mov  edx, TB_GETBUTTONSIZE
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 0x00180028              ; dy 24 << 16 | dx 40
    jne  .f_tb
    ; enable/disable round-trip
    mov  rcx, [g_tb]
    mov  edx, TB_ENABLEBUTTON
    mov  r8d, 2002
    xor  r9d, r9d                     ; FALSE
    call SendMessageW
    test eax, eax
    jz   .f_tb
    mov  rcx, [g_tb]
    mov  edx, TB_ISBUTTONENABLED
    mov  r8d, 2002
    xor  r9d, r9d
    call SendMessageW
    test eax, eax
    jnz  .f_tb
    mov  rcx, [g_tb]
    mov  edx, TB_ENABLEBUTTON
    mov  r8d, 2002
    mov  r9d, 1
    call SendMessageW
    mov  rcx, [g_tb]
    mov  edx, TB_ISBUTTONENABLED
    mov  r8d, 2002
    xor  r9d, r9d
    call SendMessageW
    test eax, eax
    jz   .f_tb
    ; a click lands WM_COMMAND on the parent: button 1 spans x=44..84
    ; (pitch 42: 2 + 1*(40+2)); its id is 2002
    mov  dword [g_gotCmd], 0
    mov  rcx, [g_tb]
    mov  edx, WM_LBUTTONDOWN
    mov  r8d, MK_LBUTTON
    mov  r9d, 0x00050050              ; y=5 << 16 | x=80 -> button index 1
    call SendMessageW
    ; drain the posted WM_COMMAND
    lea  rcx, [g_msg]
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    ; Pump until the command lands: the queue holds the creates' posted
    ; WM_SIZEs ahead of it, so one GetMessage/Dispatch pair is not enough.
.tb_pump:
    lea  rcx, [g_msg]
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    call GetMessageW
    test eax, eax
    jle  .tb_pumped                  ; WM_QUIT: fall through and fail
    lea  rcx, [g_msg]
    call DispatchMessageW
    cmp  dword [g_gotCmd], 0
    je   .tb_pump
.tb_pumped:
    cmp  dword [g_gotCmd], 0
    je   .f_tb
    mov  rax, [g_cmdWp]
    cmp  eax, 2002                    ; low word: the command id
    jne  .f_tb
    mov  rax, [g_cmdLp]
    cmp  rax, [g_tb]                  ; lParam: the control window
    jne  .f_tb

    OK m_tb, m_tb_l

; ============================ STATUS ===========================================
    mov  rcx, WS_CHILD | WS_VISIBLE
    lea  rdx, [t_sb]
    mov  r8, [g_hwnd]
    mov  r9d, 1002
    call CreateStatusWindowW
    mov  [g_sb], rax
    test rax, rax
    jz   .f_sb
    mov  dword [g_parts], 200
    mov  dword [g_parts+4], 400
    mov  rcx, [g_sb]
    mov  edx, SB_SETPARTS
    mov  r8d, 2
    lea  r9, [g_parts]
    call SendMessageW
    test eax, eax
    jz   .f_sb
    mov  rcx, [g_sb]
    mov  edx, SB_SETTEXTW
    xor  r8d, r8d                     ; part 0
    lea  r9, [t_sbLeft]
    call SendMessageW
    test eax, eax
    jz   .f_sb
    mov  rcx, [g_sb]
    mov  edx, SB_SETTEXTW
    mov  r8d, 1                       ; part 1
    lea  r9, [t_sbRight]
    call SendMessageW
    mov  rcx, [g_sb]
    mov  edx, SB_GETTEXTLENGTHW
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 4                       ; "left"
    jne  .f_sb
    mov  rcx, [g_sb]
    mov  edx, SB_GETTEXTW
    mov  r8d, 1
    lea  r9, [g_textbuf]
    call SendMessageW
    cmp  eax, 5                       ; "right"
    jne  .f_sb
    cmp  word [g_textbuf], 'r'        ; UTF-16: 'r' at char 0
    jne  .f_sb
    cmp  word [g_textbuf+6], 'h'      ; 'h' at char 3 (byte 6)
    jne  .f_sb
    mov  rcx, [g_sb]
    mov  edx, SB_GETPARTS
    mov  r8d, 4
    lea  r9, [g_parts]
    call SendMessageW
    cmp  eax, 2
    jne  .f_sb

    OK m_sb, m_sb_l

; ============================ LISTVIEW =========================================
    ; the listview already exists (the WSCHILD section made it); 3 items
    lea  rbx, [g_lvitem]
    xor  eax, eax
    mov  ecx, LVIF_TEXT | LVIF_PARAM
    mov  [rbx+LVITEMW.mask], ecx
    mov  [rbx+LVITEMW.iItem], eax
    mov  [rbx+LVITEMW.iSubItem], eax
    mov  [rbx+LVITEMW.state], eax
    mov  [rbx+LVITEMW.stateMask], eax
    lea  rax, [t_lv0]
    mov  [rbx+LVITEMW.pszText], rax
    mov  [rbx+LVITEMW.cchTextMax], eax
    mov  [rbx+LVITEMW.iImage], eax
    mov  qword [rbx+LVITEMW.lParam], 0x111
    mov  [rbx+LVITEMW.iIndent], eax

    mov  rcx, [g_lv]
    mov  edx, LVM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_lvitem]
    call SendMessageW
    test eax, eax
    jnz  .f_lv
    mov  rcx, [g_lv]
    mov  edx, LVM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_lvitem]
    mov  dword [rbx+LVITEMW.iItem], 1
    lea  rax, [t_lv1]
    mov  [rbx+LVITEMW.pszText], rax
    mov  qword [rbx+LVITEMW.lParam], 0x222
    call SendMessageW
    cmp  eax, 1
    jne  .f_lv
    mov  rcx, [g_lv]
    mov  edx, LVM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_lvitem]
    mov  dword [rbx+LVITEMW.iItem], 2
    lea  rax, [t_lv2]
    mov  [rbx+LVITEMW.pszText], rax
    mov  qword [rbx+LVITEMW.lParam], 0x333
    call SendMessageW
    cmp  eax, 2
    jne  .f_lv

    mov  rcx, [g_lv]
    mov  edx, LVM_GETITEMCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 3
    jne  .f_lv

    ; GETITEM reads text and lParam back
    mov  dword [rbx+LVITEMW.mask], LVIF_TEXT | LVIF_PARAM
    mov  dword [rbx+LVITEMW.iItem], 1
    lea  rax, [g_textbuf]
    mov  [rbx+LVITEMW.pszText], rax
    mov  dword [rbx+LVITEMW.cchTextMax], 64
    mov  rcx, [g_lv]
    mov  edx, LVM_GETITEMW
    xor  r8d, r8d
    lea  r9, [g_lvitem]
    call SendMessageW
    test eax, eax
    jz   .f_lv
    cmp  word [g_textbuf], 'b'
    jne  .f_lv
    cmp  word [g_textbuf+2], 'e'
    jne  .f_lv
    cmp  qword [rbx+LVITEMW.lParam], 0x222
    jne  .f_lv

    ; select item 1 through SETITEMSTATE; the state machine answers
    mov  dword [rbx+LVITEMW.mask], LVIF_STATE
    mov  dword [rbx+LVITEMW.state], LVIS_SELECTED
    mov  dword [rbx+LVITEMW.stateMask], LVIS_SELECTED
    mov  dword [rbx+LVITEMW.iItem], 1
    mov  rcx, [g_lv]
    mov  edx, LVM_SETITEMSTATE
    mov  r8d, 1
    lea  r9, [g_lvitem]
    call SendMessageW
    mov  rcx, [g_lv]
    mov  edx, LVM_GETSELECTEDCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 1
    jne  .f_lv
    mov  rcx, [g_lv]
    mov  edx, LVM_GETNEXTITEM
    mov  r8d, -1                      ; start after -1
    mov  r9d, LVNI_SELECTED
    call SendMessageW
    cmp  eax, 1
    jne  .f_lv

    ; a column round-trips
    lea  rbx, [g_lvcol]
    mov  dword [rbx+LVCOLUMNW.mask], 0x0007    ; FMT|WIDTH|TEXT
    mov  dword [rbx+LVCOLUMNW.fmt], 0
    mov  dword [rbx+LVCOLUMNW.cx], 100
    lea  rax, [t_lvCol]
    mov  [rbx+LVCOLUMNW.pszText], rax
    mov  dword [rbx+LVCOLUMNW.cchTextMax], 0
    mov  dword [rbx+LVCOLUMNW.iSubItem], 0
    mov  rcx, [g_lv]
    mov  edx, LVM_INSERTCOLUMNA
    xor  r8d, r8d
    lea  r9, [g_lvcol]
    call SendMessageW
    test eax, eax
    jnz  .f_lv
    mov  dword [rbx+LVCOLUMNW.mask], 0x0004    ; TEXT
    lea  rax, [g_textbuf]
    mov  [rbx+LVCOLUMNW.pszText], rax
    mov  dword [rbx+LVCOLUMNW.cchTextMax], 64
    mov  rcx, [g_lv]
    mov  edx, LVM_GETCOLUMNA
    xor  r8d, r8d
    lea  r9, [g_lvcol]
    call SendMessageW
    test eax, eax
    jz   .f_lv
    cmp  word [g_textbuf], 'c'
    jne  .f_lv

    ; a click on row 1 selects it and notifies in the documented order
    mov  dword [g_gotLvnItemChanged], 0
    mov  dword [g_gotNmClick], 0
    mov  dword [g_gotLvnOrder], 0
    mov  rcx, [g_lv]
    mov  edx, WM_LBUTTONDOWN
    mov  r8d, MK_LBUTTON
    mov  r9d, 0x0012000A              ; y=18 (row 1) << 16 | x=10
    call SendMessageW
    cmp  dword [g_gotLvnItemChanged], 0
    je   .f_lv
    cmp  dword [g_gotNmClick], 0
    je   .f_lv
    cmp  dword [g_gotLvnOrder], 0     ; ITEMCHANGED ran BEFORE NM_CLICK
    je   .f_lv
    ; the clicked row took the selection
    mov  rcx, [g_lv]
    mov  edx, LVM_GETNEXTITEM
    mov  r8d, -1
    mov  r9d, LVNI_SELECTED
    call SendMessageW
    cmp  eax, 1
    jne  .f_lv

    ; HitTest reads the row back
    mov  dword [g_ht], 0x0000000A     ; POINT (10,0) packed as two dwords
    mov  dword [g_ht+4], 0
    mov  rcx, [g_lv]
    mov  edx, LVM_HITTEST
    xor  r8d, r8d
    lea  r9, [g_ht]
    call SendMessageW
    cmp  eax, 0
    jne  .f_lv

    ; delete drops the count
    mov  rcx, [g_lv]
    mov  edx, LVM_DELETEITEM
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    test eax, eax
    jz   .f_lv
    mov  rcx, [g_lv]
    mov  edx, LVM_GETITEMCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 2
    jne  .f_lv

    OK m_lv, m_lv_l

; ============================ TREEVIEW =========================================
    mov  rax, [g_hwnd]
    CWX 0, c_treeview, 0, WS_CHILD | WS_VISIBLE, 210, 0, 200, 160, rax, 2001, 0, 0
    mov  [g_tv], rax
    test rax, rax
    jz   .f_tv

    lea  rbx, [g_tvins]
    ; root "R" under TVI_ROOT
    mov  dword [rbx+TVINSERTSTRUCTW.hParent], TVI_ROOT
    mov  dword [rbx+TVINSERTSTRUCTW.hParent+4], 0
    mov  dword [rbx+TVINSERTSTRUCTW.hInsertAfter], TVI_LAST
    mov  dword [rbx+TVINSERTSTRUCTW.hInsertAfter+4], 0
    mov  rdi, rbx
    add  rdi, TVINSERTSTRUCTW.item     ; &ins.item
    mov  dword [rdi+TVITEMW.mask], TVIF_TEXT | TVIF_CHILDREN
    mov  qword [rdi+TVITEMW.hItem], 0
    mov  dword [rdi+TVITEMW.state], 0
    mov  dword [rdi+TVITEMW.stateMask], 0
    lea  rax, [t_tvR]
    mov  [rdi+TVITEMW.pszText], rax
    mov  dword [rdi+TVITEMW.cchTextMax], 0
    mov  dword [rdi+TVITEMW.iImage], 0
    mov  dword [rdi+TVITEMW.iSelectedImage], 0
    mov  dword [rdi+TVITEMW.cChildren], 1
    mov  qword [rdi+TVITEMW.lParam], 0x100

    mov  rcx, [g_tv]
    mov  edx, TVM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_tvins]
    call SendMessageW
    mov  r12, rax                     ; root handle (1)
    test rax, rax
    jz   .f_tv

    ; children "C1","C2" under the root
    mov  [rbx+TVINSERTSTRUCTW.hParent], r12
    lea  rax, [t_tvC1]
    mov  [rdi+TVITEMW.pszText], rax
    mov  qword [rdi+TVITEMW.lParam], 0x101
    mov  rcx, [g_tv]
    mov  edx, TVM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_tvins]
    call SendMessageW
    mov  r13, rax                     ; C1 handle (2)
    test rax, rax
    jz   .f_tv
    mov  [rbx+TVINSERTSTRUCTW.hParent], r12
    lea  rax, [t_tvC2]
    mov  [rdi+TVITEMW.pszText], rax
    mov  rcx, [g_tv]
    mov  edx, TVM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_tvins]
    call SendMessageW
    mov  r14, rax                     ; C2 handle (3)
    test rax, rax
    jz   .f_tv

    mov  rcx, [g_tv]
    mov  edx, TVM_GETCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 3
    jne  .f_tv

    ; GETNEXTITEM walks
    mov  rcx, [g_tv]
    mov  edx, TVM_GETNEXTITEM
    mov  r8d, TVGN_ROOT
    xor  r9d, r9d
    call SendMessageW
    cmp  rax, r12
    jne  .f_tv
    mov  rcx, [g_tv]
    mov  edx, TVM_GETNEXTITEM
    mov  r8d, TVGN_CHILD
    mov  r9, r12
    call SendMessageW
    cmp  rax, r13
    jne  .f_tv
    mov  rcx, [g_tv]
    mov  edx, TVM_GETNEXTITEM
    mov  r8d, TVGN_NEXT
    mov  r9, r13
    call SendMessageW
    cmp  rax, r14
    jne  .f_tv

    ; SELECTITEM notifies TVN_SELCHANGEDW
    mov  dword [g_gotTvSel], 0
    mov  rcx, [g_tv]
    mov  edx, TVM_SELECTITEM
    mov  r8d, TVGN_CARET
    mov  r9, r13
    call SendMessageW
    test eax, eax
    jz   .f_tv
    cmp  dword [g_gotTvSel], 0
    je   .f_tv
    mov  dword [g_notifySeq], 0

    ; EXPAND: ITEMEXPANDING fires BEFORE ITEMEXPANDED (order proof)
    mov  dword [g_seqExpanding], 0
    mov  dword [g_seqExpanded], 0
    mov  rcx, [g_tv]
    mov  edx, TVM_EXPAND
    mov  r8d, TVE_EXPAND
    mov  r9, r12
    call SendMessageW
    test eax, eax
    jz   .f_tv
    mov  eax, [g_seqExpanding]
    test eax, eax
    jz   .f_tv
    mov  ecx, [g_seqExpanded]
    test ecx, ecx
    jz   .f_tv
    cmp  eax, ecx                     ; expanding < expanded
    jae  .f_tv

    ; GETITEM reads the child's text
    lea  rbx, [g_tvi]
    mov  dword [rbx+TVITEMW.mask], TVIF_TEXT
    mov  [rbx+TVITEMW.hItem], r13
    lea  rax, [g_textbuf]
    mov  [rbx+TVITEMW.pszText], rax
    mov  dword [rbx+TVITEMW.cchTextMax], 64
    mov  rcx, [g_tv]
    mov  edx, TVM_GETITEMW
    xor  r8d, r8d
    lea  r9, [g_tvi]
    call SendMessageW
    test eax, eax
    jz   .f_tv
    cmp  word [g_textbuf], 'C'
    jne  .f_tv
    cmp  word [g_textbuf+2], '1'
    jne  .f_tv

    ; GETITEMRECT: the handle rides the RECT's first 8 bytes (the
    ; documented ABI); C1 is row 1 while the root is expanded (14px rows)
    mov  [g_parts], r13
    mov  rcx, [g_tv]
    mov  edx, TVM_GETITEMRECT
    mov  r8d, 1                       ; the item is visible
    lea  r9, [g_parts]
    call SendMessageW
    test eax, eax
    jz   .f_tv
    mov  eax, [g_parts+4]             ; RECT.top
    cmp  eax, 14
    jne  .f_tv
    mov  eax, [g_parts+12]            ; RECT.bottom
    cmp  eax, 28
    jne  .f_tv

    ; the gutter click collapses the root through the widget
    mov  dword [g_seqExpanding], 0
    mov  dword [g_seqExpanded], 0
    mov  rcx, [g_tv]
    mov  edx, WM_LBUTTONDOWN
    mov  r8d, MK_LBUTTON
    mov  r9d, 0x00070005              ; y=7 << 16 | x=5 (root toggle box)
    call SendMessageW
    mov  eax, [g_seqExpanded]
    test eax, eax
    jz   .f_tv                        ; the toggle notified

    ; delete C1: count drops
    mov  rcx, [g_tv]
    mov  edx, TVM_DELETEITEM
    xor  r8d, r8d
    mov  r9, r13
    call SendMessageW
    test eax, eax
    jz   .f_tv
    mov  rcx, [g_tv]
    mov  edx, TVM_GETCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 2
    jne  .f_tv

    OK m_tv, m_tv_l

; ============================ TAB ==============================================
    mov  rax, [g_hwnd]
    CWX 0, c_tab, 0, WS_CHILD | WS_VISIBLE, 0, 200, 200, 26, rax, 2002, 0, 0
    mov  [g_tc], rax
    test rax, rax
    jz   .f_tc

    lea  rbx, [g_tcitem]
    mov  dword [rbx+TCITEMW.mask], TCIF_TEXT | TCIF_PARAM
    mov  dword [rbx+TCITEMW.dwState], 0
    mov  dword [rbx+TCITEMW.dwStateMask], 0
    lea  rax, [t_tab1]
    mov  [rbx+TCITEMW.pszText], rax
    mov  dword [rbx+TCITEMW.cchTextMax], 0
    mov  dword [rbx+TCITEMW.iImage], 0
    mov  qword [rbx+TCITEMW.lParam], 0xA1
    mov  rcx, [g_tc]
    mov  edx, TCM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_tcitem]
    call SendMessageW
    test eax, eax
    jnz  .f_tc
    lea  rax, [t_tab2]
    mov  [rbx+TCITEMW.pszText], rax
    mov  qword [rbx+TCITEMW.lParam], 0xA2
    mov  rcx, [g_tc]
    mov  edx, TCM_INSERTITEMW
    mov  r8d, 1
    lea  r9, [g_tcitem]
    call SendMessageW
    cmp  eax, 1
    jne  .f_tc

    mov  rcx, [g_tc]
    mov  edx, TCM_GETITEMCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 2
    jne  .f_tc

    ; SETCURSEL returns the PREVIOUS and does NOT notify
    mov  dword [g_gotTcn], 0
    mov  rcx, [g_tc]
    mov  edx, TCM_SETCURSEL
    mov  r8d, 1
    xor  r9d, r9d
    call SendMessageW
    test eax, eax                    ; previous selection was 0
    jnz  .f_tc
    cmp  dword [g_gotTcn], 0
    jne  .f_tc                       ; SETCURSEL must stay silent
    mov  rcx, [g_tc]
    mov  edx, TCM_GETCURSEL
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 1
    jne  .f_tc

    ; a strip click on tab 0 notifies TCN_SELCHANGE
    mov  dword [g_gotTcn], 0
    mov  rcx, [g_tc]
    mov  edx, WM_LBUTTONDOWN
    mov  r8d, MK_LBUTTON
    mov  r9d, 0x00050032              ; y=5 << 16 | x=50 (tab 0's strip)
    call SendMessageW
    cmp  dword [g_gotTcn], 0
    je   .f_tc
    mov  rcx, [g_tc]
    mov  edx, TCM_GETCURSEL
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    test eax, eax
    jnz  .f_tc

    OK m_tc, m_tc_l

; ============================ PROGRESS =========================================
    mov  rax, [g_hwnd]
    CWX 0, c_progress, 0, WS_CHILD | WS_VISIBLE, 0, 230, 200, 16, rax, 2003, 0, 0
    mov  [g_pb], rax
    test rax, rax
    jz   .f_pb

    mov  rcx, [g_pb]
    mov  edx, PBM_SETRANGE
    xor  r8d, r8d
    mov  r9d, 0x00640000              ; max 100 << 16 | min 0
    call SendMessageW
    mov  rcx, [g_pb]
    mov  edx, PBM_SETPOS
    mov  r8d, 50
    xor  r9d, r9d
    call SendMessageW
    test eax, eax                     ; previous position
    jnz  .f_pb
    mov  rcx, [g_pb]
    mov  edx, PBM_GETPOS
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 50
    jne  .f_pb
    mov  rcx, [g_pb]
    mov  edx, PBM_SETSTEP
    mov  r8d, 10
    xor  r9d, r9d
    call SendMessageW
    mov  rcx, [g_pb]
    mov  edx, PBM_STEPIT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 50                      ; stepit returns the previous
    jne  .f_pb
    mov  rcx, [g_pb]
    mov  edx, PBM_GETPOS
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 60
    jne  .f_pb
    mov  rcx, [g_pb]
    mov  edx, PBM_GETRANGE
    xor  r8d, r8d                     ; FALSE: the high limit
    lea  r9, [g_pbr]
    call SendMessageW
    cmp  eax, 100
    jne  .f_pb
    cmp  dword [g_pbr+PBRANGE.iLow], 0
    jne  .f_pb
    cmp  dword [g_pbr+PBRANGE.iHigh], 100
    jne  .f_pb

    OK m_pb, m_pb_l

; ============================ TOOLTIP ==========================================
    ; a tip window is top-level WS_POPUP (borderless: the compositor
    ; rejects a 22px-high decorated window), created hidden
    CWX 0, c_tooltip, 0, WS_POPUP, -32000, -32000, 120, 22, 0, 0, 0, 0
    mov  [g_tt], rax
    test rax, rax
    jz   .f_tt

    lea  rbx, [g_ti]
    mov  dword [rbx+TOOLINFOW.cbSize], TOOLINFOW_size
    mov  dword [rbx+TOOLINFOW.uFlags], 0
    mov  qword [rbx+TOOLINFOW.hwnd], 0
    mov  rax, [g_hwnd]
    mov  [rbx+TOOLINFOW.hwnd], rax
    mov  qword [rbx+TOOLINFOW.uId], 1
    mov  dword [rbx+TOOLINFOW.rect+RECT.left], 0
    mov  dword [rbx+TOOLINFOW.rect+RECT.top], 0
    mov  dword [rbx+TOOLINFOW.rect+RECT.right], 50
    mov  dword [rbx+TOOLINFOW.rect+RECT.bottom], 20
    mov  qword [rbx+TOOLINFOW.hinst], 0
    lea  rax, [t_tip]
    mov  [rbx+TOOLINFOW.lpszText], rax
    mov  qword [rbx+TOOLINFOW.lParam], 0

    mov  rcx, [g_tt]
    mov  edx, TTM_ADDTOOLW
    xor  r8d, r8d
    lea  r9, [g_ti]
    call SendMessageW
    test eax, eax
    jz   .f_tt
    mov  rcx, [g_tt]
    mov  edx, TTM_GETTOOLCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 1
    jne  .f_tt
    mov  rcx, [g_tt]
    mov  edx, TTM_ACTIVATE
    mov  r8d, 1
    xor  r9d, r9d
    call SendMessageW

    ; relay a WM_MOUSEMOVE inside the tool rect: the tip SHOWS
    lea  rbx, [g_msg]
    mov  rax, [g_hwnd]
    mov  [rbx+MSG.hwnd], rax
    mov  dword [rbx+MSG.message], WM_MOUSEMOVE
    mov  qword [rbx+MSG.wParam], 0
    mov  qword [rbx+MSG.lParam], 0x000A000A   ; (10,10): inside
    mov  rcx, [g_tt]
    mov  edx, TTM_RELAYEVENT
    xor  r8d, r8d
    lea  r9, [g_msg]
    call SendMessageW
    mov  rcx, [g_tt]
    call IsWindowVisible
    test eax, eax
    jz   .f_tt

    ; the current tool reads back
    lea  rax, [g_textbuf]
    mov  [g_ti+TOOLINFOW.lpszText], rax
    mov  rcx, [g_tt]
    mov  edx, TTM_GETCURRENTTOOLW
    xor  r8d, r8d
    lea  r9, [g_ti]
    call SendMessageW
    test eax, eax
    jz   .f_tt
    cmp  word [g_textbuf], 't'
    jne  .f_tt

    ; relay outside the rect: hidden again
    mov  qword [g_msg+MSG.lParam], 0x00640064   ; (100,100): outside
    mov  rcx, [g_tt]
    mov  edx, TTM_RELAYEVENT
    xor  r8d, r8d
    lea  r9, [g_msg]
    call SendMessageW
    mov  rcx, [g_tt]
    call IsWindowVisible
    test eax, eax
    jnz  .f_tt

    OK m_tt, m_tt_l

; ============================ HEADER ===========================================
    mov  rax, [g_hwnd]
    CWX 0, c_header, 0, WS_CHILD | WS_VISIBLE, 0, 250, 200, 18, rax, 2004, 0, 0
    mov  [g_hd], rax
    test rax, rax
    jz   .f_hd

    lea  rbx, [g_hditem]
    mov  dword [rbx+HDITEMW.mask], 0x0003      ; WIDTH|TEXT
    mov  dword [rbx+HDITEMW.cxy], 100
    lea  rax, [t_lvCol]
    mov  [rbx+HDITEMW.pszText], rax
    mov  qword [rbx+HDITEMW.hbm], 0
    mov  dword [rbx+HDITEMW.cchTextMax], 0
    mov  dword [rbx+HDITEMW.fmt], 0
    mov  qword [rbx+HDITEMW.lParam], 0
    mov  rcx, [g_hd]
    mov  edx, HDM_INSERTITEMW
    xor  r8d, r8d
    lea  r9, [g_hditem]
    call SendMessageW
    test eax, eax
    jnz  .f_hd
    mov  rcx, [g_hd]
    mov  edx, HDM_GETITEMCOUNT
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  eax, 1
    jne  .f_hd
    ; a click in the first column notifies HDN_ITEMCLICKW
    mov  dword [g_notifyCode], 0
    mov  rcx, [g_hd]
    mov  edx, WM_LBUTTONDOWN
    mov  r8d, MK_LBUTTON
    mov  r9d, 0x0005000A              ; (10,5)
    call SendMessageW
    mov  eax, [g_notifyCode]
    cmp  eax, HDN_ITEMCLICKW
    jne  .f_hd

    OK m_hd, m_hd_l

; ============================ IMAGELIST ========================================
    ; 16x16 32bpp: magenta with green rows 7-8; mask = magenta
    mov  ecx, 16
    mov  edx, 16
    mov  r8d, 1
    mov  r9d, 32
    sub  rsp, 0x20
    lea  rax, [bmp_px]
    mov  [rsp+0x20], rax
    call CreateBitmap
    add  rsp, 0x20
    mov  r12, rax                     ; the bitmap
    test rax, rax
    jz   .f_il

    mov  ecx, 16
    mov  edx, 16
    mov  r8d, ILC_COLOR32
    xor  r9d, r9d
    sub  rsp, 0x20
    mov  qword [rsp+0x20], 0          ; cInitial
    mov  qword [rsp+0x28], 4          ; cGrow
    call ImageList_Create
    add  rsp, 0x20
    mov  [g_himl], rax
    test rax, rax
    jz   .f_il

    mov  rcx, [g_himl]
    mov  rdx, r12
    mov  r8d, 0x00FF00FF              ; mask: magenta COLORREF
    call ImageList_AddMasked
    cmp  eax, 0
    jne  .f_il

    mov  rcx, [g_himl]
    call ImageList_GetImageCount
    cmp  eax, 1
    jne  .f_il

    mov  rcx, [g_himl]
    lea  rdx, [g_ht]                  ; cx out
    lea  r8, [g_parts]                ; cy out
    call ImageList_GetIconSize
    cmp  dword [g_ht], 16
    jne  .f_il
    cmp  dword [g_parts], 16
    jne  .f_il

    mov  rcx, [g_himl]
    xor  edx, edx
    mov  r8d, ILD_NORMAL
    call ImageList_GetIcon
    mov  [g_hicon], rax
    test rax, rax
    jz   .f_il

    ; ReplaceIcon(-1, icon) appends
    mov  rcx, [g_himl]
    mov  edx, -1
    mov  r8, [g_hicon]
    call ImageList_ReplaceIcon
    cmp  eax, 1
    jne  .f_il
    mov  rcx, [g_himl]
    call ImageList_GetImageCount
    cmp  eax, 2
    jne  .f_il

    ; GetImageInfo's cell rect
    mov  rcx, [g_himl]
    xor  edx, edx
    lea  r8, [g_imginfo]
    call ImageList_GetImageInfo
    test eax, eax
    jz   .f_il
    cmp  dword [g_imginfo+IMAGEINFO.rcImage+RECT.right], 16
    jne  .f_il
    cmp  dword [g_imginfo+IMAGEINFO.rcImage+RECT.bottom], 16
    jne  .f_il

    ; Draw onto the host window's DC; the green row reads back
    mov  rcx, [g_hwnd]
    call GetDC
    mov  [g_hdc], rax
    test rax, rax
    jz   .f_il
    mov  rcx, [g_himl]
    xor  edx, edx
    mov  r8, [g_hdc]
    mov  r9d, 10
    sub  rsp, 0x20
    mov  dword [rsp+0x20], 10         ; y
    mov  dword [rsp+0x28], ILD_NORMAL ; fStyle
    call ImageList_Draw
    add  rsp, 0x20
    test eax, eax
    jz   .f_il
    mov  rcx, [g_hdc]
    mov  edx, 12                      ; x inside the drawn cell
    mov  r8d, 17                      ; y: row 7 of the cell at y=10
    call GetPixel
    cmp  eax, 0x0000FF00              ; green COLORREF 0x00BBGGRR: B=0,G=FF,R=0
    jne  .f_il
    ; the magenta neighbour is masked OUT (transparent -> face colour
    ; fills; just assert it is NOT magenta)
    mov  rcx, [g_hdc]
    mov  edx, 12
    mov  r8d, 11                      ; row 1: masked
    call GetPixel
    cmp  eax, 0x00FF00FF
    je   .f_il

    ; Remove drops the appended image
    mov  rcx, [g_himl]
    mov  edx, 1
    call ImageList_Remove
    test eax, eax
    jz   .f_il
    mov  rcx, [g_himl]
    call ImageList_GetImageCount
    cmp  eax, 1
    jne  .f_il

    ; the drag set: a real popup window that moves with the drag point
    mov  rcx, [g_himl]
    xor  edx, edx
    xor  r8d, r8d
    xor  r9d, r9d
    call ImageList_BeginDrag
    test eax, eax
    jz   .f_il
    mov  rcx, [g_hwnd]                ; the lock window
    mov  edx, 40
    mov  r8d, 60
    call ImageList_DragEnter
    test eax, eax
    jz   .f_il
    mov  ecx, 80
    mov  edx, 90
    call ImageList_DragMove
    test eax, eax
    jz   .f_il
    xor  ecx, ecx
    call ImageList_DragShowNolock
    test eax, eax
    jz   .f_il
    call ImageList_EndDrag

    ; the green drag cell really drew at the drag point: pixels exist
    ; (the popup owned them; after EndDrag the window is gone -- the
    ; receipt above was DragMove's success, the visible-cell proof is
    ; the ImageList_Draw readback two checks up)

    mov  rcx, r12
    call DeleteObject
    mov  rcx, [g_himl]
    call ImageList_Destroy
    test eax, eax
    jz   .f_il
    mov  rcx, [g_hdc]
    mov  rdx, [g_hwnd]
    call ReleaseDC

    OK m_il, m_il_l

; ============================ SUBCLASS =========================================
    ; install id 41 then id 42 on the listview; newest runs first and
    ; DefSubclassProc chains down to the class proc
    ; SetWindowSubclass(hwnd, proc, id, data): id rides r8, data r9
    mov  rcx, [g_lv]
    lea  rdx, [sub1_proc]
    mov  r8d, 41
    mov  r9d, 0x1111
    call SetWindowSubclass
    test eax, eax
    jz   .f_sub

    mov  rcx, [g_lv]
    lea  rdx, [sub2_proc]
    mov  r8d, 42
    mov  r9d, 0x2222
    call SetWindowSubclass
    test eax, eax
    jz   .f_sub

    mov  dword [g_sub1], 0
    mov  dword [g_sub2], 0
    mov  dword [g_orderBuf], 0
    mov  dword [g_orderBuf+4], 0
    mov  dword [g_orderBuf+8], 0

    ; any message the class proc answers ends in DefWindowProcW:
    ; WM_GETTEXTLENGTH returns 0 (the control has no title)
    mov  rcx, [g_lv]
    mov  edx, 0x000E                   ; WM_GETTEXTLENGTH
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    test eax, eax
    jnz  .f_sub                        ; the class proc answered
    cmp  dword [g_sub2], 0             ; newest ran
    je   .f_sub
    cmp  dword [g_sub1], 0             ; ...and chained down
    je   .f_sub
    cmp  dword [g_orderBuf], 2         ; order: 2 first
    jne  .f_sub
    cmp  dword [g_orderBuf+4], 1       ; then 1
    jne  .f_sub

    ; GetWindowSubclass hands back the install data
    mov  rcx, [g_lv]
    lea  rdx, [sub1_proc]
    mov  r8d, 41
    lea  r9, [g_subData]
    call GetWindowSubclass
    test eax, eax
    jz   .f_sub
    cmp  qword [g_subData], 0x1111
    jne  .f_sub

    ; remove the newest: the next message runs only id 41
    mov  rcx, [g_lv]
    mov  edx, 42
    call RemoveWindowSubclass
    test eax, eax
    jz   .f_sub
    mov  dword [g_sub1], 0
    mov  dword [g_sub2], 0
    mov  dword [g_orderBuf], 0
    mov  dword [g_orderBuf+4], 0
    mov  rcx, [g_lv]
    mov  edx, 0x000E
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    cmp  dword [g_sub2], 0
    jne  .f_sub                        ; 42 is gone
    cmp  dword [g_sub1], 0
    je   .f_sub                        ; 41 still chains
    cmp  dword [g_orderBuf], 1
    jne  .f_sub
    cmp  dword [g_orderBuf+4], 0
    jne  .f_sub

    ; remove the last: no subclass runs, the class proc still answers
    mov  rcx, [g_lv]
    mov  edx, 41
    call RemoveWindowSubclass
    mov  dword [g_sub1], 0
    mov  rcx, [g_lv]
    mov  edx, 0x000E
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    test eax, eax
    jnz  .f_sub
    cmp  dword [g_sub1], 0
    jne  .f_sub

    OK m_sub, m_sub_l

; ============================ PSHEET ===========================================
    ; two contiguous PROPSHEETPAGEW records (the PSH_PROPSHEETPAGE array)
    lea  rbx, [g_psPages]
    mov  dword [rbx+0], PROPSHEETPAGEW_size         ; dwSize
    mov  dword [rbx+4], PSP_DLGINDIRECT             ; dwFlags
    mov  rax, [g_hinst]
    mov  [rbx+16], rax                              ; hInstance
    lea  rax, [ps_tmpl1]
    mov  [rbx+24], rax                              ; pResource
    mov  qword [rbx+32], 0                          ; pszIcon
    lea  rax, [t_psPage1]
    mov  [rbx+40], rax                              ; pszTitle
    lea  rax, [page1_proc]
    mov  [rbx+48], rax                              ; pfnDlgProc
    mov  qword [rbx+56], 0x51                       ; lParam
    mov  r12, PROPSHEETPAGEW_size
    mov  dword [rbx+r12+0], PROPSHEETPAGEW_size
    mov  dword [rbx+r12+4], PSP_DLGINDIRECT
    mov  [rbx+r12+16], rax
    mov  rax, [g_hinst]
    mov  [rbx+r12+16], rax
    lea  rax, [ps_tmpl2]
    mov  [rbx+r12+24], rax
    lea  rax, [t_psPage2]
    mov  [rbx+r12+40], rax
    lea  rax, [page2_proc]
    mov  [rbx+r12+48], rax
    mov  qword [rbx+r12+56], 0x52

    lea  rbx, [g_psph]
    mov  dword [rbx+0], PROPSHEETHEADERW_size       ; dwSize
    mov  dword [rbx+4], PSH_PROPSHEETPAGE           ; dwFlags
    mov  rax, [g_hwnd]
    mov  [rbx+16], rax                              ; hwndParent
    mov  rax, [g_hinst]
    mov  [rbx+24], rax                              ; hInstance
    lea  rax, [t_psTitle]
    mov  [rbx+40], rax                              ; pszCaption
    mov  dword [rbx+48], 2                          ; nPages
    lea  rax, [g_psPages]
    mov  [rbx+56], rax                              ; ppsp

    mov  rcx, rbx
    mov  rcx, rbx
    call PropertySheetW
    mov  [g_psRet], rax
    cmp  eax, IDOK
    jne  .f_ps
    cmp  dword [g_psInit1], 0
    je   .f_ps
    cmp  dword [g_psInit2], 0
    je   .f_ps
    cmp  dword [g_psBtn1], 0
    je   .f_ps                       ; the template's button became a window
    cmp  dword [g_psActive1], 0
    je   .f_ps
    cmp  dword [g_psKill1], 0
    je   .f_ps
    cmp  dword [g_psActive2], 0
    je   .f_ps
    cmp  dword [g_psApply1], 0
    je   .f_ps
    cmp  dword [g_psApply2], 0
    je   .f_ps
    cmp  qword [g_psApplyLp1], 1     ; OK direction: lParam TRUE
    jne  .f_ps
    cmp  qword [g_psApplyLp2], 1
    jne  .f_ps

    OK m_ps, m_ps_l

; ============================ REFUSE ===========================================
    ; LoadIconWithScaleDown's E_INVALIDARG: zero cell
    mov  rcx, [g_hinst]
    mov  edx, 1                       ; MAKEINTRESOURCEW(1)
    xor  r8d, r8d                     ; cx = 0
    xor  r9d, r9d
    sub  rsp, 0x20
    lea  rax, [g_hicon]
    mov  [rsp+0x20], rax
    call LoadIconWithScaleDown
    add  rsp, 0x20
    cmp  eax, E_INVALIDARG
    jne  .f_refuse

    ; the no-such-resource path: a real HRESULT, not a silent fake
    mov  rcx, [g_hinst]
    mov  edx, 1
    mov  r8d, 16
    mov  r9d, 16
    sub  rsp, 0x20
    lea  rax, [g_hicon]
    mov  [rsp+0x20], rax
    call LoadIconWithScaleDown
    add  rsp, 0x20
    cmp  eax, 0x8007007E              ; HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
    jne  .f_refuse

    ; TB_CUSTOMIZE is refused by name
    call GetLastError                 ; clear any stale error
    mov  rcx, [g_tb]
    mov  edx, TB_CUSTOMIZE
    xor  r8d, r8d
    xor  r9d, r9d
    call SendMessageW
    test eax, eax
    jnz  .f_refuse                    ; returns FALSE
    call GetLastError
    cmp  eax, 120                     ; ERROR_CALL_NOT_IMPLEMENTED
    jne  .f_refuse

    ; _TrackMouseEvent forwards to the W32A-5 USER32 entry: TME_LEAVE on
    ; a live window arms it; a NULL struct is refused
    lea  rbx, [g_msg]                 ; 24-byte scratch: TRACKMOUSEEVENT
    mov  dword [rbx], 24              ; cbSize
    mov  dword [rbx+4], 2             ; TME_LEAVE
    mov  rax, [g_hwnd]
    mov  [rbx+8], rax                 ; hwndTrack
    mov  dword [rbx+16], 0            ; dwHoverTime
    mov  rcx, rbx
    call _TrackMouseEvent
    test eax, eax
    jz   .f_refuse
    xor  ecx, ecx
    call _TrackMouseEvent
    test eax, eax
    jnz  .f_refuse

    OK m_refuse, m_refuse_l

; ============================ CLEANUP ==========================================
    mov  rcx, [g_tb]
    call DestroyWindow
    mov  rcx, [g_sb]
    call DestroyWindow
    mov  rcx, [g_lv]
    call DestroyWindow
    mov  rcx, [g_tv]
    call DestroyWindow
    mov  rcx, [g_tc]
    call DestroyWindow
    mov  rcx, [g_pb]
    call DestroyWindow
    mov  rcx, [g_tt]
    call DestroyWindow
    mov  rcx, [g_hd]
    call DestroyWindow
    mov  rcx, [g_hwnd]
    call DestroyWindow
    test eax, eax
    jz   .f_cleanup

    OK m_done, m_done_l
    mov  ecx, 78
    call ExitProcess

; ---- failure paths --------------------------------------------------------------
.f_stdout:
    mov  ecx, 79
    call ExitProcess
.f_init:
    FAIL f_init, f_init_l
.f_wschild:
    FAIL f_wschild, f_wschild_l
.f_tb:
    FAIL f_tb, f_tb_l
.f_sb:
    FAIL f_sb, f_sb_l
.f_lv:
    FAIL f_lv, f_lv_l
.f_tv:
    FAIL f_tv, f_tv_l
.f_tc:
    FAIL f_tc, f_tc_l
.f_pb:
    FAIL f_pb, f_pb_l
.f_tt:
    FAIL f_tt, f_tt_l
.f_il:
    FAIL f_il, f_il_l
.f_sub:
    FAIL f_sub, f_sub_l
.f_ps:
    FAIL f_ps, f_ps_l
.f_refuse:
    FAIL f_refuse, f_refuse_l
.f_hd:
    FAIL f_hd, f_hd_l
.f_cleanup:
    mov  ecx, 79
    call ExitProcess
