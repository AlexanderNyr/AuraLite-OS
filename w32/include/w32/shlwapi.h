/* shlwapi.h — W32APP_PLAN.md phase W32A-10: the SHLWAPI pure modules.
 *
 * Everything in this header is an interface fact about the documented
 * SHLWAPI path-utility and colour-utility API, pinned from published
 * documentation exactly the way the A-8 control constants were (see
 * w32/PROVENANCE.md): the values below are documented constants, not
 * copied code.  No Microsoft header text, code, or binary is included.
 *
 * Scope (decision D1): exactly the 18 ladder-measured shlwapi.dll
 * symbols (w32/stub_map.tsv at the W32A-9 baseline), all REAL, all
 * implemented in w32/src/shlwapi.c:
 *   - the Path* string family (14) — pure UTF-16 string surgery, no
 *     filesystem contact except PathFileExistsW;
 *   - the Color* HLS family (3) — pure arithmetic, the documented
 *     0..240 WORD ranges;
 *   - AssocQueryStringW — the association table is empty by design and
 *     documented (docs/win32.md); every query answers S_FALSE with a
 *     zero length, which is the documented "no association" result.
 *
 * Where the published documentation leaves an algorithm underdetermined
 * (PathCompactPathExW's exact compaction order), this header documents
 * the choice the engine makes; the host gate pins it so it cannot drift.
 *
 * Licensed Apache-2.0; interface facts only.
 */

#ifndef AURALITE_W32_SHLWAPI_H
#define AURALITE_W32_SHLWAPI_H

#include "w32_abi.h"
#include "kernel32.h"          /* W32_WCHAR/BOOL/DWORD types */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Path* ------------------------------------------------------------------ */

/* All Path* functions take/return UTF-16LE (the W spelling the ledger
 * imports).  Path semantics documented in the Win32 path functions
 * topic: a "slash" is \ or /, a drive prefix is [A-Za-z]:, and the
 * functions never touch the filesystem unless their name says so. */

/* Pointer to the first character of the file-name component (after the
 * last slash); the start of the string when there is none. */
W32_LPCWSTR W32ABI PathFindFileNameW(W32_LPCWSTR path);
/* Pointer to the last '.' in the final component if that dot is not
 * its first character, else the terminating NUL. */
W32_LPCWSTR W32ABI PathFindExtensionW(W32_LPCWSTR path);
/* Remove the file spec in place (truncate at the last slash).  TRUE
 * when something was removed. */
W32_BOOL    W32ABI PathRemoveFileSpecW(W32_LPWSTR path);
/* Remove the path prefix in place (keep the file name).  No return
 * value (documented void). */
void        W32ABI PathStripPathW(W32_LPWSTR path);
/* Append a backslash + more to path in place; an absolute `more`
 * replaces the buffer, "" is a no-op.  TRUE when appended. */
W32_BOOL    W32ABI PathAppendW(W32_LPWSTR path, W32_LPCWSTR more);
/* dest = dir + '\' + file (an absolute file wins; NULL sides are
 * handled per the documented contract).  Returns dest. */
W32_LPWSTR  W32ABI PathCombineW(W32_LPWSTR dest, W32_LPCWSTR dir,
                                W32_LPCWSTR file);
/* Append an extension if the final component has none.  A NULL pszExt
 * appends ".exe" (the documented default); a path that already ends
 * in an extension is left alone and still reports TRUE. */
W32_BOOL    W32ABI PathAddExtensionW(W32_LPWSTR path, W32_LPCWSTR ext);
/* Remove the extension in place (documented void). */
void        W32ABI PathRemoveExtensionW(W32_LPWSTR path);
/* TRUE when path has no drive prefix and does not start with a slash. */
W32_BOOL    W32ABI PathIsRelativeW(W32_LPCWSTR path);
/* TRUE for a UNC (\\server\share) prefix.  There is no network
 * provider, so this is a pure spelling test. */
W32_BOOL    W32ABI PathIsNetworkPathW(W32_LPCWSTR path);
/* Drive letter as 0..25 ('C' -> 2), -1 when there is none. */
int         W32ABI PathGetDriveNumberW(W32_LPCWSTR path);
/* TRUE when the file-name component matches a wildcard pattern
 * (* and ?, ASCII case-insensitive). */
