/* W32A-11: no IME engine. Typed, fail-clean direct-input contracts.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef AURALITE_W32_IMM32_H
#define AURALITE_W32_IMM32_H
#include "w32_abi.h"
#include "user32.h"

W32ABI void *ImmGetContext(W32_HWND hwnd);
W32ABI W32_BOOL ImmReleaseContext(W32_HWND hwnd, void *imc);
W32ABI int32_t ImmGetCompositionStringW(void *imc, W32_DWORD index,
                                         void *buffer, W32_DWORD bytes);
W32ABI W32_BOOL ImmSetCompositionWindow(void *imc, const void *form);
W32ABI W32_BOOL ImmSetCompositionFontA(void *imc, const void *font);
W32ABI W32_BOOL ImmSetCompositionFontW(void *imc, const void *font);
W32ABI W32_BOOL ImmSetCandidateWindow(void *imc, const void *form);
W32ABI W32_BOOL ImmSetCompositionStringW(void *imc, W32_DWORD index,
                                          const void *composition, W32_DWORD clen,
                                          const void *reading, W32_DWORD rlen);
W32ABI void *ImmEscapeW(void *hkl, void *imc, W32_UINT subfunction, void *data);
W32ABI W32_BOOL ImmNotifyIME(void *imc, W32_DWORD action,
                              W32_DWORD index, W32_DWORD value);
#endif
