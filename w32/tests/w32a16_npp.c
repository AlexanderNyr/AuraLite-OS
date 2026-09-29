/* w32/tests/w32a16_npp.c — W32APP_PLAN.md phase W32A-16 guest fixture.
 *
 * The CI-automatable twin of the Notepad++ app gate (App gate III).  A single
 * mingw-w64 TU linked -nostdlib with --entry=winstart (no CRT, no main) that
 * imports the exact KERNEL32 / USER32 / GDI32 / COMCTL32 / SHELL32 / SHLWAPI /
 * WININET / SENSAPI / WINTRUST surface the real notepad++.exe (8.8.9,
 * a470014b…) drives, BY NAME, and walks the personality paths Notepad++
 * actually hits — the ones that are headlessly assertable in a single process
 * without a framebuffer:
 *
 *   TAB BAR  the open-files tab strip: a real SysTabControl32 (WC_TABCONTROLW)
 *       with three TCM_INSERTITEMW pages, TCM_GETITEMCOUNT == 3, a
 *       TCM_SETCURSEL/TCM_GETCURSEL switch, TCM_DELETEITEM closing one tab
 *       down to two, and TCM_GETITEMW reading a page's text back — the control
 *       Notepad++ lists its open documents in.
 *
 *   PATH SURGERY  the SHLWAPI Path* family Notepad++ runs every filename
 *       through: PathFindFileNameW / PathFindExtensionW / PathStripPathW /
 *       PathRemoveFileSpecW / PathCombineW / PathMatchSpecW / PathIsRelativeW /
 *       PathAddExtensionW / PathRemoveExtensionW / PathCompactPathExW — asserted
 *       against known inputs.  These are pure UTF-16 string surgery and REAL.
 *
 *   DARK-MODE COLOUR  the SHLWAPI Color* family Notepad++'s theming uses:
 *       ColorRGBToHLS → ColorHLSToRGB round-trips a primary; ColorAdjustLuma
 *       darkens a colour (asserted to move toward black).
 *
 *   TRACED OPEN PATH  the §2.3 surprise recorded in the plan: notepad++.exe
 *       has NO GetOpenFileName import.  It reaches the shell namespace through
 *       SHCreateItemFromParsingName, so the gate asserts THAT path (not a
 *       dialog the app never opens): create an IShellItem for a VFS path,
 *       GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING) round-trips the path,
 *       Release frees it.  A NULL path fails clean.
 *
 *   SAVE / REOPEN  the file write-back path: CreateFileW CREATE_ALWAYS + WriteFile
 *       a buffer to /tmp, close, reopen read-only, ReadFile and ASSERT the
 *       bytes — save + reopen over the VFS, then a save-as to a second path.
 *
 *   PLUGIN DLL CHAIN  the run-time resolution Notepad++ does when it loads its
 *       plugins and the CRT: LoadLibraryA COMCTL32 + GetProcAddress
 *       InitCommonControlsEx (called), then LoadLibraryA msvcrt.dll +
 *       malloc/free — the exact dynamic-resolution shape the plugin host uses.
 *
 *   TRAY ICON  minimise-to-tray: Shell_NotifyIconW NIM_ADD (→ a compositor
 *       notification) then NIM_DELETE, both asserted TRUE.
 *
 *   OFFLINE UPDATER  the WinGUp updater path with no signed-object support and
 *       (here) an unknown network: InternetCrackUrlW parses the update URL
 *       (scheme HTTPS, host echoed), WinVerifyTrust returns TRUST_E_NOSIGNATURE
 *       (the updater's signature check never claims success), and the SENSAPI
 *       probes return a clean boolean without faulting — the D9 rule that an
 *       offline updater reports offline and never lies about success.
 *
 *   FRAME LAYOUT  the DeferWindowPos batch Notepad++ lays its docked panels,
 *       splitters, tab bar and edit view out with: BeginDeferWindowPos, three
 *       DeferWindowPos requests, one EndDeferWindowPos flush — asserted to be
 *       deferred (nothing moves until End), to apply every entry, and to
 *       consume the HDWP.  GetComboBoxInfo is asserted FAIL-CLEAN (no COMBOBOX
 *       control), the honest answer for the Find/Replace combos.
 *
 *   ICON BRIDGE  CreateIconIndirect builds an HICON from a 32bpp colour bitmap
 *       + AND mask; GetIconInfo splits one back into its two device bitmaps
 *       (dimensions asserted) — the pair NPP uses on its tab and tray icons.
 *
 *   FORMATTING  wsprintfW (the ms_abi guest export this phase bound; it used to
 *       resolve to a loud TODO stub) formats a mixed %s/%d/%x string, asserted
 *       byte-exact — Notepad++ formats status-bar/dialog text through it.
 *
 *   PLUGIN TEARDOWN  FreeLibraryWhenCallbackReturns drops a module reference
 *       (double-load, free once each way, no fault); ReadDirectoryChangesW is
 *       asserted FAIL-CLEAN with ERROR_NOT_SUPPORTED (no VFS change-journal —
 *       NPP falls back to manual reload).
 *
 * Prints W32A16-NPP-OK and exits 78 on a clean run; FAIL-<mark> +
 * W32A16-NPP-FAIL / exit 1 otherwise.  Markers greppable by
 * tests/integration/cases/test_w32a16_npp_fixture.sh.
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

/* --- prototypes we call by name; declared here to keep the TU header-light
 *     and to pin the exact ABI the personality binds (mingw import libs
 *     -lshlwapi/-lshell32/-lwininet/-lsensapi/-lwintrust resolve them). --- */
