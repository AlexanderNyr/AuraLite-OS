/* user32.h + gdi32.h — windowing and drawing, over AuraLite's compositor.
 *
 * WIN32_PLAN.md phase W32-5, and decision D5: "Map onto what exists; do not
 * build a second GUI."
 *
 * Every window here is an ag_window_*, every drawing call is an ag_* call, and
 * the message loop is ag_poll_event() with a translation table.  The
 * compositor already does the hard part -- damage, z-order, decoration,
 * theming -- and growing a parallel one would be the largest mistake
 * available in this phase.
 *
 * Where Win32 asks for something AuraLite has no equivalent for, the function
 * returns a documented failure rather than a silent no-op, so a program finds
 * out at the call instead of three frames later.
 *
 * Names, message numbers and structure layouts are the interface being
 * reimplemented, written from published documentation (w32/LICENSING.md).
 */

#ifndef AURALITE_W32_USER32_H
#define AURALITE_W32_USER32_H

#include "w32/w32_abi.h"

/* ---- handles -------------------------------------------------------------
 * HWND is an opaque token from a table, like HANDLE in W32-4 and for the same
 * reason: a program must not be able to forge one by inventing an integer. */
typedef void *W32_HWND;
typedef void *W32_HDC;
typedef void *W32_HINSTANCE;
typedef void *W32_HICON;
typedef void *W32_HCURSOR;
typedef void *W32_HBRUSH;
typedef void *W32_HMENU;

typedef uint64_t W32_WPARAM;
typedef int64_t  W32_LPARAM;
typedef int64_t  W32_LRESULT;
typedef uint32_t W32_UINT;
typedef int32_t  W32_INT;

typedef struct { int32_t x, y; } W32_POINT;
typedef struct { int32_t left, top, right, bottom; } W32_RECT;

/* ---- messages ------------------------------------------------------------ */
#define W32_WM_NULL          0x0000
#define W32_WM_CREATE        0x0001
#define W32_WM_DESTROY       0x0002
#define W32_WM_SIZE          0x0005
#define W32_WM_SETFOCUS      0x0007
#define W32_WM_KILLFOCUS     0x0008
#define W32_WM_PAINT         0x000F
#define W32_WM_CLOSE         0x0010
#define W32_WM_QUIT          0x0012
#define W32_WM_KEYDOWN       0x0100
#define W32_WM_KEYUP         0x0101
#define W32_WM_CHAR          0x0102
#define W32_WM_MOUSEMOVE     0x0200
#define W32_WM_LBUTTONDOWN   0x0201
#define W32_WM_LBUTTONUP     0x0202
#define W32_WM_RBUTTONDOWN   0x0204
#define W32_WM_RBUTTONUP     0x0205
#define W32_WM_MBUTTONDOWN   0x0207
#define W32_WM_MBUTTONUP     0x0208
#define W32_WM_MOUSEWHEEL    0x020A
#define W32_WM_TIMER         0x0113
#define W32_WM_USER          0x0400

/* ---- window styles (only those that map to an AG_WIN_* flag) ------------- */
#define W32_WS_OVERLAPPED    0x00000000u
#define W32_WS_CAPTION       0x00C00000u
#define W32_WS_SYSMENU       0x00080000u
#define W32_WS_THICKFRAME    0x00040000u
#define W32_WS_MINIMIZEBOX   0x00020000u
#define W32_WS_MAXIMIZEBOX   0x00010000u
#define W32_WS_POPUP         0x80000000u
#define W32_WS_VISIBLE       0x10000000u
#define W32_WS_OVERLAPPEDWINDOW \
    (W32_WS_OVERLAPPED | W32_WS_CAPTION | W32_WS_SYSMENU | \
     W32_WS_THICKFRAME | W32_WS_MINIMIZEBOX | W32_WS_MAXIMIZEBOX)

/* CW_USEDEFAULT asks the system to choose; AuraLite picks a cascade offset. */
#define W32_CW_USEDEFAULT ((int32_t)0x80000000)

/* ShowWindow commands. */
#define W32_SW_HIDE     0
#define W32_SW_SHOWNORMAL 1
#define W32_SW_SHOW     5

/* ---- MSG and WNDCLASS ---------------------------------------------------- */
typedef struct {
    W32_HWND   hwnd;
    W32_UINT   message;
    W32_WPARAM wParam;
    W32_LPARAM lParam;
    W32_DWORD  time;
    W32_POINT  pt;
} W32_MSG;

/* The callback direction of the ABI: the personality calls INTO the PE image.
 * W32-4's test covered calls the other way; this one is what W32-5's gate
 * adds. */
typedef W32_LRESULT (W32ABI *W32_WNDPROC)(W32_HWND, W32_UINT,
                                          W32_WPARAM, W32_LPARAM);

