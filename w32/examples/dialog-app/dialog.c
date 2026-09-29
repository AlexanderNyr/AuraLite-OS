/* w32/examples/dialog-app/dialog.c — a common-dialog Win32 program.
 *
 * W32APP_PLAN.md phase W32A-18, the ladder's dialog rung.  Apache-2.0; it
 * uses mingw-w64's public-domain <windows.h> declarations and links none of
 * mingw-w64's runtime (see the Makefile's -nostdlib), exactly like the
 * console-app example.
 *
 * It exercises COMDLG32 (GetOpenFileNameW, W32A-10) driven from USER32's
 * dialog engine (W32A-6): the personality shows a real modal file dialog and
 * returns the chosen path, or reports the honest "cancelled" outcome.
 *
 *     make                       # needs x86_64-w64-mingw32-gcc
 *     run dialog.exe             # on AuraLite
 */

#include <windows.h>

/* File-scope statics are zero-initialised in .bss, so we need no memset --
 * which matters under -nostdlib, where the CRT's memset is not linked. */
static OPENFILENAMEW ofn;
static wchar_t path[MAX_PATH];

void __stdcall winstart(void) {
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = NULL;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0Text\0*.txt\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle  = L"AuraLite — pick a file";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        MessageBoxW(NULL, path, L"You picked", MB_OK | MB_ICONINFORMATION);
    } else if (CommDlgExtendedError() == 0) {
        MessageBoxW(NULL, L"Cancelled — no file chosen.",
                    L"Open dialog", MB_OK);
    } else {
        MessageBoxW(NULL, L"The dialog reported an error.",
                    L"Open dialog", MB_OK | MB_ICONERROR);
    }
    ExitProcess(0);
}
