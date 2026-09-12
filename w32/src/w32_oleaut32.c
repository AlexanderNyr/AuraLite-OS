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
 * VARIANT coverage is deliberately partial: VT_EMPTY and VT_BSTR are real,
 * every other type returns E_NOTIMPL.  Arrays, IDispatch and decimal need
 * W32A-11's OLE-lite, and a stub that pretends to clear them would corrupt
 * the caller's memory -- the one thing a memory function must not do.
 */

#include "w32/oleaut32.h"

#ifndef AURALITE_W32_HOST_TEST
#include <stdlib.h>
#else
#include <stdlib.h>
#endif

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

W32ABI W32_DWORD VariantClear(W32_VARIANT *pvarg) {
    if (!pvarg) return W32_E_NOTIMPL;
    if (pvarg->vt == W32_VT_EMPTY) return W32_S_OK;
    if (pvarg->vt == W32_VT_BSTR) {
        SysFreeString((W32_BSTR)pvarg->u.ptr);
        pvarg->vt = W32_VT_EMPTY;
        pvarg->u.ptr = 0;
        return W32_S_OK;
    }
    /* Any other type needs W32A-11.  E_NOTIMPL, not a half-clear that would
     * leak or double-free: see the file header. */
    return W32_E_NOTIMPL;
}

W32ABI W32_DWORD VariantCopy(W32_VARIANT *dest, const W32_VARIANT *src) {
    const W32_OLECHAR *s;
    W32_BSTR fresh;
    if (!dest || !src) return W32_E_NOTIMPL;
    if (src->vt != W32_VT_EMPTY && src->vt != W32_VT_BSTR)
        return W32_E_NOTIMPL;
    /* A real VariantCopy clears the destination first, so copying over a
     * live BSTR does not leak it. */
    if (VariantClear(dest) != W32_S_OK) return W32_E_NOTIMPL;
    if (src->vt == W32_VT_EMPTY) return W32_S_OK;
    s = (const W32_OLECHAR *)src->u.ptr;
    /* Length-exact copy: embedded NULs survive, which a SysAllocString call
     * would not guarantee. */
    fresh = SysAllocStringLen(s, s ? prefix_get((W32_BSTR)s) / 2u : 0);
    if (src->u.ptr && !fresh) return W32_E_OUTOFMEMORY;
    dest->vt = W32_VT_BSTR;
    dest->u.ptr = fresh;
    return W32_S_OK;
}