typedef struct {
    W32_UINT      cbSize;
    W32_UINT      style;
    W32_WNDPROC   lpfnWndProc;
    W32_INT       cbClsExtra;
    W32_INT       cbWndExtra;
    W32_HINSTANCE hInstance;
    W32_HICON     hIcon;
    W32_HCURSOR   hCursor;
    W32_HBRUSH    hbrBackground;
    const char   *lpszMenuName;
    const char   *lpszClassName;
    W32_HICON     hIconSm;
} W32_WNDCLASSEXA;

typedef struct {
    W32_HDC   hdc;
    W32_BOOL  fErase;
    W32_RECT  rcPaint;
    W32_BOOL  fRestore;
    W32_BOOL  fIncUpdate;
    W32_BYTE  rgbReserved[32];
} W32_PAINTSTRUCT;

/* ---- USER32 -------------------------------------------------------------- */
W32ABI W32_WORD    RegisterClassExA(const W32_WNDCLASSEXA *cls);
W32ABI W32_HWND    CreateWindowExA(W32_DWORD exstyle, const char *cls,
                                   const char *title, W32_DWORD style,
                                   int32_t x, int32_t y, int32_t w, int32_t h,
                                   W32_HWND parent, W32_HMENU menu,
                                   W32_HINSTANCE inst, void *param);
W32ABI W32_BOOL    ShowWindow(W32_HWND hwnd, int32_t cmd);
W32ABI W32_BOOL    UpdateWindow(W32_HWND hwnd);
W32ABI W32_BOOL    DestroyWindow(W32_HWND hwnd);

/* W32A-4: live windows, for the unwinder's GUI-or-console decision. */
int w32_user_window_count(void);
W32ABI W32_BOOL    GetMessageA(W32_MSG *msg, W32_HWND hwnd,
                               W32_UINT min, W32_UINT max);
W32ABI W32_BOOL    PeekMessageA(W32_MSG *msg, W32_HWND hwnd,
                                W32_UINT min, W32_UINT max, W32_UINT remove);
W32ABI W32_BOOL    TranslateMessage(const W32_MSG *msg);
W32ABI W32_LRESULT DispatchMessageA(const W32_MSG *msg);
W32ABI W32_LRESULT DefWindowProcA(W32_HWND hwnd, W32_UINT msg,
                                  W32_WPARAM wp, W32_LPARAM lp);
W32ABI void        PostQuitMessage(int32_t code);
W32ABI W32_BOOL    InvalidateRect(W32_HWND hwnd, const W32_RECT *r,
                                  W32_BOOL erase);
W32ABI W32_BOOL    GetClientRect(W32_HWND hwnd, W32_RECT *r);
W32ABI int32_t     MessageBoxA(W32_HWND owner, const char *text,
                               const char *caption, W32_UINT type);

/* ---- GDI32 --------------------------------------------------------------- */
W32ABI W32_HDC   BeginPaint(W32_HWND hwnd, W32_PAINTSTRUCT *ps);
W32ABI W32_BOOL  EndPaint(W32_HWND hwnd, const W32_PAINTSTRUCT *ps);
W32ABI int32_t   FillRect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush);
W32ABI W32_BOOL  TextOutA(W32_HDC hdc, int32_t x, int32_t y,
                          const char *s, int32_t len);
W32ABI W32_BOOL  MoveToEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old);
W32ABI W32_BOOL  LineTo(W32_HDC hdc, int32_t x, int32_t y);
W32ABI W32_DWORD SetPixel(W32_HDC hdc, int32_t x, int32_t y, W32_DWORD color);
W32ABI W32_DWORD SetTextColor(W32_HDC hdc, W32_DWORD color);

/* CreateSolidBrush returns a brush whose "handle" encodes the colour, so no
 * allocation and no object table are needed for the one GDI object this
 * phase uses.  DeleteObject accepts it and does nothing. */
W32ABI W32_HBRUSH CreateSolidBrush(W32_DWORD color);
W32ABI W32_BOOL   DeleteObject(void *obj);

/* Win32 packs colours as 0x00BBGGRR; AuraLite uses 0x00RRGGBB.  Exposed so the
 * test can assert the swap rather than infer it from pixels. */
#define W32_RGB(r,g,b) ((W32_DWORD)((uint8_t)(r) | ((uint8_t)(g) << 8) | \
                                    ((uint8_t)(b) << 16)))
W32_DWORD w32_colorref_to_ag(W32_DWORD cr);

/* Reset all window/class state; called by the CRT startup path. */
void w32_user32_init(void);

/* =======================================================================
 * W32APP_PLAN.md phase W32A-5 — USER32 breadth I: windows and messages.
 *
 * Decision D6 applies from here on: the W variant is the real one and the A
 * variant forwards to it.  A name is therefore declared once, in W, and the
 * A form exists only where an import table can ask for it.
 *
 * What lives in w32/src/user32_win.c: the class registry, the window table,
 * per-thread message queues (SendMessage across threads blocks on a W32A-3
 * event), subclass chains, window properties, placement/geometry, paint and
 * update regions, scroll state, system metrics and colours taken from the
 * compositor theme, and the single monitor.  What it deliberately does not
 * do: keep its own Z-order, its own pixel surface, or its own input queue --
 * those are the compositor's, and asking it is the whole point of D5.
 * ======================================================================= */

