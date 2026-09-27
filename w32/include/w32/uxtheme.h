/* W32A-11 limited UxTheme part table: v6 BUTTON / BP_PUSHBUTTON 1..5.
 * Other classes, parts, text, animation and buffered painting stay TODO.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef AURALITE_W32_UXTHEME_H
#define AURALITE_W32_UXTHEME_H
#include "w32_abi.h"
#include "user32.h"
#include "gdi32.h"
#define W32_THEME_S_OK 0u
#define W32_THEME_E_NOTIMPL 0x80004001u
#define W32_THEME_E_HANDLE 0x80070006u
#define W32_THEME_E_INVALIDARG 0x80070057u
#define W32_THEME_E_OUTOFMEMORY 0x8007000Eu
#define W32_THEME_BP_PUSHBUTTON 1
#define W32_THEME_PBS_NORMAL 1
#define W32_THEME_PBS_HOT 2
#define W32_THEME_PBS_PRESSED 3
#define W32_THEME_PBS_DISABLED 4
#define W32_THEME_PBS_DEFAULTED 5
W32ABI void *OpenThemeData(W32_HWND hwnd, const uint16_t *classes);
W32ABI W32_DWORD CloseThemeData(void *theme);
W32ABI W32_DWORD DrawThemeBackground(void *theme, W32_HDC hdc,
                                      int part, int state, const W32_RECT *rect,
                                      const W32_RECT *clip);
W32ABI W32_DWORD GetThemeBackgroundContentRect(void *theme, W32_HDC hdc,
                                                 int part, int state,
                                                 const W32_RECT *bounds,
                                                 W32_RECT *content);
#endif
