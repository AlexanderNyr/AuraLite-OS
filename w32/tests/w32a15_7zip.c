/* w32/tests/w32a15_7zip.c — W32APP_PLAN.md phase W32A-15 guest fixture.
 *
 * The CI-automatable twin of the 7-Zip File Manager app gate.  A single
 * mingw-w64 TU linked -nostdlib with --entry=winstart (no CRT, no main) that
 * imports the exact KERNEL32 / USER32 / GDI32 / COMCTL32 / SHELL32 / ADVAPI32
 * surface the real 7zFM.exe drives, BY NAME, and walks the personality paths
 * 7-Zip actually hits — proven live against the pinned 7zFM.exe first (see
 * w32/tests/W32A15.probe.log):
 *
 *   TOOLBAR BITMAPS  the W32A-15 REAL slice: LoadBitmapW resolves an embedded
 *       RT_BITMAP (a 4bpp/16-colour packed DIB, 7-Zip's toolbar-strip format)
 *       and hands back a device HBITMAP — the loud user32 TODO the pinned
 *       7zFM.exe hit on its first paint until this phase.  GetObjectW reports
 *       the source dimensions; SelectObject into a memory DC + GetPixel proves
 *       the palette expansion put the right colours where the DIB said.  A
 *       missing id fails clean (NULL), not a fault.
 *
 *   DLL-CHAIN + msvcrt HEAP  LoadLibraryA the way 7z.dll's consumers pull the
 *       codec chain (COMCTL32) and the CRT (msvcrt) at run time, GetProcAddress
 *       InitCommonControlsEx and malloc/free/calloc, then a real heap
 *       round-trip — 7-Zip does all of its archive bookkeeping on the msvcrt
 *       heap resolved dynamically, so the gate proves that path.
 *
 *   REPORT LISTVIEW  the file-manager pane: a SysListView32 in report mode with
 *       a column and three rows (names/sizes), LVM_GETITEMCOUNT == 3 and the
 *       row text read back — the control 7zFM lists an archive's entries in.
 *
 *   DELAY-LOAD MPR  LoadLibraryA("MPR.DLL") + WNetOpenEnumW, then a call that
 *       must FAIL CLEAN (no network provider) — the exact delay path 7zFM
 *       fires on its first network-folder touch, a documented non-goal.
 *
 *   OPTIONS PERSISTENCE  the Reg*W set 7-Zip's Options sheet saves through:
 *       create HKCU\Software\7-Zip\FM, write a DWORD setting, close, reopen,
 *       read back and ASSERT it round-trips.
 *
 *   VFS FILE OP  SHFileOperationW FO_DELETE removes a file the extract flow
 *       created — asserted gone through GetFileAttributesW.
 *
 * Prints W32A15-7ZIP-OK and exits 78 on a clean run; FAIL-<mark> +
 * W32A15-7ZIP-FAIL / exit 1 otherwise.  Markers greppable by
 * tests/integration/cases/test_w32a15_7zip_fixture.sh.
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <windows.h>
#include <commctrl.h>

/* -nostdlib: the compiler may still emit these for copies/inits/zeroing. */
void *memset(void *d, int c, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n-- > 0) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    volatile const unsigned char *q = (volatile const unsigned char *)s;
    while (n-- > 0) *p++ = *q++;
    return d;
}

static HANDLE out;
static DWORD  written;
static int    fails;

static void say(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    WriteFile(out, s, n, &written, NULL);
}
static void mark_fail(const char *m) { say("FAIL-"); say(m); say("\r\n"); fails++; }
#define CHECK(cond, mark) do { if (!(cond)) mark_fail(mark); } while (0)

/* msvcrt heap entry points 7-Zip resolves dynamically. */
typedef void *(__cdecl *pmalloc)(size_t);
typedef void  (__cdecl *pfree)(void *);
typedef void *(__cdecl *prealloc)(void *, size_t);
/* mpr delay-load entry (network folders). */
typedef DWORD (WINAPI *pWNetOpenEnumW)(DWORD, DWORD, DWORD, void *, HANDLE *);
/* comctl chain entry. */
typedef BOOL  (WINAPI *pInitCCEx)(const INITCOMMONCONTROLSEX *);