/* ---- messages ---------------------------------------------------------- */
#define W32_WM_MOVE           0x0003
#define W32_WM_ACTIVATE       0x0006
#define W32_WM_ENABLE         0x000A
#define W32_WM_SETREDRAW      0x000B
#define W32_WM_SETTEXT        0x000C
#define W32_WM_GETTEXT        0x000D
#define W32_WM_GETTEXTLENGTH  0x000E
#define W32_WM_ERASEBKGND     0x0014
#define W32_WM_SHOWWINDOW     0x0018
#define W32_WM_WINDOWPOSCHANGING 0x0046
#define W32_WM_WINDOWPOSCHANGED  0x0047
#define W32_WM_NCCREATE       0x0081
#define W32_WM_NCDESTROY      0x0082
#define W32_WM_NCCALCSIZE     0x0083
#define W32_WM_NCHITTEST      0x0084
#define W32_WM_NCPAINT        0x0085
#define W32_WM_NCACTIVATE     0x0086
#define W32_WM_GETMINMAXINFO  0x0024
#define W32_WM_SYSCOMMAND     0x0112
#define W32_WM_HSCROLL        0x0114
#define W32_WM_VSCROLL        0x0115
#define W32_WM_MOUSELEAVE     0x02A3
#define W32_WM_COMMAND        0x0111
#define W32_WM_NOTIFY         0x004E
#define W32_WM_APP            0x8000
#define W32_WM_SETCURSOR      0x0020
#define W32_WM_MOUSEACTIVATE  0x0021
#define W32_WM_CTLCOLORMSGBOX 0x0132
#define W32_WM_CTLCOLOREDIT   0x0133
#define W32_WM_CTLCOLORLISTBOX 0x0134
#define W32_WM_CTLCOLORBTN    0x0135
#define W32_WM_CTLCOLORDLG    0x0136
#define W32_WM_CTLCOLORSCROLLBAR 0x0137
#define W32_WM_CTLCOLORSTATIC 0x0138
#define W32_WM_QUERYENDSESSION 0x0011
#define W32_WM_ENDSESSION     0x0016
#define W32_WM_CANCELMODE     0x001F
#define W32_WM_CONTEXTMENU    0x007B
#define W32_WM_KEYDOWN_REPEAT 0x40000000  /* lParam bit 30, previous-down */

/* WM_KEYDOWN lParam bits the personality fills in (see translate_event). */
#define W32_LPARAM_REPEAT_BIT   (1 << 30)
#define W32_LPARAM_RELEASE_BIT  (1 << 31)

/* ---- window styles ----------------------------------------------------- */
#define W32_WS_BORDER        0x00800000u
#define W32_WS_DLGFRAME      0x00400000u
#define W32_WS_VSCROLL       0x00200000u
#define W32_WS_HSCROLL       0x00100000u
#define W32_WS_CHILD         0x40000000u
#define W32_WS_POPUPWINDOW   (W32_WS_POPUP | W32_WS_BORDER | W32_WS_SYSMENU)
#define W32_WS_TABSTOP       0x00010000u
#define W32_WS_CLIPCHILDREN  0x02000000u
#define W32_WS_CLIPSIBLINGS  0x04000000u
#define W32_WS_DISABLED      0x08000000u
#define W32_WS_GROUP         0x00020000u

#define W32_WS_EX_TOPMOST     0x00000008u
#define W32_WS_EX_TRANSPARENT 0x00000020u
#define W32_WS_EX_TOOLWINDOW  0x00000080u
#define W32_WS_EX_LAYERED     0x00080000u
#define W32_WS_EX_APPWINDOW   0x00040000u
#define W32_WS_EX_CLIENTEDGE  0x00000200u
#define W32_WS_EX_DLGMODALFRAME 0x00000001u

/* ---- ShowWindow -------------------------------------------------------- */
#define W32_SW_MINIMIZE      6
#define W32_SW_SHOWMINIMIZED 2
#define W32_SW_MAXIMIZE      3
#define W32_SW_SHOWMAXIMIZED 3
#define W32_SW_SHOWNOACTIVATE 4
#define W32_SW_MINIMIZE_     6
#define W32_SW_RESTORE       9
#define W32_SW_SHOWDEFAULT   10
#define W32_SW_FORCEMINIMIZE 11

/* ---- SetWindowPos ------------------------------------------------------ */
#define W32_SWP_NOSIZE        0x0001
#define W32_SWP_NOMOVE        0x0002
#define W32_SWP_NOZORDER      0x0004
#define W32_SWP_NOREDRAW      0x0008
#define W32_SWP_NOACTIVATE    0x0010
#define W32_SWP_FRAMECHANGED  0x0020
#define W32_SWP_SHOWWINDOW    0x0040
#define W32_SWP_HIDEWINDOW    0x0080
#define W32_SWP_NOCOPYBITS    0x0100
#define W32_SWP_NOOWNERZORDER 0x0200
#define W32_SWP_NOSENDCHANGING 0x0400

