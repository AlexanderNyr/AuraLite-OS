/* w32/tests/w32c8_failclean.c — RESIDUE3_PLAN.md phase C8, box 3.
 *
 * SPDX-License-Identifier: Apache-2.0.  Written for AuraLite OS; it uses
 * mingw-w64's public headers only as a declaration source, and every call goes
 * through the personality's own binder by NAME.
 *
 * "Live FAIL-CLEAN receipts": docs/win32.md generates 35 functions across
 * nine modules that BIND and then report an honest failure.  Until this
 * fixture, that was a claim about source code: the personality's own host
 * tests call each one, but nothing proved that a real PE, loaded through
 * w32run's binder and calling by NAME, gets the same answer on the machine.
 *
 * This fixture is that proof.  It imports all 35 by name, calls each with the
 * smallest arguments that reach the refusal, checks the DOCUMENTED answer, and
 * prints one line per function:
 *
 *     [w32] FAIL-CLEAN <fn> -> <code> (as documented)
 *
 * A function that answers SUCCESS where the table documents a failure is a
 * real bug, and prints FAIL-<fn> instead — the fixture then exits 1.  A clean
 * run prints C8-FAILCLEAN-OK (<n>/35) and exits 78, matching the app-gate
 * fixtures.
 *
 * Three of the 35 are not refusals and are measured as what they are, with the
 * word in the line: `degraded` (AdjustTokenPrivileges answers TRUE with
 * ERROR_NOT_ALL_ASSIGNED — Windows' own "nothing was assigned" contract),
 * `constant` (DwmGetColorizationColor answers S_OK with opaque black, a value
 * the personality documents), and `probe` (the SENSAPI pair report real
 * reachability instead of refusing).  They are in the doc's FAIL-CLEAN list
 * and they behave that way on purpose; the receipt says which is which rather
 * than flattening them into "failed".
 */

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

typedef struct { DWORD dwSize, dwFlags1, dwShow, hwndOwner; } PRINTDLGW_MIN;

/* Declared here rather than dragged in with their SDK headers: the fixture
 * must call the DOCUMENTED names, and the personality binds by name, so a
 * header mismatch cannot silently change what is being tested. */
typedef LONG  NTSTATUS;
NTSTATUS LsaOpenPolicy(void *, void *, DWORD, void **);
NTSTATUS LsaAddAccountRights(void *, void *, void *, DWORD);
NTSTATUS LsaClose(void *);
BOOL  CertCloseStore(void *, DWORD);
void *CertFindCertificateInStore(void *, DWORD, DWORD, DWORD, const void *);
BOOL  CertGetCertificateContextProperty(void *, DWORD, void *, DWORD *);
DWORD CertGetNameStringW(void *, DWORD, DWORD, const void *, void *, DWORD);
DWORD CertNameToStrW(DWORD, const void *, DWORD, void *, DWORD);
BOOL  CryptMsgClose(void *);
BOOL  CryptMsgGetParam(void *, DWORD, DWORD, void *, DWORD *);
BOOL  CryptQueryObject(DWORD, const void *, DWORD *, DWORD *, DWORD *,
                       DWORD *, void **, const void **);
LONG  DwmGetColorizationColor(DWORD *, BOOL *);
LONG  DwmSetWindowAttribute(HWND, DWORD, const void *, DWORD);
BOOL  PrintDlgW(void *);
DWORD CommDlgExtendedError(void);
BOOL  IsNetworkAlive(DWORD *);
BOOL  IsDestinationReachableW(LPCWSTR, void *);
LONG  WinVerifyTrust(void *, const void *, const void *);
/* The IMM* prototypes come from windows.h's own imm.h -- declaring them a
 * second time is a type conflict (HIMC vs HANDLE), and the name is what the
 * binder matches anyway. */

/* winnt.h/winerror.h already carry most of these codes; the two that are
 * personality-side spellings (and C8_CRYPT_E_NOT_FOUND, which WIN32_LEAN_AND_MEAN
 * keeps out) are defined here. */