W32_BOOL    W32ABI PathMatchSpecW(W32_LPCWSTR path, W32_LPCWSTR spec);
/* TRUE when the path names an existing file or directory (the one
 * Path* call that touches the filesystem). */
W32_BOOL    W32ABI PathFileExistsW(W32_LPCWSTR path);
/* Compact path into cchMax characters, replacing leading components
 * with "..." while preserving the final component; a final component
 * that is itself too long is truncated with a trailing "...".  (The
 * published documentation names the ellipses-from-the-front idea; the
 * exact fallback order is our documented choice, pinned by the host
 * gate.)  Returns TRUE when the string was compacted. */
W32_BOOL    W32ABI PathCompactPathExW(W32_LPWSTR out, W32_LPCWSTR path,
                                      W32_UINT cchMax, W32_DWORD flags);

/* ---- Color* (the HLS triple lives in WORDs, 0..240) -------------------------- */

/* rgb -> (hue 0..240, luminance 0..240, saturation 0..240).  Achromatic
 * colours keep the hue at 0.  Documented void. */
void   W32ABI ColorRGBToHLS(W32_DWORD rgb, W32_WORD *hue,
                            W32_WORD *lum, W32_WORD *sat);
/* (hue, lum, sat) -> rgb. */
W32_DWORD W32ABI ColorHLSToRGB(W32_WORD hue, W32_WORD lum, W32_WORD sat);
/* Adjust luminance: n is in 0.1% units of the full range.  scaled=TRUE
 * is relative (n -1000..1000), FALSE absolute (0..1000, clamped).
 * Hue and saturation pass through. */
W32_DWORD W32ABI ColorAdjustLuma(W32_DWORD rgb, int n, W32_BOOL scaled);

/* ---- association queries ------------------------------------------------------ */

/* ASSOCF flags (documented): we accept the documented flags and ignore
 * the ones that only affect deferred/inherit lookups (there is no
 * registry behind this table). */
#define W32_ASSOCF_NONE                  0x00000000u
#define W32_ASSOCF_INIT_NOREMAPCLSID     0x00000001u
#define W32_ASSOCF_INIT_BYEXENAME        0x00000002u
#define W32_ASSOCF_OPEN_BYEXENAME        0x00000004u
#define W32_ASSOCF_INIT_DEFAULTTOSTAR    0x00000008u
#define W32_ASSOCF_INIT_IGNOREUNKNOWN    0x00000010u

/* ASSOCSTR values (documented). */
#define W32_ASSOCSTR_COMMAND             1u
#define W32_ASSOCSTR_EXECUTABLE          2u
#define W32_ASSOCSTR_FRIENDLYDOCNAME     3u
#define W32_ASSOCSTR_FRIENDLYAPPNAME     4u
#define W32_ASSOCSTR_NOOPEN              5u
#define W32_ASSOCSTR_SHELLNEWVALUE       6u
#define W32_ASSOCSTR_DDECOMMAND          7u
#define W32_ASSOCSTR_DDEIFEXEC           8u
#define W32_ASSOCSTR_DDEAPPLICATION      9u
#define W32_ASSOCSTR_DDETOPIC           10u
#define W32_ASSOCSTR_INFOTIP            11u
#define W32_ASSOCSTR_QUICKTIP           12u
#define W32_ASSOCSTR_TILEINFO           13u
#define W32_ASSOCSTR_CONTENTTYPE        14u
#define W32_ASSOCSTR_DEFAULTICON        15u
#define W32_ASSOCSTR_SHELLEXTENSION     16u

/* The association table is empty by design (docs/win32.md): there are
 * no file associations, so every query answers S_FALSE with *pcchOut
 * 0 — the documented "no association" result — and the reason is
 * logged once.  S_FALSE is 0x00000001, as everywhere. */
W32_LONG W32ABI AssocQueryStringW(W32_DWORD flags, W32_DWORD str,
                                  W32_LPCWSTR assoc, W32_LPCWSTR extra,
                                  W32_LPWSTR out, W32_DWORD *pcchOut);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_W32_SHLWAPI_H */