#define W32_HWND_TOP      ((W32_HWND)0)
#define W32_HWND_BOTTOM   ((W32_HWND)1)
#define W32_HWND_TOPMOST  ((W32_HWND)-1)
#define W32_HWND_NOTOPMOST ((W32_HWND)-2)

/* ---- Get/SetWindowLong ------------------------------------------------- */
#define W32_GWL_WNDPROC   (-4)
#define W32_GWL_HINSTANCE (-6)
#define W32_GWL_HWNDPARENT (-8)
#define W32_GWL_ID        (-12)
#define W32_GWL_STYLE     (-16)
#define W32_GWL_EXSTYLE   (-20)
#define W32_GWL_USERDATA  (-21)

/* ---- GetWindow -------------------------------------------------------- */
#define W32_GW_HWNDFIRST 0
#define W32_GW_HWNDLAST  1
#define W32_GW_HWNDNEXT  2
#define W32_GW_HWNDPREV  3
#define W32_GW_OWNER     4
#define W32_GW_CHILD     5
#define W32_GW_ENABLEDPOPUP 6

/* ---- GetAncestor ------------------------------------------------------ */
#define W32_GA_PARENT    1
#define W32_GA_ROOT      2
#define W32_GA_ROOTOWNER 3

/* ---- SetCapture / GetKeyState style modifiers ------------------------- */
#define W32_VK_SHIFT     0x10
#define W32_VK_CONTROL   0x11
#define W32_VK_MENU      0x12
#define W32_VK_LBUTTON   0x01
#define W32_VK_RBUTTON   0x02
#define W32_VK_MBUTTON   0x04
#define W32_VK_ESCAPE    0x1B
#define W32_VK_RETURN    0x0D
#define W32_VK_SPACE     0x20
#define W32_VK_TAB       0x09
#define W32_VK_BACK      0x08

/* ---- System metrics (the ledger's set) -------------------------------- */
#define W32_SM_CXSCREEN        0
#define W32_SM_CYSCREEN        1
#define W32_SM_CXVSCROLL       2
#define W32_SM_CYHSCROLL       3
#define W32_SM_CYCAPTION       4
#define W32_SM_CXBORDER        5
#define W32_SM_CYBORDER        6
#define W32_SM_CXDLGFRAME      7
#define W32_SM_CYDLGFRAME      8
#define W32_SM_CYVTHUMB        9
#define W32_SM_CXHTHUMB        10
#define W32_SM_CXICON          11
#define W32_SM_CYICON          12
#define W32_SM_CXCURSOR        13
#define W32_SM_CYCURSOR        14
#define W32_SM_CYMENU          15
#define W32_SM_CXFULLSCREEN    16
#define W32_SM_CYFULLSCREEN    17
#define W32_SM_CYKANJIWINDOW   18
#define W32_SM_MOUSEPRESENT    19
#define W32_SM_CYVSCROLL       20
#define W32_SM_CXHSCROLL       21
#define W32_SM_DEBUG           22
#define W32_SM_SWAPBUTTON      23
#define W32_SM_CXMIN           28
#define W32_SM_CYMIN           29
#define W32_SM_CXSIZE          30
#define W32_SM_CYSIZE          31
#define W32_SM_CXFRAME         32
#define W32_SM_CYFRAME         33
#define W32_SM_CXMINTRACK      34
#define W32_SM_CYMINTRACK      35
#define W32_SM_CMONITORS       80
#define W32_SM_SAMEDISPLAYFORMAT 81

/* ---- System colours (0x00RRGGBB after the COLORREF swap) -------------- */
#define W32_COLOR_SCROLLBAR       0
#define W32_COLOR_BACKGROUND      1
#define W32_COLOR_ACTIVECAPTION   2
#define W32_COLOR_INACTIVECAPTION 3
#define W32_COLOR_MENU            4
#define W32_COLOR_WINDOW          5
#define W32_COLOR_WINDOWFRAME     6
#define W32_COLOR_MENUTEXT        7
#define W32_COLOR_WINDOWTEXT      8
#define W32_COLOR_CAPTIONTEXT     9
#define W32_COLOR_ACTIVEBORDER    10
#define W32_COLOR_INACTIVEBORDER  11
#define W32_COLOR_APPWORKSPACE    12
#define W32_COLOR_HIGHLIGHT       13
#define W32_COLOR_HIGHLIGHTTEXT   14
#define W32_COLOR_BTNFACE         15
#define W32_COLOR_BTNSHADOW       16
#define W32_COLOR_GRAYTEXT        17
#define W32_COLOR_BTNTEXT         18
#define W32_COLOR_INACTIVECAPTIONTEXT 19
#define W32_COLOR_BTNHIGHLIGHT    20
#define W32_COLOR_3DDKSHADOW      21
#define W32_COLOR_3DLIGHT         22
#define W32_COLOR_INFOTEXT        23
#define W32_COLOR_INFOBK          24
#define W32_COLOR_DESKTOP         25

