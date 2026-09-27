/* W32A-11 flat, compositor-palette theme subset. Signatures, DTTOPTS,
 * THEMESIZE and buffered-paint structures are published Win64 ABI facts;
 * unsupported effects/parts fail rather than feigning themed pixels.
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
#define W32_THEME_EP_EDITTEXT 1
#define W32_THEME_TABP_TABITEM 1
#define W32_THEME_PP_BAR 1
#define W32_THEME_PP_BARVERT 2
#define W32_THEME_PP_CHUNK 3
#define W32_THEME_PP_CHUNKVERT 4
#define W32_THEME_CP_DROPDOWNBUTTON 1
#define W32_THEME_TS_MIN 0
#define W32_THEME_TS_TRUE 1
#define W32_THEME_TS_DRAW 2
#define W32_THEME_TMT_FONT 210
#define W32_THEME_TMT_TRANSITIONDURATIONS 6000
#define W32_THEME_DTT_TEXTCOLOR 0x0001u
#define W32_THEME_DTT_CALCRECT 0x0200u
#define W32_THEME_ETDT_DISABLE 1u
#define W32_THEME_ETDT_ENABLE 2u
#define W32_THEME_ETDT_USETABTEXTURE 4u
#define W32_THEME_BPBF_COMPATIBLEBITMAP 0
#define W32_THEME_BPAS_NONE 0

typedef struct {
    W32_DWORD dwSize, dwFlags;
    W32_DWORD crText, crBorder, crShadow;
    int32_t iTextShadowType;
    W32_POINT ptShadowOffset;
    int32_t iBorderSize, iFontPropId, iColorPropId, iStateId;
    W32_BOOL fApplyOverlay;
    int32_t iGlowSize;
    void *pfnDrawTextCallback;
    W32_LPARAM lParam;
} W32_DTTOPTS;
typedef struct {
    W32_DWORD cbSize, dwFlags;
    const W32_RECT *prcExclude;
    const void *pBlendFunction;
} W32_BP_PAINTPARAMS;
typedef struct {
    W32_DWORD cbSize, dwFlags, style, dwDuration;
} W32_BP_ANIMATIONPARAMS;

W32ABI void *OpenThemeData(W32_HWND hwnd, const uint16_t *classes);
W32ABI W32_DWORD CloseThemeData(void *theme);
W32ABI W32_DWORD DrawThemeBackground(void *theme, W32_HDC hdc,
                                      int part, int state, const W32_RECT *rect,
                                      const W32_RECT *clip);
W32ABI W32_DWORD GetThemeBackgroundContentRect(void *theme, W32_HDC hdc,
                                                 int part, int state,
                                                 const W32_RECT *bounds,
                                                 W32_RECT *content);
W32ABI W32_DWORD DrawThemeTextEx(void *theme, W32_HDC hdc, int part, int state,
                                   const uint16_t *text, int32_t count,
                                   W32_DWORD flags, W32_RECT *rect,
                                   const W32_DTTOPTS *opts);
W32ABI W32_DWORD DrawThemeParentBackground(W32_HWND hwnd, W32_HDC hdc,
                                             W32_RECT *rect);
W32ABI W32_DWORD GetThemePartSize(void *theme, W32_HDC hdc, int part,
                                   int state, W32_RECT *bounds, int which,
                                   W32_SIZE *out);
W32ABI W32_DWORD GetThemeFont(void *theme, W32_HDC hdc, int part,
                               int state, int prop, W32_LOGFONTW *out);
W32ABI W32_DWORD GetThemeTransitionDuration(void *theme, int part, int from,
                                             int to, int prop,
                                             W32_DWORD *milliseconds);
W32ABI W32_DWORD SetWindowTheme(W32_HWND hwnd, const uint16_t *subapp,
                                 const uint16_t *class_list);
W32ABI W32_DWORD EnableThemeDialogTexture(W32_HWND hwnd, W32_DWORD flags);
void w32_theme_window_destroyed(W32_HWND hwnd);
W32ABI W32_DWORD BufferedPaintInit(void);
W32ABI W32_DWORD BufferedPaintUnInit(void);
W32ABI void *BeginBufferedAnimation(W32_HWND hwnd, W32_HDC target,
                                     const W32_RECT *rect, int format,
                                     const W32_BP_PAINTPARAMS *paint,
                                     const W32_BP_ANIMATIONPARAMS *animation,
                                     W32_HDC *from, W32_HDC *to);
W32ABI W32_DWORD EndBufferedAnimation(void *buffer, W32_BOOL update);
W32ABI W32_DWORD BufferedPaintStopAllAnimations(W32_HWND hwnd);
#endif
