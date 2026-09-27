/* W32A-11: typed FAIL-CLEAN IME boundary; direct keyboard input still works.
 * No fake context, no composition engine, no buffer mutation.
 * SPDX-License-Identifier: Apache-2.0 */
#include "w32/imm32.h"
#include "w32/w32_errno.h"
#include <stddef.h>
#define UNSUPPORTED() w32_set_last_error(W32_ERROR_NOT_SUPPORTED)
W32ABI void *ImmGetContext(W32_HWND hwnd) {
    (void)hwnd; UNSUPPORTED(); return NULL;
}
W32ABI W32_BOOL ImmReleaseContext(W32_HWND hwnd, void *imc) {
    (void)hwnd; (void)imc; UNSUPPORTED(); return 0;
}
W32ABI int32_t ImmGetCompositionStringW(void *imc, W32_DWORD idx,
                                         void *buf, W32_DWORD len) {
    (void)imc; (void)idx; (void)buf; (void)len;
    UNSUPPORTED(); return 0; /* empty; buffer remains untouched */
}
W32ABI W32_BOOL ImmSetCompositionWindow(void *imc, const void *f) {
    (void)imc; (void)f; UNSUPPORTED(); return 0;
}
W32ABI W32_BOOL ImmSetCompositionFontA(void *imc, const void *f) {
    (void)imc; (void)f; UNSUPPORTED(); return 0;
}
W32ABI W32_BOOL ImmSetCompositionFontW(void *imc, const void *f) {
    (void)imc; (void)f; UNSUPPORTED(); return 0;
}
W32ABI W32_BOOL ImmSetCandidateWindow(void *imc, const void *f) {
    (void)imc; (void)f; UNSUPPORTED(); return 0;
}
W32ABI W32_BOOL ImmSetCompositionStringW(void *imc, W32_DWORD idx,
                                          const void *c, W32_DWORD cl,
                                          const void *r, W32_DWORD rl) {
    (void)imc; (void)idx; (void)c; (void)cl; (void)r; (void)rl;
    UNSUPPORTED(); return 0;
}
W32ABI void *ImmEscapeW(void *hkl, void *imc, W32_UINT sub, void *data) {
    (void)hkl; (void)imc; (void)sub; (void)data; UNSUPPORTED(); return NULL;
}
W32ABI W32_BOOL ImmNotifyIME(void *imc, W32_DWORD action,
                              W32_DWORD idx, W32_DWORD value) {
    (void)imc; (void)action; (void)idx; (void)value;
    UNSUPPORTED(); return 0;
}