/* ---- SystemParametersInfo -------------------------------------------- */
#define W32_SPI_GETBEEP              1
#define W32_SPI_GETMOUSE             3
#define W32_SPI_GETBORDER           5
#define W32_SPI_GETKEYBOARDSPEED    10
#define W32_SPI_GETMOUSESPEED       112
#define W32_SPI_GETKEYBOARDDELAY    22
#define W32_SPI_GETICONTITLEWRAP    25
#define W32_SPI_GETWORKAREA         48
#define W32_SPI_GETSCREENSAVEACTIVE 16
#define W32_SPI_GETSCREENSAVETIMEOUT 14
#define W32_SPI_GETMENUSHOWDELAY    106
#define W32_SPI_GETWHEELSCROLLLINES 104

/* ---- Monitors --------------------------------------------------------- */
#define W32_MONITOR_DEFAULTTONULL       0
#define W32_MONITOR_DEFAULTTOPRIMARY    1
#define W32_MONITOR_DEFAULTTONEAREST    2

/* ---- Scroll ----------------------------------------------------------- */
#define W32_SB_HORZ 0
#define W32_SB_VERT 1
#define W32_SB_CTL  2
#define W32_SB_BOTH 3
#define W32_SB_LINEUP 0
#define W32_SB_LINEDOWN 1
#define W32_SB_PAGEUP 2
#define W32_SB_PAGEDOWN 3
#define W32_SB_THUMBPOSITION 4
#define W32_SB_ENDSCROLL 8
#define W32_SIF_ALL 0x017F
#define W32_SIF_POS 0x0004
#define W32_SIF_RANGE 0x0001
#define W32_SIF_PAGE 0x0002
#define W32_RDW_INVALIDATE 0x0001
#define W32_RDW_ERASE 0x0004
#define W32_RDW_VALIDATE 0x0002
#define W32_RDW_UPDATENOW 0x0100
#define W32_RDW_ALLCHILDREN 0x0080
#define W32_RDW_FRAME 0x0400
#define W32_RDW_NOINTERNALPAINT 0x0010

/* ---- structs ---------------------------------------------------------- */
typedef void *W32_HMONITOR;

typedef struct {
    uint32_t cbSize;
    uint32_t style;
    W32_WNDPROC lpfnWndProc;
    int32_t  cbClsExtra;
    int32_t  cbWndExtra;
    W32_HINSTANCE hInstance;
    W32_HICON hIcon;
    W32_HCURSOR hCursor;
    W32_HBRUSH hbrBackground;
    const uint16_t *lpszMenuName;
    const uint16_t *lpszClassName;
    W32_HICON hIconSm;
} W32_WNDCLASSEXW;

typedef struct {
    uint32_t length;
    uint32_t flags;
    uint32_t showCmd;
    W32_POINT ptMinPosition;
    W32_POINT ptMaxPosition;
    W32_RECT  rcNormalPosition;
} W32_WINDOWPLACEMENT;

typedef struct {
    W32_POINT ptReserved;
    W32_POINT ptMaxSize;
    W32_POINT ptMaxPosition;
    W32_POINT ptMinTrackSize;
    W32_POINT ptMaxTrackSize;
} W32_MINMAXINFO;

typedef struct {
    uint32_t cbSize;
    uint32_t fMask;
    int32_t  nMin;
    int32_t  nMax;
    uint32_t nPage;
    int32_t  nPos;
    int32_t  nTrackPos;
} W32_SCROLLINFO;

typedef struct {
    W32_HWND hwndFrom;
    uintptr_t idFrom;
    uint32_t code;
} W32_NMHDR;

typedef struct {
    W32_RECT rcMonitor;
    W32_RECT rcWork;
    uint32_t dwFlags;
} W32_MONITORINFO;

typedef struct {
    W32_MONITORINFO mi;
    uint16_t szDevice[32];
} W32_MONITORINFOEXW;


/* ---- the W API -------------------------------------------------------- */
W32ABI W32_WORD  RegisterClassA(const W32_WNDCLASSEXA *cls);
W32ABI W32_WORD  RegisterClassW(const W32_WNDCLASSEXW *cls);
W32ABI W32_WORD  RegisterClassExW(const W32_WNDCLASSEXW *cls);
W32ABI W32_BOOL  UnregisterClassW(const uint16_t *name, W32_HINSTANCE inst);
W32ABI W32_BOOL  GetClassInfoW(W32_HINSTANCE inst, const uint16_t *name,
                               W32_WNDCLASSEXW *out);
W32ABI int32_t   GetClassNameW(W32_HWND hwnd, uint16_t *buf, int32_t max);
W32ABI int32_t   GetClassNameA(W32_HWND hwnd, char *buf, int32_t max);

W32ABI W32_HWND  CreateWindowExW(W32_DWORD exstyle, const uint16_t *cls,
                                 const uint16_t *title, W32_DWORD style,
                                 int32_t x, int32_t y, int32_t w, int32_t h,
                                 W32_HWND parent, W32_HMENU menu,
                                 W32_HINSTANCE inst, void *param);
