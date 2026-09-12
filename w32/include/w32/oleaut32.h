/* oleaut32.h — BSTR and VARIANT memory management.
 * SPDX-License-Identifier: Apache-2.0 -- written for AuraLite OS.
 *
 * W32APP_PLAN.md phase W32A-1: these eight functions are REAL already.  They
 * are pure libc (malloc, length headers, memcpy) with no OS surface, so
 * they land early as REAL the way dbghelp's ImageNtHeader was "trivially
 * real" in the plan.  W32A-11 adopts them (BSTR/VARIANT vector suite)
 * instead of writing them, and fills in the rest of VARIANT there.
 *
 * Layouts (BSTR length prefix, VARIANT 16 bytes with vt at 0) are published
 * interface facts, written from documentation (w32/LICENSING.md).
 */

#ifndef AURALITE_W32_OLEAUT32_H
#define AURALITE_W32_OLEAUT32_H

#include "w32/w32_abi.h"

/* A BSTR is UTF-16 with a 4-byte byte-length prefix before the pointer and
 * a double NUL after the data.  Wide chars are 16-bit: never wchar_t. */
typedef uint16_t W32_OLECHAR;
typedef W32_OLECHAR *W32_BSTR;

/* VARIANT: 16 bytes.  Only the header and the BSTR arm are modelled here;
 * W32A-11 extends this to arrays and IDispatch. */
typedef struct {
    uint16_t vt;
    uint16_t reserved1;
    uint16_t reserved2;
    uint16_t reserved3;
    union {
        void    *ptr;
        uint64_t u64;
        int32_t  lval;
        double   dbl;
    } u;
} W32_VARIANT;

#define W32_VT_EMPTY 0u
#define W32_VT_BSTR  8u

#define W32_S_OK           0u
#define W32_E_NOTIMPL      0x80004001u
#define W32_E_OUTOFMEMORY  0x8007000Eu

W32ABI W32_BSTR   SysAllocString(const W32_OLECHAR *psz);
W32ABI W32_BSTR   SysAllocStringLen(const W32_OLECHAR *pch, unsigned cch);
W32ABI W32_BSTR   SysAllocStringByteLen(const char *psz, unsigned len);
W32ABI void       SysFreeString(W32_BSTR bstr);
W32ABI unsigned   SysStringLen(W32_BSTR bstr);
W32ABI unsigned   SysStringByteLen(W32_BSTR bstr);
W32ABI W32_DWORD  VariantClear(W32_VARIANT *pvarg);
W32ABI W32_DWORD  VariantCopy(W32_VARIANT *dest, const W32_VARIANT *src);

#endif /* AURALITE_W32_OLEAUT32_H */
