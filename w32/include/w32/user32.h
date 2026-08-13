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

#endif /* AURALITE_W32_USER32_H */