W32ABI W32_BOOL  IsWindow(W32_HWND hwnd);
W32ABI W32_HWND  GetDesktopWindow(void);
W32ABI W32_HWND  GetShellWindow(void);
W32ABI W32_DWORD GetWindowThreadProcessId(W32_HWND hwnd, W32_DWORD *pid);

W32ABI W32_LRESULT SendMessageW(W32_HWND hwnd, W32_UINT msg,
                                W32_WPARAM wp, W32_LPARAM lp);
W32ABI W32_LRESULT SendMessageA(W32_HWND hwnd, W32_UINT msg,
                                W32_WPARAM wp, W32_LPARAM lp);
W32ABI W32_BOOL    PostMessageW(W32_HWND hwnd, W32_UINT msg,
                                W32_WPARAM wp, W32_LPARAM lp);
W32ABI W32_BOOL    PostMessageA(W32_HWND hwnd, W32_UINT msg,
                                W32_WPARAM wp, W32_LPARAM lp);
W32ABI W32_BOOL    GetMessageW(W32_MSG *msg, W32_HWND hwnd,
                               W32_UINT min, W32_UINT max);
W32ABI W32_BOOL    PeekMessageW(W32_MSG *msg, W32_HWND hwnd,
                                W32_UINT min, W32_UINT max, W32_UINT remove);
W32ABI W32_LRESULT DispatchMessageW(const W32_MSG *msg);
W32ABI W32_LRESULT DefWindowProcW(W32_HWND hwnd, W32_UINT msg,
                                  W32_WPARAM wp, W32_LPARAM lp);
W32ABI W32_LRESULT CallWindowProcW(W32_WNDPROC prev, W32_HWND hwnd,
                                   W32_UINT msg, W32_WPARAM wp, W32_LPARAM lp);
W32ABI W32_UINT    RegisterWindowMessageW(const uint16_t *s);
W32ABI W32_UINT    RegisterWindowMessageA(const char *s);
W32ABI W32_DWORD   GetMessageTime(void);
W32ABI W32_DWORD   GetMessagePos(void);
W32ABI W32_DWORD   GetQueueStatus(W32_UINT flags);
W32ABI W32_BOOL    InSendMessage(void);
W32ABI W32_BOOL    ReplyMessage(W32_LRESULT r);
W32ABI W32_LRESULT SendDlgItemMessageW(W32_HWND parent, int32_t id,
                                       W32_UINT msg, W32_WPARAM wp,
                                       W32_LPARAM lp);
W32ABI W32_LRESULT SendDlgItemMessageA(W32_HWND parent, int32_t id,
                                       W32_UINT msg, W32_WPARAM wp,
                                       W32_LPARAM lp);
W32ABI W32_DWORD   MsgWaitForMultipleObjects(W32_DWORD count,
                                             const W32_HANDLE *handles,
                                             W32_BOOL wait_all,
                                             W32_DWORD ms, W32_DWORD mask);

W32ABI intptr_t SetWindowLongPtrW(W32_HWND hwnd, int32_t idx, intptr_t v);
W32ABI intptr_t GetWindowLongPtrW(W32_HWND hwnd, int32_t idx);
W32ABI intptr_t SetWindowLongPtrA(W32_HWND hwnd, int32_t idx, intptr_t v);
W32ABI intptr_t GetWindowLongPtrA(W32_HWND hwnd, int32_t idx);
W32ABI int32_t  GetWindowLongW(W32_HWND hwnd, int32_t idx);
W32ABI int32_t  SetWindowLongW(W32_HWND hwnd, int32_t idx, int32_t v);
W32ABI int32_t  GetDlgCtrlID(W32_HWND hwnd);
W32ABI W32_BOOL SetWindowTextW(W32_HWND hwnd, const uint16_t *s);
W32ABI W32_BOOL SetWindowTextA(W32_HWND hwnd, const char *s);
W32ABI int32_t  GetWindowTextW(W32_HWND hwnd, uint16_t *buf, int32_t max);
W32ABI int32_t  GetWindowTextA(W32_HWND hwnd, char *buf, int32_t max);
W32ABI int32_t  GetWindowTextLengthW(W32_HWND hwnd);
W32ABI int32_t  GetWindowTextLengthA(W32_HWND hwnd);
W32ABI W32_BOOL SetPropW(W32_HWND hwnd, const uint16_t *key, W32_HANDLE val);
W32ABI W32_HANDLE GetPropW(W32_HWND hwnd, const uint16_t *key);
W32ABI W32_HANDLE RemovePropW(W32_HWND hwnd, const uint16_t *key);

W32ABI W32_BOOL GetWindowRect(W32_HWND hwnd, W32_RECT *r);
W32ABI W32_BOOL GetWindowPlacement(W32_HWND hwnd, W32_WINDOWPLACEMENT *p);
W32ABI W32_BOOL SetWindowPlacement(W32_HWND hwnd,
                                   const W32_WINDOWPLACEMENT *p);
