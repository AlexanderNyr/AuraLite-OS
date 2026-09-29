/* w32/examples/listview-app/listview.c — a COMCTL32 list-view program.
 *
 * W32APP_PLAN.md phase W32A-18, the ladder's common-controls rung.
 * Apache-2.0; public-domain mingw-w64 <windows.h>/<commctrl.h> declarations,
 * none of mingw-w64's runtime linked (see the Makefile's -nostdlib).
 *
 * It exercises COMCTL32's report-mode list view (W32A-8): InitCommonControlsEx,
 * a real top-level window (W32A-5), two columns and a few rows, driven by the
 * personality's message loop.
 *
 *     make                       # needs x86_64-w64-mingw32-gcc
 *     run listview.exe           # on AuraLite
 */

#include <windows.h>
#include <commctrl.h>

static const wchar_t CLASS_NAME[] = L"AuraLiteListViewExample";
static HWND g_list;

/* .bss-zeroed statics: no memset under -nostdlib. */
static LVCOLUMNW col;
static LVITEMW   item;

static void add_column(int i, int cx, const wchar_t *text) {
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = cx;
    col.iSubItem = i;
    col.pszText = (LPWSTR)text;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
}

static void add_row(int row, const wchar_t *a, const wchar_t *b) {
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)a;
    SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    item.iSubItem = 1;
    item.pszText = (LPWSTR)b;
    SendMessageW(g_list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
}

static LRESULT __stdcall WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT,
                                 0, 0, 480, 320, hwnd, NULL, NULL, NULL);
        add_column(0, 200, L"Module");
        add_column(1, 120, L"D9 class");
        add_row(0, L"kernel32", L"REAL");
        add_row(1, L"crypt32",  L"FAIL-CLEAN");
        add_row(2, L"comctl32", L"REAL");
        return 0;
    case WM_SIZE:
        MoveWindow(g_list, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void __stdcall winstart(void) {
    static INITCOMMONCONTROLSEX icc;
    static WNDCLASSEXW wc;
    static MSG m;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"AuraLite — list view",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 520, 380,
                               NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        ExitProcess(1);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ExitProcess((UINT)m.wParam);
}
