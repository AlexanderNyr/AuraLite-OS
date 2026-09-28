/* W32A-11 COM-lite partial: genuine per-thread balanced init and task heap.
 * No native TLS segment in AuraLite libc: bind depth to a stable Win32 TID.
 * CLSIDFromProgID/CoCreateInstance are REAL over the committed state: the
 * hive's HKCR view is the ProgID table, and the in-process activation table
 * holds zero factories because no pinned-app CLSID/IID pair was observed
 * (docs/w32app_receipts.md) -- requests are logged whole, never invented.
 * SPDX-License-Identifier: Apache-2.0 */
#include "w32/ole32.h"
#include "w32/kernel32.h"
#include "w32/advapi32.h"
#include "w32/w32_errno.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define W32_COM_MAX_THREADS 128
static struct { W32_DWORD id, co_depth, ole_depth; int used; }
    com_threads[W32_COM_MAX_THREADS];
static volatile int com_lock;
static void lock_com(void) {
    while (__sync_lock_test_and_set(&com_lock, 1))
        while (com_lock) __asm__ volatile("pause" ::: "memory");
}
static void unlock_com(void) { __sync_lock_release(&com_lock); }
static int find_thread(W32_DWORD id, int create) {
    int empty = -1;
    for (int i = 0; i < W32_COM_MAX_THREADS; ++i) {
        if (com_threads[i].used && com_threads[i].id == id) return i;
        if (!com_threads[i].used && empty < 0) empty = i;
    }
    if (!create || empty < 0) return -1;
    com_threads[empty].id = id;
    com_threads[empty].co_depth = 0;
    com_threads[empty].ole_depth = 0;
    com_threads[empty].used = 1;
    return empty;
}
W32ABI W32_DWORD CoInitialize(void *reserved) {
    if (reserved) return W32_COM_E_INVALIDARG;
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 1);
    if (slot < 0 || com_threads[slot].co_depth == UINT32_MAX) {
        unlock_com(); return W32_COM_E_OUTOFMEMORY;
    }
    W32_DWORD rc = com_threads[slot].co_depth++ ? W32_COM_S_FALSE : W32_COM_S_OK;
    unlock_com(); return rc;
}
W32ABI void CoUninitialize(void) {
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 0);
    if (slot >= 0 && com_threads[slot].co_depth &&
        !--com_threads[slot].co_depth) {
        com_threads[slot].ole_depth = 0;
        com_threads[slot].used = 0;
    }
    unlock_com();
}
W32ABI W32_DWORD OleInitialize(void *reserved) {
    W32_DWORD rc = CoInitialize(reserved);
    if (rc != W32_COM_S_OK && rc != W32_COM_S_FALSE) return rc;
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 0);
    if (slot >= 0 && com_threads[slot].ole_depth != UINT32_MAX) {
        com_threads[slot].ole_depth++;
        unlock_com(); return rc;
    }
    unlock_com();
    CoUninitialize();
    return W32_COM_E_OUTOFMEMORY;
}
W32ABI void OleUninitialize(void) {
    int valid = 0;
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 0);
    if (slot >= 0 && com_threads[slot].ole_depth) {
        --com_threads[slot].ole_depth;
        valid = 1;
    }
    unlock_com();
    if (valid) CoUninitialize();
}
W32ABI void *CoTaskMemAlloc(size_t bytes) {
    void *ptr = malloc(bytes ? bytes : 1);
    if (!ptr) w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return ptr;
}
W32ABI void *CoTaskMemRealloc(void *ptr, size_t bytes) {
    if (!bytes) { free(ptr); return NULL; }
    void *next = realloc(ptr, bytes);
    if (!next) w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return next;
}
W32ABI void CoTaskMemFree(void *ptr) { free(ptr); }

/* Drop targets require an OleInitialize on this thread. No cross-process
 * marshalling/apartments are invented by this single-process personality. */
int w32_com_ole_ready(void) {
    int ready = 0;
    lock_com();
    int slot = find_thread(GetCurrentThreadId(), 0);
    if (slot >= 0 && com_threads[slot].ole_depth) ready = 1;
    unlock_com();
    return ready;
}

