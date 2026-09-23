/* version.h — W32APP_PLAN.md phase W32A-10: the VERSION module.
 *
 * Everything in this header is an interface fact about the documented
 * file-version API, pinned from published documentation exactly the
 * way the A-8 control constants were (see w32/PROVENANCE.md): the
 * values below are documented constants, not copied code.  No
 * Microsoft header text, code, or binary is included.
 *
 * Scope (decision D1): exactly the 3 ladder-measured version.dll
 * symbols, all REAL in w32/src/version.c: GetFileVersionInfoSizeW,
 * GetFileVersionInfoW, VerQueryValueW — a real reader for the PE
 * VS_VERSION_INFO resource (the documented tree: the fixed
 * VS_FIXEDFILEINFO block plus the StringFileInfo tables), extending
 * the W32A-1 resource walk.  The reader takes a FILE PATH (like the
 * real API): it opens, reads and parses the image itself.
 *
 * Sub-block grammar (documented): "\\" -> the VS_FIXEDFILEINFO;
 * "\\StringFileInfo\\<lang>\\<key>" -> the string value.  Anything
 * else answers FALSE (ERROR_RESOURCE_TYPE_NOT_FOUND semantics, the
 * documented failure for a missing sub-block).
 *
 * Licensed Apache-2.0; interface facts only.
 */

#ifndef AURALITE_W32_VERSION_H
#define AURALITE_W32_VERSION_H

#include "w32_abi.h"
#include "kernel32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The fixed block's documented signature. */
#define W32_VS_FFI_SIGNATURE   0xFEEF04BDu
/* The documented structure version this reader accepts. */
#define W32_VS_FFI_STRUCVERSION_1_0 0x00010000u

/* Size the caller must allocate for the blob: the whole
 * VS_VERSION_INFO resource's byte length plus 2 (the engine's length
 * prefix -- see below), 0 when the file has none (the reason logged
 * once; GetLastError carries the file error). */
W32_DWORD W32ABI GetFileVersionInfoSizeW(W32_LPCWSTR path,
                                                  W32_DWORD *handle);
/* Copy the engine blob into the caller's buffer.  The blob is opaque
 * to the caller; VerQueryValueW reads it back.  Engine shape (the
 * Win32 API carries no length, so the prefix is what bounds the walk):
 * [u16 resource unit count][resource bytes][u16 zero pad]. */
W32_BOOL  W32ABI GetFileVersionInfoW(W32_LPCWSTR path, W32_DWORD handle,
                                              W32_DWORD len, void *data);
/* Parse the blob.  *out points INTO the blob (documented: no copy),
 * *outLen its size (bytes for the fixed block, UTF-16 units for
 * strings, NUL included). */
W32_BOOL  W32ABI VerQueryValueW(const void *block, W32_LPCWSTR sub,
                                         void **out, W32_UINT *outLen);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_W32_VERSION_H */
