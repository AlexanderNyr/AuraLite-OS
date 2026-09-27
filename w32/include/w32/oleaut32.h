/* BSTR and VARIANT memory ownership (W32A-1 + W32A-11).
 * Interface layouts are published facts, not copied SDK code.
 * Unsupported array/record arms refuse without damaging ownership.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef AURALITE_W32_OLEAUT32_H
#define AURALITE_W32_OLEAUT32_H
#include "w32_abi.h"

typedef uint16_t W32_OLECHAR;
typedef W32_OLECHAR *W32_BSTR;
typedef struct {
    uint16_t vt, reserved1, reserved2, reserved3;
    union { void *ptr; uint64_t u64; int32_t lval; double dbl; } u;
} W32_VARIANT;
#define W32_VT_EMPTY 0u
#define W32_VT_NULL 1u
#define W32_VT_I2 2u
#define W32_VT_I4 3u
#define W32_VT_R4 4u
#define W32_VT_R8 5u
#define W32_VT_CY 6u
#define W32_VT_DATE 7u
#define W32_VT_BSTR 8u
#define W32_VT_DISPATCH 9u
#define W32_VT_ERROR 10u
#define W32_VT_BOOL 11u
#define W32_VT_UNKNOWN 13u
#define W32_VT_UI1 17u
#define W32_VT_UI2 18u
#define W32_VT_UI4 19u
#define W32_VT_I8 20u
#define W32_VT_UI8 21u
#define W32_VT_INT 22u
#define W32_VT_UINT 23u
#define W32_VT_ARRAY 0x2000u
#define W32_VT_BYREF 0x4000u
#define W32_S_OK 0u
#define W32_E_NOTIMPL 0x80004001u
#define W32_E_OUTOFMEMORY 0x8007000Eu
#define W32_E_INVALIDARG 0x80070057u

W32ABI void VariantInit(W32_VARIANT *pvarg);
W32ABI W32_BSTR SysAllocString(const W32_OLECHAR *psz);
W32ABI W32_BSTR SysAllocStringLen(const W32_OLECHAR *pch, unsigned cch);
W32ABI W32_BSTR SysAllocStringByteLen(const char *psz, unsigned len);
W32ABI void SysFreeString(W32_BSTR bstr);
W32ABI unsigned SysStringLen(W32_BSTR bstr);
W32ABI unsigned SysStringByteLen(W32_BSTR bstr);
W32ABI W32_DWORD VariantClear(W32_VARIANT *pvarg);
W32ABI W32_DWORD VariantCopy(W32_VARIANT *dest, const W32_VARIANT *src);
#endif