#ifndef STATUS_ACCESS_DENIED
#define STATUS_ACCESS_DENIED   0xC0000022L
#endif
#define C8_PDERR_NODEFAULTPRN  0x2006u
#define C8_ERROR_NO_SUCH_PRIV  1313u
#define C8_CRYPT_E_NOT_FOUND   0x80092009u
#define C8_TRUST_E_NOSIGNATURE 0x800B0100L
#define C8_E_NOTIMPL           0x80004001L

/* No CRT is linked (-nostdlib), so the compiler's own memset call has nothing
 * to resolve against: zero by hand. */
static void zero(void *p, unsigned n) {
    unsigned char *b = (unsigned char *)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

static HANDLE out;
static DWORD  written;
static int    fails, checks;

static void say(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    WriteFile(out, s, n, &written, NULL);
}

/* The receipt line.  `code` is what the call answered; `tail` says how that
 * answer is documented ("as documented", "degraded, ...", "probe", ...). */
static void receipt(const char *fn, const char *code, const char *tail) {
    checks++;
    say("[w32] FAIL-CLEAN ");
    say(fn);
    say(" -> ");
    say(code);
    say(" (");
    say(tail);
    say(")\r\n");
}
static void bad(const char *fn, const char *code, DWORD want, DWORD got) {
    char buf[16];
    say("FAIL-");
    say(fn);
    say(": got ");
    say(code);
    say(" err=");
    for (int i = 0; i < 8; i++) {           /* high nibble first */
        int nib = (int)((got >> ((7 - i) * 4)) & 0xF);
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[8] = 0;
    say(buf);
    (void)want;
    say("\r\n");
    fails++;
}

/* ---- the 35 ------------------------------------------------------------- */

static void advapi32_checks(void) {
    DWORD err;

    SetLastError(0);
    BOOL ok = AdjustTokenPrivileges(NULL, FALSE, NULL, 0, NULL, NULL);
    err = GetLastError();
    if (!ok || err != ERROR_NOT_ALL_ASSIGNED) bad("AdjustTokenPrivileges", "unexpected", 1300, err);
    else receipt("AdjustTokenPrivileges", "TRUE", "degraded: ERROR_NOT_ALL_ASSIGNED, nothing assigned");

    SetLastError(0);
    DWORD need = 0;
    ok = GetFileSecurityW(L"C:\\nope", 4 /*OWNER_SECURITY_INFORMATION*/, NULL, 0, &need);
    err = GetLastError();
    if (ok || err != 50 /*ERROR_NOT_SUPPORTED*/) bad("GetFileSecurityW", "unexpected", 50, err);
    else receipt("GetFileSecurityW", "FALSE", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    DWORD sidlen = 0, domlen = 0;
    int use_storage = 0;               /* SID_NAME_USE lives here */
    PSID_NAME_USE use = (PSID_NAME_USE)&use_storage;
    ok = LookupAccountNameW(NULL, L"Aura", NULL, &sidlen, NULL, &domlen, use);
    err = GetLastError();
    if (ok || err != ERROR_NONE_MAPPED) bad("LookupAccountNameW", "unexpected", 1332, err);
    else receipt("LookupAccountNameW", "FALSE", "as documented: ERROR_NONE_MAPPED");

    SetLastError(0);
    LUID luid;
    ok = LookupPrivilegeValueW(NULL, L"SeBackupPrivilege", &luid);
    err = GetLastError();
    if (ok || err != C8_ERROR_NO_SUCH_PRIV) bad("LookupPrivilegeValueW", "unexpected", 1313, err);
    else receipt("LookupPrivilegeValueW", "FALSE", "as documented: ERROR_NO_SUCH_PRIVILEGE");

    NTSTATUS st = LsaAddAccountRights(NULL, NULL, NULL, 0);
    if ((ULONG)st != (ULONG)STATUS_ACCESS_DENIED) bad("LsaAddAccountRights", "unexpected", 0xC0000022u, (DWORD)st);
    else receipt("LsaAddAccountRights", "0xC0000022", "as documented: STATUS_ACCESS_DENIED");

    st = LsaClose(NULL);
    if ((ULONG)st != (ULONG)STATUS_INVALID_HANDLE) bad("LsaClose", "unexpected", 0xC0000008u, (DWORD)st);
    else receipt("LsaClose", "0xC0000008", "as documented: STATUS_INVALID_HANDLE");

    void *policy = (void *)0x1;
    st = LsaOpenPolicy(NULL, NULL, 0, &policy);
    if ((ULONG)st != (ULONG)STATUS_ACCESS_DENIED || policy != NULL)
        bad("LsaOpenPolicy", "unexpected", 0xC0000022u, (DWORD)st);
    else receipt("LsaOpenPolicy", "0xC0000022", "as documented: STATUS_ACCESS_DENIED, no handle");

    SetLastError(0);
    HANDLE token = NULL;
    ok = OpenProcessToken(NULL, 0, &token);
    err = GetLastError();
    if (ok || err != ERROR_NO_TOKEN || token) bad("OpenProcessToken", "unexpected", 1008, err);
    else receipt("OpenProcessToken", "FALSE", "as documented: ERROR_NO_TOKEN");

    SetLastError(0);
    ok = SetFileSecurityW(L"C:\\nope", 4, NULL);
    err = GetLastError();
    if (ok || err != 50) bad("SetFileSecurityW", "unexpected", 50, err);
    else receipt("SetFileSecurityW", "FALSE", "as documented: ERROR_NOT_SUPPORTED");
}

static void comdlg32_checks(void) {
    PRINTDLGW_MIN pd;
    zero(&pd, (unsigned)sizeof pd);
    pd.dwSize = sizeof pd;
    SetLastError(0);
    BOOL ok = PrintDlgW(&pd);
    DWORD err = GetLastError();
    DWORD ext = CommDlgExtendedError();
    if (ok || err != 120 /*ERROR_CALL_NOT_IMPLEMENTED*/ || ext != C8_PDERR_NODEFAULTPRN)
        bad("PrintDlgW", "unexpected", 120, err);
    else receipt("PrintDlgW", "FALSE", "as documented: PDERR_NODEFAULTPRN, no printers");
}

static void crypt32_checks(void) {
    DWORD err;

    SetLastError(0);
    BOOL ok = CertCloseStore(NULL, 0);
    err = GetLastError();
    if (ok || err != 6) bad("CertCloseStore", "unexpected", 6, err);
    else receipt("CertCloseStore", "FALSE", "as documented: ERROR_INVALID_HANDLE");

    /* A non-NULL store is what makes this the CRYPT_E_NOT_FOUND branch: a
     * NULL store reports ERROR_INVALID_HANDLE instead (measured -- the first
     * run of this fixture expected CRYPT_E_NOT_FOUND from NULL and was
     * wrong about which refusal it was testing). */
    void *c = CertFindCertificateInStore((void *)&checks, 0, 0, 0, NULL);
    err = GetLastError();
    if (c != NULL || err != C8_CRYPT_E_NOT_FOUND) bad("CertFindCertificateInStore", "unexpected", C8_CRYPT_E_NOT_FOUND, err);
    else receipt("CertFindCertificateInStore", "NULL", "as documented: CRYPT_E_NOT_FOUND");

    SetLastError(0);
    DWORD len = 0;
    ok = CertGetCertificateContextProperty(NULL, 0, NULL, &len);
    err = GetLastError();
    if (ok || err != 6) bad("CertGetCertificateContextProperty", "unexpected", 6, err);
    else receipt("CertGetCertificateContextProperty", "FALSE", "as documented: ERROR_INVALID_HANDLE");

    DWORD n = CertGetNameStringW(NULL, 0, 0, NULL, NULL, 0);
    err = GetLastError();
    if (n != 0 || err != 6) bad("CertGetNameStringW", "unexpected", 6, err);
    else receipt("CertGetNameStringW", "0", "as documented: no name, ERROR_INVALID_HANDLE");

    n = CertNameToStrW(0, NULL, 0, NULL, 0);
    err = GetLastError();
    if (n != 0 || err != 6) bad("CertNameToStrW", "unexpected", 6, err);
    else receipt("CertNameToStrW", "0", "as documented: ERROR_INVALID_HANDLE");

    ok = CryptMsgClose(NULL);
    err = GetLastError();
    if (ok || err != 6) bad("CryptMsgClose", "unexpected", 6, err);
    else receipt("CryptMsgClose", "FALSE", "as documented: ERROR_INVALID_HANDLE");

    SetLastError(0);
    ok = CryptMsgGetParam(NULL, 0, 0, NULL, &len);
    err = GetLastError();
    if (ok || err != 6) bad("CryptMsgGetParam", "unexpected", 6, err);
    else receipt("CryptMsgGetParam", "FALSE", "as documented: ERROR_INVALID_HANDLE");

    SetLastError(0);
    ok = CryptQueryObject(0, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    err = GetLastError();
    if (ok || err != 120) bad("CryptQueryObject", "unexpected", 120, err);
    else receipt("CryptQueryObject", "FALSE", "as documented: ERROR_CALL_NOT_IMPLEMENTED");
}

static void dwm_checks(void) {
    DWORD colour = 0;
    BOOL  opaque = 0;
    LONG  hr = DwmGetColorizationColor(&colour, &opaque);
    if (hr != 0 || colour != 0xFF000000u || !opaque)
        bad("DwmGetColorizationColor", "unexpected", 0, (DWORD)hr);
    else receipt("DwmGetColorizationColor", "S_OK 0xFF000000",
                 "constant: composition off, documented opaque black");

    hr = DwmSetWindowAttribute(NULL, 0, NULL, 0);
    if ((ULONG)hr != (ULONG)C8_E_NOTIMPL) bad("DwmSetWindowAttribute", "unexpected", 0x80004001u, (DWORD)hr);
    else receipt("DwmSetWindowAttribute", "0x80004001", "as documented: E_NOTIMPL");
}

static void imm_checks(void) {
    DWORD err;
    HIMC imc = ImmGetContext(NULL);
    err = GetLastError();
    if (imc != NULL || err != 50) bad("ImmGetContext", "unexpected", 50, err);
    else receipt("ImmGetContext", "NULL", "as documented: no IME context");

    SetLastError(0);
    BOOL ok = ImmReleaseContext(NULL, NULL);
    err = GetLastError();
    if (ok || err != 50) bad("ImmReleaseContext", "unexpected", 50, err);
    else receipt("ImmReleaseContext", "FALSE", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    LONG n = ImmGetCompositionStringW(NULL, 0, NULL, 0);
    err = GetLastError();
    if (n != 0 || err != 50) bad("ImmGetCompositionStringW", "unexpected", 50, err);
    else receipt("ImmGetCompositionStringW", "0", "as documented: empty, buffer untouched");

    SetLastError(0);
    ok = ImmSetCompositionWindow(NULL, NULL);
    err = GetLastError();
    if (ok || err != 50) bad("ImmSetCompositionWindow", "unexpected", 50, err);
    else receipt("ImmSetCompositionWindow", "FALSE", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    ok = ImmSetCompositionFontA(NULL, NULL);
    err = GetLastError();
    if (ok || err != 50) bad("ImmSetCompositionFontA", "unexpected", 50, err);
    else receipt("ImmSetCompositionFontA", "FALSE", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    ok = ImmSetCompositionFontW(NULL, NULL);
    err = GetLastError();
    if (ok || err != 50) bad("ImmSetCompositionFontW", "unexpected", 50, err);
    else receipt("ImmSetCompositionFontW", "FALSE", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    ok = ImmSetCandidateWindow(NULL, NULL);
    err = GetLastError();
    if (ok || err != 50) bad("ImmSetCandidateWindow", "unexpected", 50, err);
    else receipt("ImmSetCandidateWindow", "FALSE", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    ok = ImmSetCompositionStringW(NULL, 0, NULL, 0, NULL, 0);
    err = GetLastError();
    if (ok || err != 50) bad("ImmSetCompositionStringW", "unexpected", 50, err);
    else receipt("ImmSetCompositionStringW", "FALSE", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    void *e = (void *)(ULONG_PTR)ImmEscapeW(NULL, NULL, 0, NULL);
    err = GetLastError();
    if (e != NULL || err != 50) bad("ImmEscapeW", "unexpected", 50, err);
    else receipt("ImmEscapeW", "NULL", "as documented: ERROR_NOT_SUPPORTED");

    SetLastError(0);
    ok = ImmNotifyIME(NULL, 0, 0, 0);
    err = GetLastError();
    if (ok || err != 50) bad("ImmNotifyIME", "unexpected", 50, err);
    else receipt("ImmNotifyIME", "FALSE", "as documented: ERROR_NOT_SUPPORTED");
}

static void kernel32_checks(void) {
    SetLastError(0);
    BOOL ok = ReadDirectoryChangesW(NULL, NULL, 0, FALSE, 0, NULL, NULL, NULL);
    DWORD err = GetLastError();
    if (ok || err != 50) bad("ReadDirectoryChangesW", "unexpected", 50, err);
    else receipt("ReadDirectoryChangesW", "FALSE", "as documented: ERROR_NOT_SUPPORTED");
}

static void sensapi_checks(void) {
    DWORD flags = 0xFFFF;
    BOOL alive = IsNetworkAlive(&flags);
    /* Not a refusal: the personality probes and reports.  Both answers are
     * honest, so the receipt records which one this machine gave. */
    receipt("IsNetworkAlive", alive ? "TRUE" : "FALSE",
            "probe: reachability reported, not invented");

    BOOL r = IsDestinationReachableW(L"10.0.2.2:53", NULL);
    receipt("IsDestinationReachableW", r ? "TRUE" : "FALSE",
            "probe: reachability reported, not invented");

    /* ...and the refusal half it does have: an empty destination. */
    SetLastError(0);
    r = IsDestinationReachableW(L"", NULL);
    if (r != FALSE) bad("IsDestinationReachableW", "unexpected", 0, (DWORD)r);
    else receipt("IsDestinationReachableW(empty)", "FALSE",
                 "as documented: nothing to probe");
}

static void user32_checks(void) {
    COMBOBOXINFO cbi;
    zero(&cbi, (unsigned)sizeof cbi);
    cbi.cbSize = sizeof cbi;
    SetLastError(0);
    BOOL ok = GetComboBoxInfo((HWND)(ULONG_PTR)0x1234, &cbi);
    DWORD err = GetLastError();
    if (ok || err != 6) bad("GetComboBoxInfo", "unexpected", 6, err);
    else receipt("GetComboBoxInfo", "FALSE", "as documented: ERROR_INVALID_HANDLE");
}

static void wintrust_checks(void) {
    LONG r = WinVerifyTrust(NULL, NULL, NULL);
    if ((ULONG)r != (ULONG)C8_TRUST_E_NOSIGNATURE) bad("WinVerifyTrust", "unexpected", 0x800B0100u, (DWORD)r);
    else receipt("WinVerifyTrust", "0x800B0100", "as documented: TRUST_E_NOSIGNATURE");
}

void __stdcall winstart(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    fails = 0;
    checks = 0;
    say("C8: FAIL-CLEAN live receipts (docs/win32.md generates 35)\r\n");

    advapi32_checks();     /*  9 */
    comdlg32_checks();     /* 10 */
    crypt32_checks();      /* 18 */
    dwm_checks();          /* 20 */
    imm_checks();          /* 30 */
    kernel32_checks();     /* 31 */
    sensapi_checks();      /* 34 (two probes + one refusal) */
    user32_checks();       /* 35 */
    wintrust_checks();     /* 36 */

    if (fails) {
        say("C8-FAILCLEAN-FAIL\r\n");
        ExitProcess(1);
    }
    say("C8-FAILCLEAN-OK\r\n");
    ExitProcess(78);
}