/* ---------------------------------------------------------------------------
 * W32A-11 activation: ProgID lookup and in-process class activation.
 *
 * What is REAL here and what is a named refusal:
 *   * CLSIDFromProgID is a genuine registry query against the W32A-9 hive:
 *     HKCR\<progid>\CLSID, default value, REG_SZ, strict "{...}" GUID text.
 *     The hive's HKCR view IS the committed ProgID table -- it is seeded
 *     empty and is managed through RegSetValueEx/RegCreateKeyEx; nothing is
 *     hardcoded per application and no class exists only in a comment.
 *   * CoCreateInstance has full argument/context vocabulary (E_POINTER,
 *     E_INVALIDARG, CLASS_E_NOAGGREGATION, context E_NOTIMPL) and looks up
 *     the committed in-process activation table.  The table holds ZERO
 *     factories: the pinned-app probe (docs/w32app_receipts.md) observed
 *     no CLSID/IID pair, so no row is defensible and every activation
 *     answers REGDB_E_CLASSNOTREG with the request logged whole.
 * Both keep per-invocation probe lines (w32a11-clsid-probe: /
 * w32a11-progid-probe:) so a future scripted app session still produces
 * the phase's runtime table -- now with its outcome, not instead of it.
 * ------------------------------------------------------------------------ */

/* Canonical GUID text, little-endian Data1/Data2/Data3 memory layout -- the
 * same order the generated probe printed (guest gate asserts the format). */
