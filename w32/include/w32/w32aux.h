/* w32aux.h — W32APP_PLAN.md phase W32A-10: the small modules.
 *
 * Everything in this header is an interface fact about the documented
 * APIs of the seven one-purpose DLLs the ladder measured (published
 * documentation pinned the way the A-8 constants were — see
 * w32/PROVENANCE.md; documented constants, not copied code):
 *
 *   WININET  InternetCrackUrlW  — the pure URL parser, REAL;
 *   DBGHELP  ImageNtHeader      — trivially REAL (pointer arithmetic
 *                                 over a mapped module);
 *   DWMAPI   the pair           — composition is off: colourisation
 *                                 reports the default constant,
 *                                 window attributes report E_NOTIMPL,
 *                                 each with the reason recorded;
 *   SENSAPI  the pair           — REAL best-effort probes: a TCP
 *                                 connect with the documented short
 *                                 path, the outcome reported, never
 *                                 hardcoded;
 *   WINTRUST WinVerifyTrust     — signature status "unknown":
 *                                 TRUST_E_NOSIGNATURE, no trust
 *                                 decision made or implied;
 *   CRYPT32  the eight          — FAIL-CLEAN: no certificate store,
 *                                 no crypt message; invalid handles
 *                                 answer ERROR_INVALID_HANDLE, lookups
 *                                 answer CRYPT_E_NOT_FOUND, and
 *                                 CryptQueryObject names its refusal.
 *
 * Licensed Apache-2.0; interface facts only.
 */

#ifndef AURALITE_W32_W32AUX_H
#define AURALITE_W32_W32AUX_H

#include "w32_abi.h"
#include "kernel32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- WININET: InternetCrackUrlW ---------------------------------------------- */

#define W32_INTERNET_SCHEME_PARTIAL (-1)
#define W32_INTERNET_SCHEME_DEFAULT   0
#define W32_INTERNET_SCHEME_FTP       1
#define W32_INTERNET_SCHEME_GOPHER    2
#define W32_INTERNET_SCHEME_HTTP      3
#define W32_INTERNET_SCHEME_HTTPS     4
#define W32_INTERNET_SCHEME_FILE      5

#define W32_ICU_DECODE     0x10000000u   /* accepted: decoding is identity
                                          * here (the parser hands out the
                                          * raw substrings, like the real
                                          * one without ICU_DECODE) */
#define W32_ICU_ESCAPE     0x80000000u   /* accepted: no-op (no escaper) */

typedef struct {
    W32_DWORD   dwStructSize;
    W32_LPWSTR  lpszScheme;
    W32_DWORD   dwSchemeLength;
    int32_t     nScheme;
    W32_LPWSTR  lpszHostName;
    W32_DWORD   dwHostNameLength;
    W32_WORD    nPort;
    W32_LPWSTR  lpszUserName;
    W32_DWORD   dwUserNameLength;
    W32_LPWSTR  lpszPassword;
    W32_DWORD   dwPasswordLength;
    W32_LPWSTR  lpszUrlPath;
    W32_DWORD   dwUrlPathLength;
    W32_LPWSTR  lpszExtraInfo;
    W32_DWORD   dwExtraInfoLength;
} W32_URL_COMPONENTSW;

/* The documented contract: a member is cracked when its pointer is
 * non-NULL and its length holds the buffer size; on return the
 * lengths are the component lengths (NULs are written when they fit).
 * A too-small buffer fails with ERROR_INSUFFICIENT_BUFFER and the
 * needed lengths.  nPort carries the URL's port, or the scheme
 * default (21/80/443) when the URL spells none — our documented
 * choice, pinned by the host gate. */
W32_BOOL W32ABI InternetCrackUrlW(W32_LPCWSTR url, W32_DWORD len,
                                           W32_DWORD flags,
                                           W32_URL_COMPONENTSW *parts);

/* ---- DBGHELP: ImageNtHeader --------------------------------------------------- */

/* PIMAGE_NT_HEADERS for a mapped module (GetModuleHandleW's answer),
 * or NULL when the base does not carry MZ + e_lfanew + PE\0\0. */
const void *W32ABI ImageNtHeader(const void *base);

/* ---- DWMAPI: the pair ---------------------------------------------------------- */

/* S_OK (0) + the default colourisation (0xFF000000, opaque black) —
 * composition is off and the default is a constant, the reason
 * recorded in docs/win32.md. */
W32_LONG W32ABI DwmGetColorizationColor(W32_DWORD *color, W32_BOOL *opaque);
/* E_NOTIMPL: there is no composition to set attributes on. */
W32_LONG W32ABI DwmSetWindowAttribute(W32_HANDLE hwnd, W32_DWORD attr,
                                               const void *val, W32_DWORD size);

/* ---- SENSAPI: the probes -------------------------------------------------------- */

/* TRUE + the alive flags (0: no known subnetwork semantics) when a
 * TCP connect to the user-network gateway answers; FALSE when the
 * stack is absent or the connect is refused.  Never hardcoded — the
 * probe runs on every call. */
W32_BOOL W32ABI IsNetworkAlive(W32_DWORD *flags);
/* TRUE when a TCP connect to "a.b.c.d[:port]" (port default 80)
 * succeeds; FALSE otherwise (unparseable host included).  The QOCINFO
 * speeds are reported as 0 (unknown), the documented shape. */
W32_BOOL W32ABI IsDestinationReachableW(W32_LPCWSTR dest, void *info);

typedef struct {
    W32_DWORD dwSize;
    W32_DWORD dwInSpeed;    /* 0: unknown */
    W32_DWORD dwOutSpeed;   /* 0: unknown */
} W32_QOCINFO;

/* ---- WINTRUST: WinVerifyTrust --------------------------------------------------- */

#define W32_TRUST_E_NOSIGNATURE 0x800B0100uL   /* the "unknown" answer:
                                                * no trust decision made */

W32_LONG W32ABI WinVerifyTrust(void *hwnd, const void *action,
                                        const void *data);

/* ---- CRYPT32: the eight (FAIL-CLEAN) ---------------------------------------------- */

#define W32_CRYPT_E_NOT_FOUND 0x80092009uL

W32_BOOL  W32ABI CryptQueryObject(W32_DWORD kind, const void *source,
                                           W32_DWORD *ct, W32_DWORD *cd,
                                           W32_DWORD *st, W32_DWORD *sd,
                                           void **hctx, const void **pv);
void     *W32ABI CertFindCertificateInStore(void *store,
                                                     W32_DWORD enc,
                                                     W32_DWORD flags,
                                                     W32_DWORD type,
                                                     const void *what,
                                                     void *prev);
W32_BOOL  W32ABI CertGetCertificateContextProperty(void *ctx,
                                                            W32_DWORD prop,
                                                            void *data,
                                                            W32_DWORD *len);
W32_DWORD W32ABI CertGetNameStringW(void *ctx, W32_DWORD type,
                                             W32_DWORD flags, void *type8,
                                             W32_LPWSTR out, W32_DWORD cch);
W32_BOOL  W32ABI CertNameToStrW(W32_DWORD type, const void *blob,
                                         W32_DWORD flags, W32_LPWSTR out,
                                         W32_DWORD cch);
W32_BOOL  W32ABI CryptMsgClose(W32_HANDLE msg);
W32_BOOL  W32ABI CryptMsgGetParam(W32_HANDLE msg, W32_DWORD type,
                                           W32_DWORD index, void *data,
                                           W32_DWORD *len);
W32_BOOL  W32ABI CertCloseStore(void *store, W32_DWORD flags);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_W32_W32AUX_H */