W32ABI W32_BOOL IsWindowVisible(W32_HWND hwnd);
W32ABI W32_BOOL IsIconic(W32_HWND hwnd);
W32ABI W32_BOOL IsZoomed(W32_HWND hwnd);
W32ABI W32_BOOL BringWindowToTop(W32_HWND hwnd);
W32ABI W32_BOOL SetWindowPos(W32_HWND hwnd, W32_HWND after, int32_t x,
                             int32_t y, int32_t cx, int32_t cy,
                             W32_UINT flags);
W32ABI W32_BOOL MoveWindow(W32_HWND hwnd, int32_t x, int32_t y,
                           int32_t w, int32_t h, W32_BOOL repaint);
W32ABI W32_BOOL AdjustWindowRectEx(W32_RECT *r, W32_DWORD style,
                                   W32_BOOL menu, W32_DWORD exstyle);
W32ABI W32_BOOL ClientToScreen(W32_HWND hwnd, W32_POINT *pt);
W32ABI W32_BOOL ScreenToClient(W32_HWND hwnd, W32_POINT *pt);
W32ABI int32_t  MapWindowPoints(W32_HWND from, W32_HWND to, W32_POINT *pts,
                                W32_UINT count);
W32ABI W32_HWND WindowFromPoint(W32_POINT pt);
W32ABI W32_HWND ChildWindowFromPointEx(W32_HWND parent, W32_POINT pt,
                                       W32_UINT flags);
W32ABI W32_HWND GetAncestor(W32_HWND hwnd, W32_UINT flags);
W32ABI W32_HWND GetParent(W32_HWND hwnd);
W32ABI W32_HWND SetParent(W32_HWND child, W32_HWND parent);
W32ABI W32_HWND GetWindow(W32_HWND hwnd, W32_UINT cmd);
W32ABI W32_BOOL IsChild(W32_HWND parent, W32_HWND child);
W32ABI W32_BOOL EnumChildWindows(W32_HWND parent,
                                 W32_BOOL (W32ABI *cb)(W32_HWND, W32_LPARAM),
                                 W32_LPARAM lp);
W32ABI W32_BOOL EnumThreadWindows(W32_DWORD tid,
                                  W32_BOOL (W32ABI *cb)(W32_HWND, W32_LPARAM),
                                  W32_LPARAM lp);
W32ABI W32_HWND FindWindowW(const uint16_t *cls, const uint16_t *title);
W32ABI W32_HWND FindWindowA(const char *cls, const char *title);
W32ABI W32_HWND FindWindowExW(W32_HWND parent, W32_HWND after,
                              const uint16_t *cls, const uint16_t *title);
W32ABI W32_HWND GetFocus(void);
W32ABI W32_HWND SetFocus(W32_HWND hwnd);
W32ABI W32_HWND GetActiveWindow(void);
W32ABI W32_HWND SetActiveWindow(W32_HWND hwnd);
W32ABI W32_HWND GetForegroundWindow(void);
W32ABI W32_BOOL SetForegroundWindow(W32_HWND hwnd);
W32ABI W32_HWND GetLastActivePopup(W32_HWND hwnd);
W32ABI W32_BOOL FlashWindow(W32_HWND hwnd, W32_BOOL invert);
W32ABI W32_BOOL FlashWindowEx(void *info);
W32ABI W32_HWND GetCapture(void);
W32ABI W32_HWND SetCapture(W32_HWND hwnd);
W32ABI W32_BOOL ReleaseCapture(void);
W32ABI W32_BOOL SetLayeredWindowAttributes(W32_HWND hwnd, W32_DWORD key,
                                           uint8_t alpha, W32_DWORD flags);
W32ABI W32_BOOL EnableWindow(W32_HWND hwnd, W32_BOOL enable);
W32ABI W32_BOOL IsWindowEnabled(W32_HWND hwnd);

W32ABI W32_BOOL ValidateRect(W32_HWND hwnd, const W32_RECT *r);
W32ABI int32_t  GetUpdateRgn(W32_HWND hwnd, void *rgn, W32_BOOL erase);
W32ABI W32_BOOL RedrawWindow(W32_HWND hwnd, const W32_RECT *r, void *rgn,
                             W32_UINT flags);
W32ABI W32_BOOL LockWindowUpdate(W32_HWND hwnd);
W32ABI W32_BOOL GetScrollInfo(W32_HWND hwnd, int32_t bar, W32_SCROLLINFO *si);
W32ABI int32_t  SetScrollInfo(W32_HWND hwnd, int32_t bar,
                              const W32_SCROLLINFO *si, W32_BOOL redraw);
W32ABI int32_t  GetScrollPos(W32_HWND hwnd, int32_t bar);
W32ABI int32_t  SetScrollPos(W32_HWND hwnd, int32_t bar, int32_t pos,
                             W32_BOOL redraw);