static void w32a11_guid_text(const W32_GUID *guid, char out[37]) {
    if (!guid) { strcpy(out, "(null)"); return; }
    const uint8_t *b = (const uint8_t *)guid;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[3],b[2],b[1],b[0],b[5],b[4],b[7],b[6],
             b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

/* Strict "{01234567-89ab-cdef-0123-456789abcdef}" -> W32_GUID.  Returns 1 on
 * success; any wrong brace, dash, non-hex digit or length is refused.  The
 * inverse of w32a11_guid_text: parse(text(g)) byte-equals g. */
static int w32a11_hex(uint16_t ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
static int w32a11_guid_parse(const uint16_t *text, W32_GUID *out) {
    static const int dash_at[4] = { 9, 14, 19, 24 };
    uint8_t nib[32];
    if (!text || text[0] != '{' || text[37] != '}' || text[38] != 0) return 0;
    for (int i = 0; i < 4; ++i)
        if (text[dash_at[i]] != '-') return 0;
    int n = 0;
    for (int i = 1; i < 37 && n < 32; ++i) {
        if (text[i] == '-') continue;
        int v = w32a11_hex(text[i]);
        if (v < 0) return 0;
        nib[n++] = (uint8_t)v;
    }
    if (n != 32) return 0;
    out->data1 = (uint32_t)(nib[0] << 4 | nib[1]) << 24 |
                 (uint32_t)(nib[2] << 4 | nib[3]) << 16 |
                 (uint32_t)(nib[4] << 4 | nib[5]) << 8 |
                 (uint32_t)(nib[6] << 4 | nib[7]);
    out->data2 = (uint16_t)((nib[8] << 4 | nib[9]) << 8 | (nib[10] << 4 | nib[11]));
    out->data3 = (uint16_t)((nib[12] << 4 | nib[13]) << 8 | (nib[14] << 4 | nib[15]));
    for (int i = 0; i < 8; ++i)
        out->data4[i] = (uint8_t)(nib[16 + 2 * i] << 4 | nib[17 + 2 * i]);
    return 1;
}

/* Log the UTF-16 code units, never a lossy ASCII '?' approximation of an
 * untrusted ProgID: identifier-safe ASCII stays readable, every other unit
 * (space, '=', newline included) is escaped so the line cannot be forged.
 * A bounded, explicitly marked truncation is NOT a complete observation.
 * Semantics byte-identical to the generated probe this replaces. */
#define W32A11_PROGID_LOG_UNITS 192u
static int w32a11_progid_escape(const uint16_t *id,
                                char out[W32A11_PROGID_LOG_UNITS * 6u + 1u]) {
    static const char hex[] = "0123456789abcdef";
    size_t i = 0, used = 0;
    if (!id) { strcpy(out, "(null)"); return 0; }
    while (i < W32A11_PROGID_LOG_UNITS && id[i]) {
        unsigned ch = id[i++];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-') {
            out[used++] = (char)ch;
        } else {
            out[used++] = 0x5c; out[used++] = 'u';
            out[used++] = hex[(ch >> 12) & 15u];
            out[used++] = hex[(ch >> 8) & 15u];
            out[used++] = hex[(ch >> 4) & 15u];
            out[used++] = hex[ch & 15u];
        }
    }
    out[used] = 0;
    return i == W32A11_PROGID_LOG_UNITS && id[i] != 0;
}

/* ProgID spelling bound: the HKCR key path must be found NUL-terminated
 * within 256 units.  Anything longer is a named refusal, not a silent
 * truncation against the registry. */
#define W32A11_PROGID_MAX_UNITS 256u

W32ABI W32_DWORD CLSIDFromProgID(const uint16_t *progid, W32_GUID *out) {
    if (out) memset(out, 0, sizeof *out);   /* fail-clean on every path */
    char escaped[W32A11_PROGID_LOG_UNITS * 6u + 1u];
    unsigned truncated = (unsigned)w32a11_progid_escape(progid, escaped);
    W32_DWORD result = W32_COM_S_OK;
    if (!progid) {
        result = W32_COM_E_INVALIDARG;
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else {
        size_t n = 0;
        while (n < W32A11_PROGID_MAX_UNITS && progid[n]) ++n;
        if (n == 0 || n == W32A11_PROGID_MAX_UNITS) {
            result = W32_COM_CO_E_CLASSSTRING;
            w32_set_last_error(W32_ERROR_INVALID_NAME);
        } else {
            W32_HKEY key = 0;
            /* HKCR\<progid> is the registered class key; its CLSID subkey's
             * default value carries the "{...}" GUID text (the documented
             * registry layout, the W32A-9 merge view supplies HKCR). */
            uint16_t subkey[W32A11_PROGID_MAX_UNITS + 7u];
            static const uint16_t leaf[7] = { '\\','C','L','S','I','D',0 };
            memcpy(subkey, progid, n * sizeof *subkey);
            memcpy(subkey + n, leaf, sizeof leaf);
            if (RegOpenKeyExW(W32_HKEY_CLASSES_ROOT, subkey, 0, 0, &key) != 0) {
                result = W32_COM_CO_E_CLASSSTRING;
                w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
            } else {
                uint16_t text[40];
                W32_DWORD type = 0, len = sizeof text;
                long rc = RegQueryValueExW(key, NULL, NULL, &type,
                                           (uint8_t *)text, &len);
                RegCloseKey(key);
                if (rc != 0 || type != W32_REG_SZ || len < 2 ||
                    len % 2 || len / 2 > 39 || text[len / 2 - 1] != 0 ||
                    !w32a11_guid_parse(text, out)) {
                    /* The value IS registered but is not a CLSID string:
                     * CO_E_CLASSSTRING's documented meaning. */
                    memset(out, 0, sizeof *out);
                    result = W32_COM_CO_E_CLASSSTRING;
                    w32_set_last_error(W32_ERROR_INVALID_DATA);
                }
            }
        }
    }
    printf("w32a11-progid-probe: UTF16=%s truncated=%u result=0x%08lx\n",
           escaped, truncated, (unsigned long)result);
    return result;
}

/* The committed in-process activation table: row schema {clsid, iid}, ZERO
 * rows.  tools/check_w32app_claims.py and the receipts pin the row count to
 * the pinned-app probe state (no observed pair).  An observed class adds a
 * row here -- the lookup below must not change shape when that happens. */
struct w32_com_class_row { W32_GUID clsid, iid; };
static const struct w32_com_class_row w32_com_class_table[1];
#define W32_COM_CLASS_COUNT 0u

W32ABI W32_DWORD CoCreateInstance(const W32_GUID *clsid, void *outer,
                                  W32_DWORD ctx, const W32_GUID *iid,
                                  void **out) {
    if (!out) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_COM_E_POINTER;
    }
    *out = NULL;                            /* fail-clean on every path */
    char class_id[37], interface_id[37];
    w32a11_guid_text(clsid, class_id);
    w32a11_guid_text(iid, interface_id);
    printf("w32a11-clsid-probe: CLSID=%s IID=%s CLSCTX=%u\n",
           class_id, interface_id, (unsigned)ctx);
    W32_DWORD result;
    if (!clsid || !iid) {
        result = W32_COM_E_INVALIDARG;
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else if (outer) {
        /* Aggregation is not served by any class here; say so by name. */
        result = W32_COM_CLASS_E_NOAGGREGATION;
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
    } else if (ctx == 0) {
        result = W32_COM_E_INVALIDARG;
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else if (ctx != W32_CLSCTX_INPROC_SERVER) {
        /* No out-of-process/local/remote COM server exists in this
         * personality; the context cookie is refused, never coerced. */
        result = W32_COM_E_NOTIMPL;
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
    } else {
        const struct w32_com_class_row *row = NULL;
        /* The count is a committed literal zero (pinned by the receipts);
         * reading it into a variable keeps -Wtype-limits silent without
         * hiding that the loop body is unreachable until the first
         * observed class lands a row. */
        unsigned rows = W32_COM_CLASS_COUNT;
        for (unsigned i = 0; i < rows; ++i) {
            const struct w32_com_class_row *cand = &w32_com_class_table[i];
            if (!memcmp(&cand->clsid, clsid, sizeof *clsid) &&
                !memcmp(&cand->iid, iid, sizeof *iid)) { row = cand; break; }
        }
        if (row) {
            /* Unreachable while the table is empty; the row's factory lands
             * with the first observed class, per the schema comment above. */
            result = W32_COM_E_NOTIMPL;
            w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
        } else {
            result = W32_COM_REGDB_E_CLASSNOTREG;
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        }
    }
    return result;
}
