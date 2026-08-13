/* w32/examples/gui-app/window.c — a Win32 GUI program for AuraLite.
 *
 * WIN32_PLAN.md phase W32-8.  Apache-2.0; mingw-w64's public-domain
 * <windows.h> declarations, none of its runtime.
 *
 * Registers a class, creates a window, and paints in WM_PAINT.  On AuraLite
 * these map onto the native compositor (decision D5) rather than onto a
 * reimplementation of USER32.
 *
 * As with the console example, nothing here is AuraLite-specific.
 */

#include <windows.h>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT r = { 10, 10, 200, 60 };
        HBRUSH brush = CreateSolidBrush(RGB(40, 90, 200));
        FillRect(hdc, &r, brush);
        DeleteObject(brush);

        SetTextColor(hdc, RGB(255, 255, 255));
        TextOutA(hdc, 20, 28, "Win32 on AuraLite", 17);

        MoveToEx(hdc, 10, 80, NULL);
        LineTo(hdc, 200, 80);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void __stdcall winstart(void) {
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = WndProc;
    wc.lpszClassName = "AuraLiteExample";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, "AuraLiteExample", "Win32 Example",
                                WS_OVERLAPPEDWINDOW,
                                80, 80, 320, 200,
                                NULL, NULL, NULL, NULL);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    ExitProcess(0);
}