W32ABI W32_BOOL GetScrollRange(W32_HWND hwnd, int32_t bar, int32_t *min,
                               int32_t *max);
W32ABI W32_BOOL SetScrollRange(W32_HWND hwnd, int32_t bar, int32_t min,
                               int32_t max, W32_BOOL redraw);
W32ABI W32_BOOL ShowScrollBar(W32_HWND hwnd, int32_t bar, W32_BOOL show);
W32ABI int32_t  ScrollWindow(W32_HWND hwnd, int32_t dx, int32_t dy,
                             const W32_RECT *scroll, const W32_RECT *clip);
W32ABI W32_HDC  GetDC(W32_HWND hwnd);
W32ABI W32_HDC  GetDCEx(W32_HWND hwnd, void *rgn, W32_DWORD flags);
W32ABI W32_HDC  GetWindowDC(W32_HWND hwnd);
W32ABI int32_t  ReleaseDC(W32_HWND hwnd, W32_HDC dc);

W32ABI W32_BOOL EqualRect(const W32_RECT *a, const W32_RECT *b);
W32ABI W32_BOOL InflateRect(W32_RECT *r, int32_t dx, int32_t dy);
W32ABI W32_BOOL IntersectRect(W32_RECT *dst, const W32_RECT *a,
                              const W32_RECT *b);
W32ABI W32_BOOL OffsetRect(W32_RECT *r, int32_t dx, int32_t dy);
W32ABI W32_BOOL PtInRect(const W32_RECT *r, W32_POINT pt);
W32ABI W32_BOOL SetRectEmpty(W32_RECT *r);
W32ABI W32_BOOL IsRectEmpty(const W32_RECT *r);

W32ABI int32_t  GetSystemMetrics(int32_t index);
W32ABI W32_DWORD GetSysColor(int32_t index);
W32ABI W32_HBRUSH GetSysColorBrush(int32_t index);
W32ABI W32_BOOL SystemParametersInfoW(W32_UINT action, W32_UINT param,
                                      void *data, W32_UINT winini);
W32ABI W32_BOOL SystemParametersInfoA(W32_UINT action, W32_UINT param,
                                      void *data, W32_UINT winini);
W32ABI W32_DWORD GetDoubleClickTime(void);
W32ABI W32_DWORD GetCaretBlinkTime(void);
W32ABI int32_t  GetKeyboardType(int32_t type);

W32ABI W32_BOOL EnumDisplayMonitors(W32_HDC dc, const W32_RECT *clip,
                                    W32_BOOL (W32ABI *cb)(W32_HMONITOR,
                                                          W32_HDC, W32_RECT *,
                                                          W32_LPARAM),
                                    W32_LPARAM lp);
W32ABI W32_BOOL GetMonitorInfoW(W32_HMONITOR mon, W32_MONITORINFOEXW *mi);
W32ABI W32_BOOL GetMonitorInfoA(W32_HMONITOR mon, void *mi);
W32ABI W32_HMONITOR MonitorFromWindow(W32_HWND hwnd, W32_DWORD flags);
W32ABI W32_HMONITOR MonitorFromRect(const W32_RECT *r, W32_DWORD flags);
W32ABI W32_HMONITOR MonitorFromPoint(W32_POINT pt, W32_DWORD flags);

W32ABI int16_t  GetKeyState(int32_t vk);
W32ABI W32_BOOL GetKeyboardState(uint8_t *keys);
W32ABI W32_BOOL SetKeyboardState(uint8_t *keys);
W32ABI W32_DWORD GetKeyboardLayout(W32_DWORD tid);
W32ABI W32_UINT MapVirtualKeyW(W32_UINT code, W32_UINT type);
W32ABI int32_t  ToAscii(W32_UINT vk, W32_UINT scan, const uint8_t *state,
                        uint16_t *out, W32_UINT flags);
W32ABI int32_t  ToAsciiEx(W32_UINT vk, W32_UINT scan, const uint8_t *state,
                          uint16_t *out, W32_UINT flags, W32_DWORD layout);
W32ABI W32_BOOL GetCursorPos(W32_POINT *pt);
W32ABI W32_BOOL SetCursorPos(int32_t x, int32_t y);
W32ABI void     mouse_event(W32_DWORD flags, W32_DWORD dx, W32_DWORD dy,
                            W32_DWORD data, uintptr_t extra);
W32ABI W32_BOOL TrackMouseEvent(void *tev);
W32ABI W32_LRESULT MessageBoxW(W32_HWND owner, const uint16_t *text,
                               const uint16_t *caption, W32_UINT type);

/* Character classification/conversion: Windows exports this family from
 * USER32, and W32A-2 wrote the code (kernel32_loc.c).  W32A-5 therefore
 * binds those existing implementations under the user32.dll names in the
 * export table instead of writing a second copy; their prototypes live in
 * w32/kernel32.h (CharUpperW, CharLowerW, IsCharAlphaW,
 * IsCharAlphaNumericW, IsCharUpperW, IsCharLowerW). */

#endif /* AURALITE_W32_USER32_H */
