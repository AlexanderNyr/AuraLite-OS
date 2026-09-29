/* w32/tests/wr1_shellns.c — W32RUN_PLAN.md phase WR-1 guest fixture.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The CI-automatable twin of the SHELL32 namespace gate.  A single mingw-w64
 * TU linked -nostdlib with --entry=winstart (no CRT, no main) that drives the
 * exact IShellFolder / IEnumIDList / PIDL surface the pinned 7zFM.exe imports
 * (SHGetDesktopFolder + the PIDL info calls; W32RUN_PLAN D-WR1: this surface,
 * no more), through the documented COM vtable — the same call path a real
 * consumer takes.  It walks the namespace the implementation builds:
 *
 *     Desktop  ->  My Computer  ->  C:  ->  the filesystem (CFSFolder)
 *
 *   DESKTOP        SHGetDesktopFolder hands back a live IShellFolder; QI for
 *       IShellFolder and IUnknown both succeed.
 *   MY COMPUTER    EnumObjects yields at least one child; GetDisplayNameOf
 *       (SHGDN_NORMAL) is non-empty; GetAttributesOf reports SFGAO_FOLDER.
 *   DRIVE          BindToObject(My Computer) -> a folder that enumerates the
 *       drive; its parsing name resolves to a "C:"-shaped path.
 *   FILESYSTEM     BindToObject(C:) -> the CFSFolder; EnumObjects lists the
 *       real VFS root through FindFirstFileW; a directory child reports
 *       SFGAO_FOLDER|SFGAO_FILESYSTEM and its parsing name is absolute.
 *   PARSE          ParseDisplayName of an absolute path that exists returns a
 *       PIDL; a path that does not exist fails clean (no fault).
 *   FAIL-CLEAN     CreateViewObject (a GUI object — a documented non-goal on
 *       this layer) returns E_NOTIMPL, named, not a fault.
 *
 * Prints WR1-SHELLNS-OK and exits 78 on a clean run; FAIL-<mark> +
 * WR1-SHELLNS-FAIL / exit 1 otherwise.  Markers are greppable by
 * tests/integration/cases/test_wr1_shell_namespace.sh.
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <windows.h>
#include <shlobj.h>

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

static int wlen(const WCHAR *s) { int n = 0; while (s && s[n]) n++; return n; }

/* Pull a display name out of a STRRET (our namespace always returns WSTR, but
 * a faithful consumer handles the other arms too). Returns non-zero length. */
static int name_len(STRRET *sr, const ITEMIDLIST *pidl) {
    if (sr->uType == STRRET_WSTR) {
        int n = wlen(sr->pOleStr);
        if (sr->pOleStr) CoTaskMemFree(sr->pOleStr);
        return n;
    }
    if (sr->uType == STRRET_CSTR) { int n = 0; while (sr->cStr[n]) n++; return n; }
    if (sr->uType == STRRET_OFFSET) {
        const WCHAR *p = (const WCHAR *)((const BYTE *)pidl + sr->uOffset);
        return wlen(p);
    }
    return 0;
}

/* Copy the WSTR of a STRRET into buf (namespace always returns WSTR here). */
static void name_copy(STRRET *sr, WCHAR *buf, int cap) {
    int i = 0;
    if (sr->uType == STRRET_WSTR && sr->pOleStr) {
        for (; sr->pOleStr[i] && i + 1 < cap; i++) buf[i] = sr->pOleStr[i];
        CoTaskMemFree(sr->pOleStr);
    }
    buf[i] = 0;
}