void __stdcall winstart(void) {
    LONG rc;
    HKEY key = NULL;

    out   = GetStdHandle(STD_OUTPUT_HANDLE);
    fails = 0;
    say("W32A15: 7-Zip FM app-gate fixture start\r\n");

    HINSTANCE self = GetModuleHandleW(NULL);

    /* ---- toolbar bitmaps: the W32A-15 LoadBitmapW REAL slice ------------- */
    {
        HBITMAP hb = LoadBitmapW(self, MAKEINTRESOURCEW(100));
        CHECK(hb != NULL, "loadbitmap-null");
        if (hb) {
            BITMAP bm;
            memset(&bm, 0, sizeof bm);
            int got = GetObjectW(hb, sizeof bm, &bm);
            CHECK(got == (int)sizeof bm, "loadbitmap-getobject");
            CHECK(bm.bmWidth == 8 && bm.bmHeight == 8, "loadbitmap-dims");

            /* Select it into a memory DC and read pixels back: the border is
             * palette index 3 (red) and the interior a blue/green check --
             * proves the 4bpp->32bpp palette expansion landed correctly. */
            HDC mdc = CreateCompatibleDC(NULL);
            CHECK(mdc != NULL, "loadbitmap-memdc");
            if (mdc) {
                HGDIOBJ oldb = SelectObject(mdc, hb);
                COLORREF corner = GetPixel(mdc, 0, 0);           /* border */
                CHECK(corner == RGB(255, 0, 0), "loadbitmap-corner-red");
                COLORREF inside = GetPixel(mdc, 2, 2);           /* check   */
                CHECK(inside == RGB(0, 255, 0) || inside == RGB(0, 0, 255),
                      "loadbitmap-inside-check");
                SelectObject(mdc, oldb);
                DeleteDC(mdc);
            }
            DeleteObject(hb);
        }
        /* a resource that does not exist fails clean, not with a fault */
        HBITMAP miss = LoadBitmapW(self, MAKEINTRESOURCEW(4242));
        CHECK(miss == NULL, "loadbitmap-missing-null");
    }

    /* ---- DLL-chain + msvcrt heap, resolved at run time ------------------- */
    {
        HMODULE cc = LoadLibraryA("COMCTL32.DLL");
        CHECK(cc != NULL, "loadlibrary-comctl32");
        if (cc) {
            pInitCCEx fInit = (pInitCCEx)(void *)GetProcAddress(cc, "InitCommonControlsEx");
            CHECK(fInit != NULL, "getproc-initcommoncontrolsex");
            if (fInit) {
                INITCOMMONCONTROLSEX icc;
                icc.dwSize = sizeof icc;
                icc.dwICC  = ICC_LISTVIEW_CLASSES;
                CHECK(fInit(&icc) == TRUE, "initcommoncontrolsex");
            }
        }

        HMODULE crt = LoadLibraryA("msvcrt.dll");
        CHECK(crt != NULL, "loadlibrary-msvcrt");
        if (crt) {
            /* malloc/free/realloc is the exact heap trio 7-Zip imports from
             * msvcrt (see the ledger); calloc is NOT in its surface, so the
             * gate does not ask for it. */
            pmalloc  fmalloc  = (pmalloc) (void *)GetProcAddress(crt, "malloc");
            pfree    ffree    = (pfree)   (void *)GetProcAddress(crt, "free");
            prealloc frealloc = (prealloc)(void *)GetProcAddress(crt, "realloc");
            CHECK(fmalloc && ffree && frealloc, "getproc-msvcrt-heap");
            if (fmalloc && ffree && frealloc) {
                unsigned char *p = (unsigned char *)fmalloc(4096);
                CHECK(p != NULL, "msvcrt-malloc");
                if (p) {
                    p[0] = 0x7A; p[4095] = 0x5A;
                    CHECK(p[0] == 0x7A && p[4095] == 0x5A, "msvcrt-heap-rw");
                    unsigned char *q = (unsigned char *)frealloc(p, 8192);
                    CHECK(q != NULL, "msvcrt-realloc");
                    if (q) { CHECK(q[0] == 0x7A, "msvcrt-realloc-preserve"); ffree(q); }
                    else ffree(p);
                }
            }
        }
    }

    /* ---- report listview: 7zFM's archive-entry pane --------------------- */
    {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = self;
        wc.lpszClassName = L"W32A157ZipHost";
        ATOM a = RegisterClassExW(&wc);
        CHECK(a != 0, "listview-hostclass");

        HWND host = CreateWindowExW(0, L"W32A157ZipHost", L"w32a15", WS_OVERLAPPEDWINDOW,
                                    0, 0, 640, 400, NULL, NULL, self, NULL);
        CHECK(host != NULL, "listview-hostwin");

        HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT,
                                  0, 0, 620, 360, host, NULL, self, NULL);
        CHECK(lv != NULL, "listview-create");
        if (lv) {
            LVCOLUMNW col;
            memset(&col, 0, sizeof col);
            col.mask    = LVCF_TEXT | LVCF_WIDTH;
            col.cx      = 300;
            col.pszText = (LPWSTR)L"Name";
            rc = (LONG)SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
            CHECK(rc == 0, "listview-insertcol");

            static const wchar_t *names[3] = { L"alpha.txt", L"beta.txt", L"gamma.txt" };
            for (int i = 0; i < 3; i++) {
                LVITEMW it;
                memset(&it, 0, sizeof it);
                it.mask    = LVIF_TEXT;
                it.iItem   = i;
                it.pszText = (LPWSTR)names[i];
                rc = (LONG)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
                CHECK(rc == i, "listview-insertitem");
            }
            LONG cnt = (LONG)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
            CHECK(cnt == 3, "listview-count");

            wchar_t back[32];
            memset(back, 0, sizeof back);
            LVITEMW q;
            memset(&q, 0, sizeof q);
            q.mask       = LVIF_TEXT;
            q.iItem      = 1;
            q.pszText    = back;
            q.cchTextMax = 32;
            SendMessageW(lv, LVM_GETITEMTEXTW, 1, (LPARAM)&q);
            CHECK(back[0] == L'b' && back[1] == L'e', "listview-itemtext");
            DestroyWindow(lv);
        }
        if (host) DestroyWindow(host);
    }

    /* ---- delay-load MPR: first network-folder touch fails clean --------- */
    {
        HMODULE mpr = LoadLibraryA("MPR.DLL");
        CHECK(mpr != NULL, "loadlibrary-mpr");
        if (mpr) {
            pWNetOpenEnumW fEnum =
                (pWNetOpenEnumW)(void *)GetProcAddress(mpr, "WNetOpenEnumW");
            CHECK(fEnum != NULL, "getproc-wnetopenenumw");
            if (fEnum) {
                HANDLE he = NULL;
                DWORD  wr = fEnum(RESOURCE_GLOBALNET, RESOURCETYPE_DISK, 0, NULL, &he);
                /* no provider on the lite personality: anything but NO_ERROR,
                 * and above all no fault. */
                CHECK(wr != NO_ERROR, "mpr-failclean");
            }
        }
    }

    /* ---- options persistence: the Reg*W set the Options sheet saves ------ */
    {
        const wchar_t *path = L"Software\\7-Zip\\FM";
        DWORD disp = 0;
        rc = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0,
                             KEY_READ | KEY_WRITE, NULL, &key, &disp);
        CHECK(rc == ERROR_SUCCESS && key, "reg-create");

        DWORD flags = 0x00000007;   /* ShowDots|ShowRealFileIcons|FullRow */
        rc = RegSetValueExW(key, L"ListMode", 0, REG_DWORD,
                            (const BYTE *)&flags, sizeof flags);
        CHECK(rc == ERROR_SUCCESS, "reg-set");
        RegCloseKey(key); key = NULL;

        rc = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key);
        CHECK(rc == ERROR_SUCCESS && key, "reg-reopen");
        DWORD rd = 0, len = sizeof rd, type = 0;
        rc = RegQueryValueExW(key, L"ListMode", NULL, &type, (BYTE *)&rd, &len);
        CHECK(rc == ERROR_SUCCESS && type == REG_DWORD, "reg-get-rc");
        CHECK(rd == 0x00000007, "reg-roundtrip");
        RegCloseKey(key); key = NULL;
    }

    /* ---- VFS file op: SHFileOperationW FO_DELETE ------------------------- */
    {
        const wchar_t *fn = L"\\tmp\\w32a15_extract.tmp";
        HANDLE h = CreateFileW(fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h != INVALID_HANDLE_VALUE, "shfileop-create");
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, "extracted\r\n", 11, &w, NULL);
            CloseHandle(h);

            wchar_t from[64];
            memset(from, 0, sizeof from);
            int i = 0; while (fn[i]) { from[i] = fn[i]; i++; }
            from[i] = 0; from[i + 1] = 0;    /* double-NUL terminated list */

            SHFILEOPSTRUCTW op;
            memset(&op, 0, sizeof op);
            op.wFunc  = FO_DELETE;
            op.pFrom  = from;
            op.fFlags = FOF_NO_UI;
            int r = SHFileOperationW(&op);
            CHECK(r == 0, "shfileop-delete-rc");
            CHECK(GetFileAttributesW(fn) == INVALID_FILE_ATTRIBUTES,
                  "shfileop-gone");
        }
    }

    if (fails == 0) {
        say("W32A15-7ZIP-OK\r\n");
        ExitProcess(78);
    }
    say("W32A15-7ZIP-FAIL\r\n");
    ExitProcess(1);
}
