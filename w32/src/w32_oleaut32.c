/* w32_oleaut32.c — BSTR and VARIANT memory management.
 * SPDX-License-Identifier: Apache-2.0 -- written for AuraLite OS.
 *
 * W32APP_PLAN.md phase W32A-1: the eight ladder-measured OLEAUT32 ordinals
 * (#2,4,6,7,9,10,149,150) are REAL here, not stubs.  They are pure memory
 * management -- no windows, no registry, no network -- so "real" costs
 * about a hundred lines of malloc and memcpy, and anything less would be a
 * placeholder for code with no reason to wait.
 *
 * BSTR rules the implementation honours (all published behaviour):
 *   - the length prefix counts BYTES, not characters (SysStringByteLen is
 *     the prefix itself; SysStringLen is half of it);
 *   - embedded NULs are data (SysAllocStringLen takes an explicit count);
 *   - SysAllocString(NULL) returns NULL; SysFreeString(NULL) is a no-op;
 *   - every BSTR carries a trailing NUL past its length, so a BSTR with no
 *     embedded NULs also reads as a plain C string.
 *
 * W32A-11 extends VARIANT to scalar, BYREF and IUnknown/IDispatch
 * reference-counted arms. Arrays, records and decimal stay fail-clean;
 * silently clearing them would corrupt ownership.
 */

#include "w32/oleaut32.h"

#ifndef AURALITE_W32_HOST_TEST
#include <stdlib.h>
#else
#include <stdlib.h>
#endif
#include <string.h>

/* The length prefix sits 4 bytes before the pointer the caller sees.  All
 * access is byte-wise: the prefix is unaligned by construction on some
 * allocators, and a cast would be a latent alignment fault. */
static uint32_t prefix_get(const W32_BSTR b) {
    const uint8_t *p = (const uint8_t *)b - 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void prefix_set(W32_BSTR b, uint32_t v) {
    uint8_t *p = (uint8_t *)b - 4;
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static unsigned ole_strlen(const W32_OLECHAR *s) {
    unsigned n = 0;
    if (s) while (s[n]) n++;
    return n;
}

W32ABI W32_BSTR SysAllocString(const W32_OLECHAR *psz) {
    if (!psz) return 0;
    return SysAllocStringLen(psz, ole_strlen(psz));
}

W32ABI W32_BSTR SysAllocStringLen(const W32_OLECHAR *pch, unsigned cch) {
    /* Overflow guard: cch * 2 + 6 must fit.  A 2 GB string request is a bug
     * or an attack, not a string. */
    if (cch > 0x3FFFFFFBu) return 0;
    uint8_t *raw = malloc((size_t)cch * 2 + 6);
    if (!raw) return 0;
    W32_BSTR b = (W32_BSTR)(raw + 4);
    prefix_set(b, cch * 2u);
    if (pch && cch) {
        for (unsigned i = 0; i < cch; i++) b[i] = pch[i];
    } else if (cch) {
        for (unsigned i = 0; i < cch; i++) b[i] = 0;
    }
    b[cch] = 0;
    return b;
}

W32ABI W32_BSTR SysAllocStringByteLen(const char *psz, unsigned len) {
    if (len > 0xFFFFFFF9u) return 0;
    uint8_t *raw = malloc((size_t)len + 6);
    if (!raw) return 0;
    W32_BSTR b = (W32_BSTR)(raw + 4);
    prefix_set(b, len);
    if (psz && len) {
        for (unsigned i = 0; i < len; i++) raw[4 + i] = (uint8_t)psz[i];
    } else if (len) {
        for (unsigned i = 0; i < len; i++) raw[4 + i] = 0;
    }
    raw[4 + len] = 0;
    raw[4 + len + 1] = 0;
    return b;
}

W32ABI void SysFreeString(W32_BSTR bstr) {
    if (!bstr) return;
    free((uint8_t *)bstr - 4);
}

W32ABI unsigned SysStringLen(W32_BSTR bstr) {
    if (!bstr) return 0;
    return prefix_get(bstr) / 2u;
}

W32ABI unsigned SysStringByteLen(W32_BSTR bstr) {
    if (!bstr) return 0;
    return prefix_get(bstr);
}

/* Unsupported variants keep their exact bytes and ownership. BYREF
 * pointers are borrowed; IUnknown/IDispatch have AddRef/Release at 1/2. */
static int variant_supported(uint16_t vt) {
    if (vt & (uint16_t)~(W32_VT_BYREF | 0x0fffu)) return 0;
    switch (vt & 0x0fffu) {
    case W32_VT_EMPTY: case W32_VT_NULL: case W32_VT_I2: case W32_VT_I4:
    case W32_VT_R4: case W32_VT_R8: case W32_VT_CY: case W32_VT_DATE:
    case W32_VT_BSTR: case W32_VT_DISPATCH: case W32_VT_ERROR:
    case W32_VT_BOOL: case W32_VT_UNKNOWN: case W32_VT_UI1:
    case W32_VT_UI2: case W32_VT_UI4: case W32_VT_I8: case W32_VT_UI8:
    case W32_VT_INT: case W32_VT_UINT: return 1;
    default: return 0;
    }
}
typedef W32_DWORD (W32ABI *ole_ref_fn)(void *self);
static void ole_ref(void *obj, unsigned slot) {
    if (obj) {
        void **vtable = *(void ***)obj;
        if (vtable && vtable[slot]) ((ole_ref_fn)vtable[slot])(obj);
    }
}
W32ABI void VariantInit(W32_VARIANT *pvarg) {
    if (pvarg) memset(pvarg, 0, sizeof *pvarg);
}
W32ABI W32_DWORD VariantClear(W32_VARIANT *pvarg) {
    if (!pvarg) return W32_E_INVALIDARG;
    if (!variant_supported(pvarg->vt)) return W32_E_NOTIMPL;
    if (!(pvarg->vt & W32_VT_BYREF)) {
        if (pvarg->vt == W32_VT_BSTR) SysFreeString((W32_BSTR)pvarg->u.ptr);
        if (pvarg->vt == W32_VT_UNKNOWN || pvarg->vt == W32_VT_DISPATCH)
            ole_ref(pvarg->u.ptr, 2);
    }
    VariantInit(pvarg);
    return W32_S_OK;
}
W32ABI W32_DWORD VariantCopy(W32_VARIANT *dest, const W32_VARIANT *src) {
    if (!dest || !src) return W32_E_INVALIDARG;
    if (!variant_supported(src->vt) || !variant_supported(dest->vt))
        return W32_E_NOTIMPL;
    if (dest == src) return W32_S_OK;
    W32_VARIANT temp = *src;
    if (src->vt == W32_VT_BSTR && src->u.ptr) {
        /* Byte exact: an odd-byte BSTR would be truncated by WCHAR-count. */
        unsigned bytes = SysStringByteLen((W32_BSTR)src->u.ptr);
        temp.u.ptr = SysAllocStringByteLen((const char *)src->u.ptr, bytes);
        if (!temp.u.ptr) return W32_E_OUTOFMEMORY;
    } else if (src->vt == W32_VT_UNKNOWN || src->vt == W32_VT_DISPATCH) {
        ole_ref(src->u.ptr, 1);
    }
    W32_DWORD rc = VariantClear(dest);
    if (rc != W32_S_OK) {
        if (src->vt == W32_VT_BSTR) SysFreeString((W32_BSTR)temp.u.ptr);
        if (src->vt == W32_VT_UNKNOWN || src->vt == W32_VT_DISPATCH)
            ole_ref(temp.u.ptr, 2);
        return rc;
    }
    *dest = temp;
    return W32_S_OK;
}