WINAPI LPCWSTR PathFindFileNameW(LPCWSTR);
WINAPI LPCWSTR PathFindExtensionW(LPCWSTR);
WINAPI BOOL    PathRemoveFileSpecW(LPWSTR);
WINAPI void    PathStripPathW(LPWSTR);
WINAPI LPWSTR  PathCombineW(LPWSTR, LPCWSTR, LPCWSTR);
WINAPI BOOL    PathMatchSpecW(LPCWSTR, LPCWSTR);
WINAPI BOOL    PathIsRelativeW(LPCWSTR);
WINAPI BOOL    PathAddExtensionW(LPWSTR, LPCWSTR);
WINAPI void    PathRemoveExtensionW(LPWSTR);
WINAPI BOOL    PathCompactPathExW(LPWSTR, LPCWSTR, UINT, DWORD);
WINAPI void    ColorRGBToHLS(COLORREF, WORD *, WORD *, WORD *);
WINAPI COLORREF ColorHLSToRGB(WORD, WORD, WORD);
WINAPI COLORREF ColorAdjustLuma(COLORREF, int, BOOL);

WINAPI LONG SHCreateItemFromParsingName(LPCWSTR, void *, const void *, void **);
/* Shell_NotifyIconW / NOTIFYICONDATAW come from <windows.h> (shellapi.h). */

/* A minimal IShellItem vtable view — the personality's is {QI,AddRef,Release,
 * GetDisplayName}; we only need Release + GetDisplayName here. */