void __stdcall winstart(void) {
    HRESULT hr;
    IShellFolder *desktop = NULL, *computer = NULL, *drive = NULL;
    IEnumIDList  *e = NULL;
    LPITEMIDLIST  pidl = NULL;
    ULONG fetched = 0;

    out   = GetStdHandle(STD_OUTPUT_HANDLE);
    fails = 0;
    say("WR1: SHELL32 namespace fixture start\r\n");

    CoInitialize(NULL);

    /* --- DESKTOP: a live IShellFolder + working QueryInterface --- */
    hr = SHGetDesktopFolder(&desktop);
    CHECK(hr == S_OK && desktop != NULL, "getdesktop");
    if (!desktop) { say("WR1-SHELLNS-FAIL\r\n"); ExitProcess(1); }

    {
        IUnknown *unk = NULL;
        hr = desktop->lpVtbl->QueryInterface(desktop, &IID_IShellFolder, (void **)&unk);
        CHECK(hr == S_OK && unk != NULL, "qi-ishellfolder");
        if (unk) unk->lpVtbl->Release(unk);
        unk = NULL;
        hr = desktop->lpVtbl->QueryInterface(desktop, &IID_IUnknown, (void **)&unk);
        CHECK(hr == S_OK && unk != NULL, "qi-iunknown");
        if (unk) unk->lpVtbl->Release(unk);
    }

    /* --- MY COMPUTER: enumerate the desktop, one browsable folder child --- */
    LPITEMIDLIST comp = NULL;
    hr = desktop->lpVtbl->EnumObjects(desktop, NULL,
                                      SHCONTF_FOLDERS, &e);
    CHECK(hr == S_OK && e != NULL, "desktop-enum");
    if (e) {
        hr = e->lpVtbl->Next(e, 1, &pidl, &fetched);
        CHECK(fetched == 1 && pidl != NULL, "desktop-enum-next");
        comp = pidl; pidl = NULL;
        e->lpVtbl->Release(e); e = NULL;
    }
    if (comp) {
        STRRET sr;
        hr = desktop->lpVtbl->GetDisplayNameOf(desktop, comp, SHGDN_NORMAL, &sr);
        CHECK(hr == S_OK && name_len(&sr, comp) > 0, "computer-name");

        ULONG attrs = SFGAO_FOLDER | SFGAO_HASSUBFOLDER;
        hr = desktop->lpVtbl->GetAttributesOf(desktop, 1,
                                              (LPCITEMIDLIST *)&comp, &attrs);
        CHECK(hr == S_OK && (attrs & SFGAO_FOLDER), "computer-attrs");
    }

    /* --- DRIVE: bind My Computer, enumerate to the C: drive --- */
    LPITEMIDLIST drivepidl = NULL;
    if (comp) {
        hr = desktop->lpVtbl->BindToObject(desktop, comp, NULL,
                                           &IID_IShellFolder, (void **)&computer);
        CHECK(hr == S_OK && computer != NULL, "bind-computer");
    }
    if (computer) {
        hr = computer->lpVtbl->EnumObjects(computer, NULL,
                                           SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e);
        CHECK(hr == S_OK && e != NULL, "computer-enum");
        if (e) {
            hr = e->lpVtbl->Next(e, 1, &pidl, &fetched);
            CHECK(fetched == 1 && pidl != NULL, "computer-enum-next");
            drivepidl = pidl; pidl = NULL;
            e->lpVtbl->Release(e); e = NULL;
        }
        if (drivepidl) {
            STRRET sr; WCHAR nm[64];
            hr = computer->lpVtbl->GetDisplayNameOf(computer, drivepidl,
                                                    SHGDN_FORPARSING, &sr);
            CHECK(hr == S_OK, "drive-parsename-rc");
            name_copy(&sr, nm, 64);
            CHECK(nm[0] == L'C' && nm[1] == L':', "drive-parsename-shape");
        }
    }

    /* --- FILESYSTEM: bind C:, enumerate the real VFS root --- */
    if (computer && drivepidl) {
        hr = computer->lpVtbl->BindToObject(computer, drivepidl, NULL,
                                            &IID_IShellFolder, (void **)&drive);
        CHECK(hr == S_OK && drive != NULL, "bind-drive");
    }
    if (drive) {
        int seen = 0, sawdir = 0;
        hr = drive->lpVtbl->EnumObjects(drive, NULL,
                                        SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e);
        CHECK(hr == S_OK && e != NULL, "fs-enum");
        while (e && e->lpVtbl->Next(e, 1, &pidl, &fetched) == S_OK && fetched == 1) {
            seen++;
            ULONG attrs = SFGAO_FOLDER | SFGAO_FILESYSTEM;
            drive->lpVtbl->GetAttributesOf(drive, 1,
                                           (LPCITEMIDLIST *)&pidl, &attrs);
            if ((attrs & SFGAO_FOLDER) && (attrs & SFGAO_FILESYSTEM)) {
                STRRET sr; WCHAR nm[MAX_PATH];
                if (drive->lpVtbl->GetDisplayNameOf(drive, pidl,
                                                    SHGDN_FORPARSING, &sr) == S_OK) {
                    name_copy(&sr, nm, MAX_PATH);
                    if (nm[0] == L'C' && nm[1] == L':') sawdir = 1;
                }
            }
            CoTaskMemFree(pidl); pidl = NULL;
        }
        if (e) { e->lpVtbl->Release(e); e = NULL; }
        CHECK(seen > 0, "fs-enum-nonempty");
        CHECK(sawdir, "fs-dir-absolute-parsename");

        /* --- PARSE: an absolute path that exists round-trips to a PIDL --- */
        LPITEMIDLIST parsed = NULL;
        ULONG eaten = 0, pattrs = 0;
        WCHAR root[] = L"C:\\";
        hr = drive->lpVtbl->ParseDisplayName(drive, NULL, NULL, root,
                                             &eaten, &parsed, &pattrs);
        CHECK(hr == S_OK && parsed != NULL, "parse-root");
        if (parsed) CoTaskMemFree(parsed);

        WCHAR nope[] = L"C:\\this_path_does_not_exist_wr1.zzz";
        parsed = NULL;
        hr = drive->lpVtbl->ParseDisplayName(drive, NULL, NULL, nope,
                                             &eaten, &parsed, &pattrs);
        CHECK(hr != S_OK && parsed == NULL, "parse-miss-failclean");

        /* --- FAIL-CLEAN: the GUI view object is a documented non-goal --- */
        void *view = (void *)1;
        hr = drive->lpVtbl->CreateViewObject(drive, NULL, &IID_IUnknown, &view);
        CHECK(hr == E_NOTIMPL && view == NULL, "createview-notimpl");
    }

    /* --- release the whole graph --- */
    if (drivepidl) CoTaskMemFree(drivepidl);
    if (comp) CoTaskMemFree(comp);
    if (drive) drive->lpVtbl->Release(drive);
    if (computer) computer->lpVtbl->Release(computer);
    if (desktop) desktop->lpVtbl->Release(desktop);
    CoUninitialize();

    if (fails == 0) {
        say("WR1-SHELLNS-OK\r\n");
        ExitProcess(78);
    }
    say("WR1-SHELLNS-FAIL\r\n");
    ExitProcess(1);
}