typedef struct { void *vtbl; } ISI;
typedef struct {
    LONG  (WINAPI *QueryInterface)(void *, const void *, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    LONG  (WINAPI *GetDisplayName)(void *, LONG, LPWSTR *);
} ISIVtbl;
#define SIGDN_DESKTOPABSOLUTEPARSING 0x80018000L
/* The documented IShellItem IID {43826d1e-e718-42ee-bc55-a1e261c37bfe}. */
static const unsigned char IID_ISI[16] = {
    0x1e, 0x6d, 0x82, 0x43, 0x18, 0xe7, 0xee, 0x42,
    0xbc, 0x55, 0xa1, 0xe2, 0x61, 0x1c, 0x37, 0xfe
};

/* WININET URL parse — the updater's URL split. */
typedef struct {
    DWORD  dwStructSize;
    LPWSTR lpszScheme;      DWORD dwSchemeLength; int nScheme;
    LPWSTR lpszHostName;    DWORD dwHostNameLength;
    WORD   nPort;
    LPWSTR lpszUserName;    DWORD dwUserNameLength;
    LPWSTR lpszPassword;    DWORD dwPasswordLength;
    LPWSTR lpszUrlPath;     DWORD dwUrlPathLength;
    LPWSTR lpszExtraInfo;   DWORD dwExtraInfoLength;
} URLCOMP16;
WINAPI BOOL InternetCrackUrlW(LPCWSTR, DWORD, DWORD, URLCOMP16 *);
#define W32_INTERNET_SCHEME_HTTPS 4

/* WINTRUST / SENSAPI — the offline-updater trust + reachability probes. */
WINAPI LONG WinVerifyTrust(void *, const void *, const void *);
WINAPI BOOL IsNetworkAlive(DWORD *);
WINAPI BOOL IsDestinationReachableW(LPCWSTR, void *);
#define W32_TRUST_E_NOSIGNATURE ((LONG)0x800B0100)

typedef void *(__cdecl *pmalloc)(size_t);
typedef void  (__cdecl *pfree)(void *);
typedef BOOL  (WINAPI *pInitCCEx)(const INITCOMMONCONTROLSEX *);

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

static int weq(const wchar_t *a, const wchar_t *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}
static int wlen(const wchar_t *a) { int n = 0; while (a[n]) n++; return n; }

void __stdcall winstart(void) {
    out   = GetStdHandle(STD_OUTPUT_HANDLE);
    fails = 0;
    say("W32A16: Notepad++ app-gate fixture start\r\n");

    HINSTANCE self = GetModuleHandleW(NULL);

    /* ---- tab bar: the open-files tab strip ------------------------------- */
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof icc;
        icc.dwICC  = ICC_TAB_CLASSES;
        InitCommonControlsEx(&icc);

        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = self;
        wc.lpszClassName = L"W32A16NppHost";
        ATOM a = RegisterClassExW(&wc);
        CHECK(a != 0, "tab-hostclass");

        HWND host = CreateWindowExW(0, L"W32A16NppHost", L"w32a16", WS_OVERLAPPEDWINDOW,
                                    0, 0, 900, 640, NULL, NULL, self, NULL);
        CHECK(host != NULL, "tab-hostwin");

        HWND tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE,
                                   0, 0, 880, 28, host, NULL, self, NULL);
        CHECK(tab != NULL, "tab-create");
        if (tab) {
            static const wchar_t *docs[3] = { L"main.c", L"README.md", L"notes.txt" };
            for (int i = 0; i < 3; i++) {
                TCITEMW it;
                memset(&it, 0, sizeof it);
                it.mask    = TCIF_TEXT;
                it.pszText = (LPWSTR)docs[i];
                LONG r = (LONG)SendMessageW(tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&it);
                CHECK(r == i, "tab-insert");
            }
            LONG cnt = (LONG)SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0);
            CHECK(cnt == 3, "tab-count3");

            SendMessageW(tab, TCM_SETCURSEL, (WPARAM)2, 0);
            LONG cur = (LONG)SendMessageW(tab, TCM_GETCURSEL, 0, 0);
            CHECK(cur == 2, "tab-cursel");

            /* read a page's title back */
            wchar_t back[32];
            memset(back, 0, sizeof back);
            TCITEMW q;
            memset(&q, 0, sizeof q);
            q.mask       = TCIF_TEXT;
            q.pszText    = back;
            q.cchTextMax = 32;
            SendMessageW(tab, TCM_GETITEMW, (WPARAM)1, (LPARAM)&q);
            CHECK(weq(back, L"README.md"), "tab-itemtext");

            /* close one tab -> two remain */
            SendMessageW(tab, TCM_DELETEITEM, (WPARAM)1, 0);
            cnt = (LONG)SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0);
            CHECK(cnt == 2, "tab-count2");
            DestroyWindow(tab);
        }
        if (host) DestroyWindow(host);
    }

    /* ---- SHLWAPI path surgery: the filename family --------------------- */
    {
        const wchar_t *full = L"C:\\src\\npp\\main.c";
        CHECK(weq(PathFindFileNameW(full), L"main.c"), "path-filename");
        CHECK(weq(PathFindExtensionW(full), L".c"), "path-ext");

        wchar_t buf[64];
        memcpy(buf, full, (wlen(full) + 1) * 2);
        PathStripPathW(buf);
        CHECK(weq(buf, L"main.c"), "path-strip");

        memcpy(buf, full, (wlen(full) + 1) * 2);
        CHECK(PathRemoveFileSpecW(buf) == TRUE, "path-rmspec-rc");
        CHECK(weq(buf, L"C:\\src\\npp"), "path-rmspec");

        wchar_t comb[64];
        CHECK(PathCombineW(comb, L"C:\\src\\npp", L"README.md") != NULL, "path-combine-rc");
        CHECK(weq(comb, L"C:\\src\\npp\\README.md"), "path-combine");

        CHECK(PathMatchSpecW(L"main.c", L"*.c") == TRUE, "path-match-c");
        CHECK(PathMatchSpecW(L"main.c", L"*.cpp") == FALSE, "path-match-cpp");

        CHECK(PathIsRelativeW(L"docs\\a.txt") == TRUE, "path-rel-yes");
        CHECK(PathIsRelativeW(L"C:\\docs\\a.txt") == FALSE, "path-rel-no");

        wchar_t ae[64];
        memcpy(ae, L"session", sizeof(L"session"));
        CHECK(PathAddExtensionW(ae, L".xml") == TRUE, "path-addext-rc");
        CHECK(weq(ae, L"session.xml"), "path-addext");
        PathRemoveExtensionW(ae);
        CHECK(weq(ae, L"session"), "path-rmext");

        wchar_t comp[16];
        BOOL cr = PathCompactPathExW(comp, L"C:\\a\\b\\c\\d\\reallylong.txt", 15, 0);
        CHECK(cr == TRUE, "path-compact-rc");
        CHECK(wlen(comp) <= 15 && comp[0] != 0, "path-compact-len");
    }

    /* ---- SHLWAPI dark-mode colour arithmetic --------------------------- */
    {
        WORD h = 0, l = 0, s = 0;
        COLORREF base = RGB(0x40, 0x80, 0xC0);   /* a NPP editor accent */
        ColorRGBToHLS(base, &h, &l, &s);
        COLORREF rt = ColorHLSToRGB(h, l, s);
        /* HLS round-trip is integer-lossy; assert each channel is close. */
        int dr = (int)(rt & 0xFF)        - 0x40; if (dr < 0) dr = -dr;
        int dg = (int)((rt >> 8) & 0xFF) - 0x80; if (dg < 0) dg = -dg;
        int db = (int)((rt >> 16) & 0xFF)- 0xC0; if (db < 0) db = -db;
        CHECK(dr <= 6 && dg <= 6 && db <= 6, "color-roundtrip");

        /* darken by 30% of range: luminance must drop toward black. */
        COLORREF dark = ColorAdjustLuma(base, -300, TRUE);
        WORD h2 = 0, l2 = 0, s2 = 0;
        ColorRGBToHLS(dark, &h2, &l2, &s2);
        CHECK(l2 < l, "color-darken");
    }

    /* ---- traced open path: SHCreateItemFromParsingName (no GetOpenFileName) */
    {
        void *si = NULL;
        LONG hr = SHCreateItemFromParsingName(L"\\tmp\\open_me.txt", NULL,
                                              IID_ISI, &si);
        CHECK(hr == 0 && si != NULL, "shitem-create");
        if (hr == 0 && si) {
            ISIVtbl *v = *(ISIVtbl **)si;
            LPWSTR name = NULL;
            LONG g = v->GetDisplayName(si, SIGDN_DESKTOPABSOLUTEPARSING, &name);
            CHECK(g == 0 && name != NULL, "shitem-getname");
            if (name) {
                CHECK(weq(name, L"\\tmp\\open_me.txt"), "shitem-name");
                CoTaskMemFree(name);
            }
            v->Release(si);
        }
        /* NULL path fails clean (E_INVALIDARG), not a fault. */
        void *bad = (void *)1;
        LONG hr2 = SHCreateItemFromParsingName(NULL, NULL, IID_ISI, &bad);
        CHECK(hr2 != 0 && bad == NULL, "shitem-null-failclean");
    }

    /* ---- save / reopen over the VFS ------------------------------------ */
    {
        const wchar_t *fn  = L"\\tmp\\w32a16_doc.txt";
        const char    *txt = "#include <stdio.h>\r\nint main(void){return 0;}\r\n";
        DWORD tlen = 0; while (txt[tlen]) tlen++;

        HANDLE h = CreateFileW(fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h != INVALID_HANDLE_VALUE, "save-create");
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            CHECK(WriteFile(h, txt, tlen, &w, NULL) && w == tlen, "save-write");
            CloseHandle(h);

            HANDLE r = CreateFileW(fn, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            CHECK(r != INVALID_HANDLE_VALUE, "save-reopen");
            if (r != INVALID_HANDLE_VALUE) {
                char rb[128];
                DWORD got = 0;
                CHECK(ReadFile(r, rb, sizeof rb, &got, NULL) && got == tlen,
                      "save-readlen");
                int same = (got == tlen);
                for (DWORD i = 0; same && i < tlen; i++)
                    if (rb[i] != txt[i]) same = 0;
                CHECK(same, "save-bytes");
                CloseHandle(r);
            }
        }

        /* save-as to a second path */
        const wchar_t *fn2 = L"\\tmp\\w32a16_doc_copy.txt";
        HANDLE h2 = CreateFileW(fn2, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h2 != INVALID_HANDLE_VALUE, "saveas-create");
        if (h2 != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h2, txt, tlen, &w, NULL);
            CloseHandle(h2);
            CHECK(GetFileAttributesW(fn2) != INVALID_FILE_ATTRIBUTES, "saveas-exists");
        }
    }

    /* ---- plugin DLL chain + msvcrt heap, resolved at run time ---------- */
    {
        HMODULE cc = LoadLibraryA("COMCTL32.DLL");
        CHECK(cc != NULL, "plugin-loadcomctl");
        if (cc) {
            pInitCCEx fInit = (pInitCCEx)(void *)GetProcAddress(cc, "InitCommonControlsEx");
            CHECK(fInit != NULL, "plugin-getproc-icc");
            if (fInit) {
                INITCOMMONCONTROLSEX icc;
                icc.dwSize = sizeof icc;
                icc.dwICC  = ICC_TAB_CLASSES;
                CHECK(fInit(&icc) == TRUE, "plugin-callicc");
            }
        }
        HMODULE crt = LoadLibraryA("msvcrt.dll");
        CHECK(crt != NULL, "plugin-loadmsvcrt");
        if (crt) {
            pmalloc fmalloc = (pmalloc)(void *)GetProcAddress(crt, "malloc");
            pfree   ffree   = (pfree)  (void *)GetProcAddress(crt, "free");
            CHECK(fmalloc && ffree, "plugin-getproc-heap");
            if (fmalloc && ffree) {
                unsigned char *p = (unsigned char *)fmalloc(2048);
                CHECK(p != NULL, "plugin-malloc");
                if (p) { p[0] = 0x4E; p[2047] = 0x50; CHECK(p[0] == 0x4E && p[2047] == 0x50, "plugin-heap-rw"); ffree(p); }
            }
        }
    }

    /* ---- tray icon: minimise-to-tray ---------------------------------- */
    {
        NOTIFYICONDATAW nid;
        memset(&nid, 0, sizeof nid);
        nid.cbSize = sizeof nid;
        nid.hWnd   = (HWND)0x1;
        nid.uID    = 1;
        nid.uFlags = NIF_TIP;
        static const wchar_t tip[] = L"Notepad++";
        for (int i = 0; tip[i] && i < 63; i++) nid.szTip[i] = tip[i];
        CHECK(Shell_NotifyIconW(NIM_ADD, &nid) == TRUE, "tray-add");
        CHECK(Shell_NotifyIconW(NIM_DELETE, &nid) == TRUE, "tray-delete");
    }

    /* ---- offline updater: parse, distrust, probe clean ---------------- */
    {
        URLCOMP16 uc;
        memset(&uc, 0, sizeof uc);
        uc.dwStructSize = sizeof uc;
        wchar_t scheme[16], host[64], path[128];
        uc.lpszScheme = scheme; uc.dwSchemeLength = 16;
        uc.lpszHostName = host; uc.dwHostNameLength = 64;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 128;
        BOOL ok = InternetCrackUrlW(
            L"https://notepad-plus-plus.org/update/getDownloadUrl.php", 0, 0, &uc);
        CHECK(ok == TRUE, "upd-crackurl");
        CHECK(uc.nScheme == W32_INTERNET_SCHEME_HTTPS, "upd-scheme");
        CHECK(weq(host, L"notepad-plus-plus.org"), "upd-host");

        /* the updater's signature check never claims success. */
        LONG tr = WinVerifyTrust(NULL, NULL, NULL);
        CHECK(tr == W32_TRUST_E_NOSIGNATURE, "upd-notrust");

        /* the SENSAPI probes answer a clean boolean and never fault; an
         * empty destination is unreachable by definition (the offline arm
         * we can assert deterministically). */
        DWORD flags = 0;
        BOOL alive = IsNetworkAlive(&flags);
        CHECK(alive == TRUE || alive == FALSE, "upd-alive-clean");
        CHECK(IsDestinationReachableW(L"", NULL) == FALSE, "upd-empty-unreachable");
    }

    /* ---- W32A-16 REAL slices: the imports that used to hit loud stubs -----
     * The nine notepad++.exe imports this phase landed, driven by name so the
     * gate walks the REAL bodies (and the documented fail-clean ones). */

    /* DeferWindowPos: the flicker-free frame-layout batch.  Three top-level
     * windows (WS_POPUP: no decoration, so GetClientRect is the size we set)
     * are queued and flushed in one pass; the batch is deferred until End. */
    {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = self;
        wc.lpszClassName = L"W32A16Dwp";
        CHECK(RegisterClassExW(&wc) != 0, "dwp-class");

        HWND a = CreateWindowExW(0, L"W32A16Dwp", L"a", WS_POPUP, 0,  0,  100, 40, NULL, NULL, self, NULL);
        HWND b = CreateWindowExW(0, L"W32A16Dwp", L"b", WS_POPUP, 10, 10, 120, 50, NULL, NULL, self, NULL);
        HWND c = CreateWindowExW(0, L"W32A16Dwp", L"c", WS_POPUP, 20, 20, 90,  60, NULL, NULL, self, NULL);
        CHECK(a && b && c, "dwp-wins");

        HDWP hd = BeginDeferWindowPos(3);
        CHECK(hd != NULL, "dwp-begin");
        UINT fl = SWP_NOZORDER | SWP_NOACTIVATE;
        hd = DeferWindowPos(hd, a, NULL, 200, 100, 110, 80, fl);
        hd = DeferWindowPos(hd, b, NULL, 210, 110, 120, 90, fl);
        hd = DeferWindowPos(hd, c, NULL, 220, 120, 100, 70, fl);
        CHECK(hd != NULL, "dwp-defer");

        RECT r;
        /* deferred: nothing applied yet */
        if (a) { GetClientRect(a, &r); CHECK(r.right == 100 && r.bottom == 40, "dwp-deferred"); }

        CHECK(EndDeferWindowPos(hd) == TRUE, "dwp-end");
        if (a) { GetClientRect(a, &r); CHECK(r.right == 110 && r.bottom == 80, "dwp-applied-a"); }
        if (c) { GetClientRect(c, &r); CHECK(r.right == 100 && r.bottom == 70, "dwp-applied-c"); }
        /* the HDWP is consumed */
        CHECK(EndDeferWindowPos(hd) == FALSE, "dwp-consumed");

        /* GetComboBoxInfo: documented fail-clean (no COMBOBOX control) */
        if (a) {
            COMBOBOXINFO cbi;
            memset(&cbi, 0, sizeof cbi);
            cbi.cbSize = sizeof cbi;
            CHECK(GetComboBoxInfo(a, &cbi) == FALSE, "comboinfo-failclean");
        }
        if (a) DestroyWindow(a);
        if (b) DestroyWindow(b);
        if (c) DestroyWindow(c);
    }

    /* CreateIconIndirect / GetIconInfo: the icon<->bitmap bridge. */
    {
        static const unsigned int argb[16] = {
            0xFF102030u,0xFF405060u,0xFF708090u,0xFFA0B0C0u,
            0xFF112233u,0xFF445566u,0xFF778899u,0xFFAABBCCu,
            0xFF010203u,0xFF040506u,0xFF070809u,0xFF0A0B0Cu,
            0xFFFFFFFFu,0xFF000000u,0xFF123456u,0xFF654321u,
        };
        HBITMAP col = CreateBitmap(4, 4, 1, 32, argb);
        HBITMAP msk = CreateBitmap(4, 4, 1, 1, NULL);   /* 1bpp AND mask */
        CHECK(col != NULL && msk != NULL, "icon-bitmaps");
        ICONINFO ii;
        memset(&ii, 0, sizeof ii);
        ii.fIcon = TRUE; ii.hbmColor = col; ii.hbmMask = msk;
        HICON ico = CreateIconIndirect(&ii);
        CHECK(ico != NULL, "icon-create");

        ICONINFO back;
        memset(&back, 0, sizeof back);
        CHECK(GetIconInfo(ico, &back) == TRUE, "icon-getinfo");
        CHECK(back.fIcon == TRUE, "icon-ficon");
        CHECK(back.hbmColor != NULL && back.hbmMask != NULL, "icon-bitmaps-back");
        BITMAP bm;
        memset(&bm, 0, sizeof bm);
        if (back.hbmColor) {
            GetObjectW(back.hbmColor, sizeof bm, &bm);
            CHECK(bm.bmWidth == 4 && bm.bmHeight == 4, "icon-color-dims");
        }
        if (back.hbmColor) DeleteObject(back.hbmColor);
        if (back.hbmMask)  DeleteObject(back.hbmMask);
        if (ico) DestroyIcon(ico);
        if (col) DeleteObject(col);
        if (msk) DeleteObject(msk);
    }

    /* wsprintfW: the ms_abi guest twin (this is the export that used to be a
     * loud TODO stub).  A mixed %s/%d/%x format is asserted byte-exact. */
    {
        wchar_t wb[64];
        memset(wb, 0, sizeof wb);
        int n = wsprintfW(wb, L"%s=%d/%x", L"line", 42, 255);
        CHECK(n == 10, "wsprintf-len");
        CHECK(weq(wb, L"line=42/ff"), "wsprintf-text");
    }

    /* FreeLibraryWhenCallbackReturns: drops a module reference (a plugin's
     * thread-pool teardown).  Load twice, free once the ordinary way and once
     * through this call; the module survives the first and no call faults. */
    {
        HMODULE m1 = LoadLibraryA("comctl32.dll");
        HMODULE m2 = LoadLibraryA("comctl32.dll");
        CHECK(m1 != NULL && m2 != NULL, "flwcr-load");
        FreeLibraryWhenCallbackReturns(NULL, m2);   /* void: must not fault */
        if (m1) FreeLibrary(m1);
    }

    /* ReadDirectoryChangesW: documented fail-clean (no VFS change-journal). */
    {
        DWORD got = 0xdead;
        char buf[64];
        SetLastError(0);
        BOOL rc = ReadDirectoryChangesW((HANDLE)0x1, buf, sizeof buf, FALSE,
                                        FILE_NOTIFY_CHANGE_LAST_WRITE, &got,
                                        NULL, NULL);
        CHECK(rc == FALSE, "rdc-failclean");
        CHECK(got == 0, "rdc-zero");
        CHECK(GetLastError() == ERROR_NOT_SUPPORTED, "rdc-errno");
    }

    if (fails == 0) {
        say("W32A16-NPP-OK\r\n");
        ExitProcess(78);
    }
    say("W32A16-NPP-FAIL\r\n");
    ExitProcess(1);
}
