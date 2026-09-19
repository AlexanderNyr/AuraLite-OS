/* gdi32.h — the GDI32 personality surface (W32APP_PLAN.md W32A-7).
 * SPDX-License-Identifier: Apache-2.0 -- written for AuraLite OS.
 *
 * The DC model, the object table (pens/brushes/fonts/bitmaps/regions/
 * palettes/icons), the memory-DC raster engine, blitting with raster
 * ops, region clipping, font metrics against the shipped PSF2 font, the
 * 8-bit palette path, and the printing-adjacent calls as named
 * refusals (there are no printers -- D9 says name it, not fake it).
 *
 * Layout fidelity: every struct below matches its Win32 counterpart
 * field-for-field on x64 (they are caller-allocated buffers; a wrong
 * offset is memory corruption in the guest, not a wrong number).  The
 * base types (W32_HDC, W32_RECT, W32_POINT, W32ABI) come from
 * user32.h / w32_abi.h, same as the A-5/A-6 surface.
 *
 * Deliberate scope (D1 -- the import ledger decides, not taste):
 *   * No StretchBlt, no SetViewportOrgEx/GetMapMode/LPtoDP, no
 *     PtInRegion/EqualRgn/GetPaletteEntries/GetNearestPaletteIndex,
 *     no AbortDoc/GetFontLanguageInfo, no CreateDiscardableBitmap:
 *     none of them appear in the K/U/G import union.
 *   * Bitmap font only: the shipped PSF2 VGA 8x16 face is the one
 *     font; CreateFont* records the request (GetObject hands it back
 *     verbatim) and every metric reports the real 8x16 geometry --
 *     scaling is "nearest", which here means the single shipped size.
 */
#ifndef AURALITE_W32_GDI32_H
#define AURALITE_W32_GDI32_H

#include <stddef.h>

#include "w32/w32_abi.h"
#include "w32/user32.h"

/* ---- Handles ------------------------------------------------------------ */
typedef void *W32_HGDIOBJ;
typedef void *W32_HBITMAP;
typedef void *W32_HPEN;
typedef void *W32_HBRUSH;      /* user32.h already has it; re-typedef safe */
typedef void *W32_HFONT;
typedef void *W32_HRGN;
typedef void *W32_HPALETTE;

typedef struct { int32_t cx, cy; } W32_SIZE;

/* ---- Object types (GetCurrentObject) ------------------------------------ */
#define W32_OBJ_PEN       1
#define W32_OBJ_BRUSH     2
#define W32_OBJ_DC        3
#define W32_OBJ_METADC    4
#define W32_OBJ_PAL       5
#define W32_OBJ_FONT      6
#define W32_OBJ_BITMAP    7
#define W32_OBJ_REGION    8
#define W32_OBJ_ICON      12

/* ---- Stock objects -------------------------------------------------------- */
#define W32_WHITE_BRUSH        0
#define W32_LTGRAY_BRUSH       1
#define W32_GRAY_BRUSH         2
#define W32_DKGRAY_BRUSH       3
#define W32_BLACK_BRUSH        4
#define W32_NULL_BRUSH         5
#define W32_HOLLOW_BRUSH       W32_NULL_BRUSH
#define W32_WHITE_PEN          6
#define W32_BLACK_PEN          7
#define W32_NULL_PEN           8
#define W32_OEM_FIXED_FONT     10
#define W32_ANSI_FIXED_FONT    11
#define W32_ANSI_VAR_FONT      12
#define W32_SYSTEM_FONT        13
#define W32_DEVICE_DEFAULT_FONT 14
#define W32_DEFAULT_PALETTE    15
#define W32_SYSTEM_FIXED_FONT  16
#define W32_DEFAULT_GUI_FONT   17

/* ---- Raster ops (BitBlt/PatBlt) ------------------------------------------ */
#define W32_BLACKNESS     0x00000042u
#define W32_DSTINVERT     0x00550009u
#define W32_MERGEPAINT    0x00BB0226u
#define W32_NOTSRCCOPY    0x00330008u
#define W32_PATCOPY       0x00F00021u
#define W32_PATINVERT     0x005A0049u
#define W32_PATPAINT      0x00FB0A09u
#define W32_SRCAND        0x008800C6u
#define W32_SRCCOPY       0x00CC0020u
#define W32_SRCPAINT      0x00EE0086u
#define W32_SRCINVERT     0x00660046u
#define W32_WHITENESS     0x00FF0062u

/* ---- Pen / brush styles --------------------------------------------------- */
#define W32_PS_SOLID        0
#define W32_PS_DASH         1
#define W32_PS_DOT          2
#define W32_PS_DASHDOT      3
#define W32_PS_DASHDOTDOT   4
#define W32_PS_NULL         5
#define W32_PS_INSIDEFRAME  6
#define W32_PS_GEOMETRIC    0x00010000u
#define W32_PS_COSMETIC     0x00000000u

#define W32_HS_HORIZONTAL  0
#define W32_HS_VERTICAL    1
#define W32_HS_FDIAGONAL   2
#define W32_HS_BDIAGONAL   3
#define W32_HS_CROSS       4
#define W32_HS_DIAGCROSS   5

/* ---- Regions --------------------------------------------------------------- */
#define W32_RGN_AND  1
#define W32_RGN_OR   2
#define W32_RGN_XOR  3
#define W32_RGN_DIFF 4
#define W32_RGN_COPY 5
#define W32_RGN_ERROR   0
#define W32_NULLREGION  1
#define W32_SIMPLEREGION 2
#define W32_COMPLEXREGION 3

/* ---- Mapping modes / backgrounds / alignment -------------------------------- */
#define W32_MM_TEXT        1
#define W32_MM_LOMETRIC    2
#define W32_MM_HIMETRIC    3
#define W32_MM_LOENGLISH   4
#define W32_MM_HIENGLISH   5
#define W32_MM_TWIPS       6
#define W32_MM_ISOTROPIC   7
#define W32_MM_ANISOTROPIC 8

#define W32_TRANSPARENT 1
#define W32_OPAQUE      2

#define W32_TA_LEFT      0
#define W32_TA_RIGHT     2
#define W32_TA_CENTER    6
#define W32_TA_TOP       0
#define W32_TA_BOTTOM    8
#define W32_TA_BASELINE  24

#define W32_R2_COPYPEN   13

/* ---- GetDeviceCaps indices -------------------------------------------------- */
#define W32_DRIVERVERSION 0
#define W32_TECHNOLOGY    2
#define W32_DT_RASDISPLAY 1
#define W32_HORZSIZE       4
#define W32_VERTSIZE       6
#define W32_HORZRES       8
#define W32_VERTRES       10
#define W32_BITSPIXEL     12
#define W32_PLANES        14
#define W32_NUMBRUSHES    16
#define W32_NUMPENS       18
#define W32_NUMMARKERS    20
#define W32_NUMFONTS      22
#define W32_NUMCOLORS     24
#define W32_PDEVICESIZE   26
#define W32_CURVECAPS     28
#define W32_LINECAPS      30
#define W32_POLYGONALCAPS 32
#define W32_TEXTCAPS      34
#define W32_CLIPCAPS      36
#define W32_RASTERCAPS    38
#define W32_ASPECTX       40
#define W32_ASPECTY       42
#define W32_ASPECTXY      44
#define W32_LOGPIXELSX    88
#define W32_LOGPIXELSY    90
#define W32_SIZEPALETTE   104
#define W32_NUMRESERVED   106
#define W32_COLORRES      108
#define W32_PHYSICALWIDTH   110
#define W32_PHYSICALHEIGHT  111
#define W32_PHYSICALOFFSETX 112
#define W32_PHYSICALOFFSETY 113
#define W32_SCALINGFACTORX  114
#define W32_SCALINGFACTORY  115
#define W32_VREFRESH      116
#define W32_DESKTOPVERTRES 117
#define W32_DESKTOPHORZRES 118
#define W32_BLTALIGNMENT  119
#define W32_SHADEBLENDCAPS   120
#define W32_COLORMGMTCAPS    121

/* caps bits reported by the raster engine */
#define W32_TC_OP_CHARACTER  0x0001
#define W32_RC_BITBLT        0x0001
#define W32_RC_BITMAP64      0x0008
#define W32_RC_DI_BITMAP     0x0080
#define W32_CP_RECTANGLE     1
#define W32_SB_NONE          0
#define W32_CMGRAST_NONE     0

/* ---- DIB --------------------------------------------------------------------- */
#define W32_DIB_RGB_COLORS  0
#define W32_DIB_PAL_COLORS  1
#define W32_BI_RGB          0u

#define W32_ETO_OPAQUE   0x0002u
#define W32_ETO_CLIPPED  0x0004u

/* ---- Charsets ------------------------------------------------------------------ */
#define W32_ANSI_CHARSET    0
#define W32_DEFAULT_CHARSET 1
#define W32_OEM_CHARSET     255

#define W32_TCI_SRCCHARSET  1
#define W32_TCI_SRCCODEPAGE 2

/* ---- Icons ---------------------------------------------------------------------- */
#define W32_DI_MASK    1
#define W32_DI_IMAGE   2
#define W32_DI_NORMAL  3

/* ---- Misc ------------------------------------------------------------------------ */
#define W32_GDI_ERROR   0xFFFFFFFFu
#define W32_CLR_INVALID 0xFFFFFFFFu
#define W32_SP_ERROR    ((int)0xFFFFFFFF)
#define W32_GCP_MAXEXTENT 0x100000u

/* ===================================================================== */
/* Structures (Win32 layouts)                                            */
/* ===================================================================== */

typedef struct { uint8_t blue, green, red, reserved; } W32_RGBQUAD;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;          /* >0 bottom-up, <0 top-down */
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} W32_BITMAPINFOHEADER;

typedef struct {
    W32_BITMAPINFOHEADER bmiHeader;
    W32_RGBQUAD          bmiColors[256];
} W32_BITMAPINFO;

typedef struct {
    int32_t  bmType;
    int32_t  bmWidth;
    int32_t  bmHeight;
    int32_t  bmWidthBytes;
    uint16_t bmPlanes;
    uint16_t bmBitsPixel;
    void    *bmBits;
} W32_BITMAP;

typedef struct { uint8_t peRed, peGreen, peBlue, peFlags; } W32_PALETTEENTRY;

typedef struct {
    uint16_t         palVersion;
    uint16_t         palNumEntries;
    W32_PALETTEENTRY palPalEntry[1];
} W32_LOGPALETTE;

typedef struct {
    uint32_t lbStyle;
    uint32_t lbColor;           /* COLORREF */
    uint32_t lbHatch;           /* HS_* or pattern origin */
} W32_LOGBRUSH;

typedef struct {
    uint32_t  lopnStyle;
    W32_POINT lopnWidth;
    uint32_t  lopnColor;
} W32_LOGPEN;

typedef struct {
    int32_t  lfHeight;
    int32_t  lfWidth;
    int32_t  lfEscapement;
    int32_t  lfOrientation;
    int32_t  lfWeight;
    uint8_t  lfItalic, lfUnderline, lfStrikeOut;
    uint8_t  lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality;
    uint8_t  lfPitchAndFamily;
    uint16_t lfFaceName[32];
} W32_LOGFONTW;

typedef struct {
    int32_t  lfHeight;
    int32_t  lfWidth;
    int32_t  lfEscapement;
    int32_t  lfOrientation;
    int32_t  lfWeight;
    uint8_t  lfItalic, lfUnderline, lfStrikeOut;
    uint8_t  lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality;
    uint8_t  lfPitchAndFamily;
    char     lfFaceName[32];
} W32_LOGFONTA;

typedef struct {
    int32_t  tmHeight;
    int32_t  tmAscent;
    int32_t  tmDescent;
    int32_t  tmInternalLeading;
    int32_t  tmExternalLeading;
    int32_t  tmAveCharWidth;
    int32_t  tmMaxCharWidth;
    int32_t  tmWeight;
    int32_t  tmOverhang;
    int32_t  tmDigitizedAspectX;
    int32_t  tmDigitizedAspectY;
    uint16_t tmFirstChar;
    uint16_t tmLastChar;
    uint16_t tmDefaultChar;
    uint16_t tmBreakChar;
    uint8_t  tmItalic, tmUnderlined, tmStruckOut;
    uint8_t  tmPitchAndFamily;
    uint8_t  tmCharSet;
} W32_TEXTMETRICA;

typedef struct {
    int32_t  tmHeight;
    int32_t  tmAscent;
    int32_t  tmDescent;
    int32_t  tmInternalLeading;
    int32_t  tmExternalLeading;
    int32_t  tmAveCharWidth;
    int32_t  tmMaxCharWidth;
    int32_t  tmWeight;
    int32_t  tmOverhang;
    int32_t  tmDigitizedAspectX;
    int32_t  tmDigitizedAspectY;
    uint16_t tmFirstChar;
    uint16_t tmLastChar;
    uint16_t tmDefaultChar;
    uint16_t tmBreakChar;
    uint8_t  tmItalic, tmUnderlined, tmStruckOut;
    uint8_t  tmPitchAndFamily;
    uint8_t  tmCharSet;
    uint16_t tmPad;             /* Win32 packs W with 2-byte alignment */
} W32_TEXTMETRICW;

typedef struct { float abcfA, abcfB, abcfC; } W32_ABCFLOAT;

typedef struct {
    W32_LOGFONTW elfLogFont;
    uint16_t     elfFullName[64];
    uint16_t     elfStyle[32];
    uint16_t     elfScript[32];
} W32_ENUMLOGFONTEXW;

/* OUTLINETEXTMETRICA: the fixed part plus three name offsets, Win32
 * layout (strings live at otmFaceName/otmStyleName/otmFullName offsets
 * from the struct start; we place them right after the fixed part). */
typedef struct {
    uint32_t           otmSize;
    W32_TEXTMETRICA    otmTextMetrics;
    uint8_t            otmFiller;
    uint8_t            otmPanose[10];
    uint16_t           otmfsSelection;
    uint16_t           otmfsType;
    int32_t            otmsCharSlopeRise;
    int32_t            otmsCharSlopeRun;
    int32_t            otmEMSquare;
    int32_t            otmAscent;
    int32_t            otmDescent;
    uint32_t           otmLineGap;
    uint32_t           otmsCapEmHeight;
    uint32_t           otmsXHeight;
    W32_RECT           otmrcFontBox;
    int32_t            otmMacAscent;
    int32_t            otmMacDescent;
    uint32_t           otmMacLineGap;
    uint32_t           otmusMinimumPPEM;
    W32_POINT          otmptSubscriptSize;
    W32_POINT          otmptSubscriptOffset;
    W32_POINT          otmptSuperscriptSize;
    W32_POINT          otmptSuperscriptOffset;
    uint32_t           otmsStrikeoutSize;
    int32_t            otmsStrikeoutPosition;
    int32_t            otmsUnderlineSize;
    int32_t            otmsUnderlinePosition;
    uint32_t           otmFaceName;    /* offset from struct start */
    uint32_t           otmStyleName;
    uint32_t           otmFamilyName;
    uint32_t           otmFullName;
} W32_OUTLINETEXTMETRICA;

typedef struct {
    uint32_t  lStructSize;
    char     *lpClass;
    int32_t  *lpDx;
    int32_t  *lpCaretPos;
    void     *lpOutline;
    uint16_t *lpGlyphs;
    uint32_t  nGlyphs;
    int32_t   nMaxFit;
} W32_GCP_RESULTSW;

typedef struct {
    uint32_t ciCharset;
    uint32_t ciACP;
    uint32_t fsCsb[2];
    uint32_t fsUsb[2];
} W32_CHARSETINFO;

typedef struct {
    int32_t          cbSize;
    const uint16_t  *lpszDocName;
    const uint16_t  *lpszOutput;
    const uint16_t  *lpszDatatype;
    uint32_t         fwType;
} W32_DOCINFOW;

typedef struct {
    uint8_t BlendOp;
    uint8_t BlendFlags;
    uint8_t SourceConstantAlpha;
    uint8_t AlphaFormat;
} W32_BLENDFUNCTION;

#define W32_AC_SRC_OVER  0x00
#define W32_AC_SRC_ALPHA 0x01

/* ===================================================================== */
/* Prototypes                                                            */
/* ===================================================================== */

/* DC lifecycle and state */
W32ABI W32_HDC     CreateCompatibleDC(W32_HDC hdc);
W32ABI W32_BOOL    DeleteDC(W32_HDC hdc);
W32ABI int32_t     SaveDC(W32_HDC hdc);
W32ABI W32_BOOL    RestoreDC(W32_HDC hdc, int32_t saved);
W32ABI W32_HGDIOBJ SelectObject(W32_HDC hdc, W32_HGDIOBJ obj);
W32ABI W32_HGDIOBJ GetCurrentObject(W32_HDC hdc, W32_UINT type);
W32ABI W32_HGDIOBJ GetStockObject(int32_t idx);
/* the A-5 seven, moved from user32.h when W32A-7 gave them real DC state */
W32ABI W32_HBRUSH  CreateSolidBrush(W32_DWORD color);
W32ABI W32_BOOL    DeleteObject(void *obj);
W32ABI W32_BOOL    MoveToEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old);
W32ABI W32_BOOL    LineTo(W32_HDC hdc, int32_t x, int32_t y);
W32ABI W32_DWORD   SetPixel(W32_HDC hdc, int32_t x, int32_t y, W32_DWORD color);
W32ABI W32_DWORD   SetTextColor(W32_HDC hdc, W32_DWORD color);
W32ABI W32_BOOL    TextOutA(W32_HDC hdc, int32_t x, int32_t y,
                            const char *s, int32_t len);
W32ABI W32_BOOL    TextOutW(W32_HDC hdc, int32_t x, int32_t y,
                            const uint16_t *s, int32_t len);
W32ABI int32_t     GetObjectA(W32_HGDIOBJ obj, int32_t cb, void *buf);
W32ABI int32_t     GetObjectW(W32_HGDIOBJ obj, int32_t cb, void *buf);
W32ABI W32_BOOL    UnrealizeObject(W32_HGDIOBJ obj);
W32ABI W32_BOOL    DeleteObject(W32_HGDIOBJ obj);
W32ABI W32_UINT    GetBkMode(W32_HDC hdc);
W32ABI int32_t     SetBkMode(W32_HDC hdc, int32_t mode);
W32ABI W32_DWORD   SetBkColor(W32_HDC hdc, W32_DWORD color);
W32ABI W32_UINT    SetTextAlign(W32_HDC hdc, W32_UINT align);
W32ABI int32_t     SetROP2(W32_HDC hdc, int32_t mode);
W32ABI int32_t     GetROP2(W32_HDC hdc);
W32ABI int32_t     SetMapMode(W32_HDC hdc, int32_t mode);
W32ABI W32_BOOL    DPtoLP(W32_HDC hdc, W32_POINT *pts, int32_t n);
W32ABI W32_BOOL    SetWindowOrgEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old);
W32ABI W32_BOOL    OffsetWindowOrgEx(W32_HDC hdc, int32_t dx, int32_t dy, W32_POINT *old);
W32ABI W32_BOOL    SetBrushOrgEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old);
W32ABI int32_t     GetDeviceCaps(W32_HDC hdc, int32_t idx);
W32ABI W32_DWORD   GetPixel(W32_HDC hdc, int32_t x, int32_t y);
W32ABI W32_DWORD   SetPixel(W32_HDC hdc, int32_t x, int32_t y, W32_DWORD color);

/* Bitmaps and DIBs */
W32ABI W32_HBITMAP CreateCompatibleBitmap(W32_HDC hdc, int32_t w, int32_t h);
W32ABI W32_HBITMAP CreateBitmap(int32_t w, int32_t h, W32_UINT planes,
                                W32_UINT bpp, const void *bits);
W32ABI W32_HBITMAP CreateDIBSection(W32_HDC hdc, const W32_BITMAPINFO *bi,
                                    W32_UINT usage, void **bits,
                                    void *section, W32_DWORD offset);
W32ABI int32_t     GetDIBits(W32_HDC hdc, W32_HBITMAP bmp, W32_UINT start,
                             W32_UINT lines, void *bits, W32_BITMAPINFO *bi,
                             W32_UINT usage);
W32ABI int32_t     SetDIBits(W32_HDC hdc, W32_HBITMAP bmp, W32_UINT start,
                             W32_UINT lines, const void *bits,
                             const W32_BITMAPINFO *bi, W32_UINT usage);

/* Blitting */
W32ABI W32_BOOL    BitBlt(W32_HDC dst, int32_t x, int32_t y, int32_t w, int32_t h,
                          W32_HDC src, int32_t sx, int32_t sy, W32_DWORD rop);
W32ABI W32_BOOL    PatBlt(W32_HDC hdc, int32_t x, int32_t y, int32_t w, int32_t h,
                          W32_DWORD rop);
W32ABI W32_BOOL    GdiAlphaBlend(W32_HDC dst, int32_t x, int32_t y, int32_t w, int32_t h,
                                 W32_HDC src, int32_t sx, int32_t sy,
                                 int32_t sw, int32_t sh,
                                 W32_BLENDFUNCTION bf);

/* Pens, brushes */
W32ABI W32_HPEN    CreatePen(int32_t style, int32_t width, W32_DWORD color);
W32ABI W32_HPEN    ExtCreatePen(W32_DWORD style, W32_DWORD width,
                                const W32_LOGBRUSH *lb, W32_DWORD style_count,
                                const W32_DWORD *style_vals);
W32ABI W32_HBRUSH  CreateSolidBrush(W32_DWORD color);
W32ABI W32_HBRUSH  CreateHatchBrush(int32_t style, W32_DWORD color);
W32ABI W32_HBRUSH  CreatePatternBrush(W32_HBITMAP bmp);

/* Regions */
W32ABI W32_HRGN    CreateRectRgn(int32_t l, int32_t t, int32_t r, int32_t b);
W32ABI W32_HRGN    CreateRectRgnIndirect(const W32_RECT *r);
W32ABI int32_t     CombineRgn(W32_HRGN dst, W32_HRGN a, W32_HRGN b, int32_t mode);
W32ABI int32_t     SelectClipRgn(W32_HDC hdc, W32_HRGN rgn);
W32ABI int32_t     GetClipRgn(W32_HDC hdc, W32_HRGN rgn);
W32ABI int32_t     ExcludeClipRect(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b);
W32ABI int32_t     IntersectClipRect(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b);
W32ABI W32_BOOL    RectVisible(W32_HDC hdc, const W32_RECT *r);

/* Shapes */
W32ABI W32_BOOL    Rectangle(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b);
W32ABI W32_BOOL    Ellipse(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b);
W32ABI W32_BOOL    RoundRect(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b,
                             int32_t ew, int32_t eh);
W32ABI W32_BOOL    Polygon(W32_HDC hdc, const W32_POINT *pts, int32_t n);
W32ABI W32_BOOL    Polyline(W32_HDC hdc, const W32_POINT *pts, int32_t n);
W32ABI int32_t     FrameRect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush);

/* Fonts and text */
W32ABI W32_HFONT   CreateFontA(int32_t h, int32_t w, int32_t esc, int32_t ori,
                               int32_t weight, W32_DWORD italic, W32_DWORD underline,
                               W32_DWORD strikeout, W32_DWORD charset,
                               W32_DWORD outprec, W32_DWORD clipprec,
                               W32_DWORD quality, W32_DWORD pitch,
                               const char *face);
W32ABI W32_HFONT   CreateFontW(int32_t h, int32_t w, int32_t esc, int32_t ori,
                               int32_t weight, W32_DWORD italic, W32_DWORD underline,
                               W32_DWORD strikeout, W32_DWORD charset,
                               W32_DWORD outprec, W32_DWORD clipprec,
                               W32_DWORD quality, W32_DWORD pitch,
                               const uint16_t *face);
W32ABI W32_HFONT   CreateFontIndirectA(const W32_LOGFONTA *lf);
W32ABI W32_HFONT   CreateFontIndirectW(const W32_LOGFONTW *lf);
W32ABI W32_BOOL    GetTextMetricsA(W32_HDC hdc, W32_TEXTMETRICA *tm);
W32ABI W32_BOOL    GetTextMetricsW(W32_HDC hdc, W32_TEXTMETRICW *tm);
W32ABI W32_BOOL    GetTextExtentPoint32A(W32_HDC hdc, const char *s, int32_t len, W32_SIZE *sz);
W32ABI W32_BOOL    GetTextExtentPoint32W(W32_HDC hdc, const uint16_t *s, int32_t len, W32_SIZE *sz);
W32ABI W32_BOOL    GetTextExtentPointA(W32_HDC hdc, const char *s, int32_t len, W32_SIZE *sz);
W32ABI W32_BOOL    GetTextExtentPointW(W32_HDC hdc, const uint16_t *s, int32_t len, W32_SIZE *sz);
W32ABI W32_BOOL    GetTextExtentExPointA(W32_HDC hdc, const char *s, int32_t len,
                                         int32_t max_extent, int32_t *fit, int32_t *dx,
                                         W32_SIZE *sz);
W32ABI W32_BOOL    GetTextExtentExPointW(W32_HDC hdc, const uint16_t *s, int32_t len,
                                         int32_t max_extent, int32_t *fit, int32_t *dx,
                                         W32_SIZE *sz);
W32ABI W32_BOOL    GetCharWidthA(W32_HDC hdc, W32_UINT first, W32_UINT last, int32_t *buf);
W32ABI W32_BOOL    GetCharWidthW(W32_HDC hdc, W32_UINT first, W32_UINT last, int32_t *buf);
W32ABI W32_BOOL    GetCharWidth32A(W32_HDC hdc, W32_UINT first, W32_UINT last, int32_t *buf);
W32ABI W32_BOOL    GetCharWidth32W(W32_HDC hdc, W32_UINT first, W32_UINT last, int32_t *buf);
W32ABI W32_BOOL    GetCharABCWidthsFloatA(W32_HDC hdc, W32_UINT first, W32_UINT last,
                                          W32_ABCFLOAT *buf);
W32ABI W32_UINT    GetOutlineTextMetricsA(W32_HDC hdc, W32_UINT cb,
                                          W32_OUTLINETEXTMETRICA *otm);
W32ABI int32_t     EnumFontFamiliesExW(W32_HDC hdc, const W32_LOGFONTW *lf,
                                       void *proc, W32_LPARAM lparam, W32_DWORD flags);
W32ABI W32_DWORD   GetCharacterPlacementW(W32_HDC hdc, const uint16_t *s, int32_t n,
                                          int32_t max_ext, W32_GCP_RESULTSW *res,
                                          W32_DWORD flags);
W32ABI W32_BOOL    TranslateCharsetInfo(W32_DWORD *src, W32_CHARSETINFO *cs,
                                        W32_DWORD flags);
W32ABI W32_BOOL    ExtTextOutA(W32_HDC hdc, int32_t x, int32_t y, W32_UINT flags,
                               const W32_RECT *r, const char *s, W32_UINT len,
                               const int32_t *dx);
W32ABI W32_BOOL    ExtTextOutW(W32_HDC hdc, int32_t x, int32_t y, W32_UINT flags,
                               const W32_RECT *r, const uint16_t *s, W32_UINT len,
                               const int32_t *dx);
W32ABI W32_BOOL    TextOutA(W32_HDC hdc, int32_t x, int32_t y,
                            const char *s, int32_t len);
W32ABI W32_BOOL    TextOutW(W32_HDC hdc, int32_t x, int32_t y,
                            const uint16_t *s, int32_t len);

/* Palettes */
W32ABI W32_HPALETTE CreatePalette(const W32_LOGPALETTE *lp);
W32ABI W32_HPALETTE SelectPalette(W32_HDC hdc, W32_HPALETTE pal, W32_BOOL bkgnd);
W32ABI W32_UINT    RealizePalette(W32_HDC hdc);
W32ABI W32_UINT    SetPaletteEntries(W32_HPALETTE pal, W32_UINT start,
                                     W32_UINT count, const W32_PALETTEENTRY *ent);
W32ABI W32_BOOL    UpdateColors(W32_HDC hdc);

/* Printing (fail-clean: no printers exist) */
W32ABI int32_t     StartDocW(W32_HDC hdc, const W32_DOCINFOW *di);
W32ABI int32_t     EndDoc(W32_HDC hdc);
W32ABI int32_t     StartPage(W32_HDC hdc);
W32ABI int32_t     EndPage(W32_HDC hdc);

/* ---- internal exports (personality-internal, not bound) ---------------- */
/* Reset a window slot's common-DC state (user32.c's w32_win_make_dc). */
void w32_gdi_reset_win_dc(int win_index);
/* The metrics-only screen DC for GetDC(NULL). */
W32_HDC w32_gdi_screen_dc(void);
/* Decode an ICO/CUR blob into an icon object; 0 + named error on a blob
 * that is not an icon directory or image.  Used by w32_rsrc.c's loader. */
W32_HICON w32_gdi_icon_decode(const uint8_t *bytes, size_t len);
/* Register a resource blob the loader handed out (it alone knows the
 * size; DrawIconEx only gets the pointer). */
void w32_gdi_icon_cache_add(const uint8_t *blob, size_t len);
/* user32.c's DrawIconEx / FillRect draw through the raster engine. */
W32_BOOL w32_gdi_draw_icon(W32_HDC hdc, int32_t x, int32_t y, W32_HICON icon,
                           int32_t cx, int32_t cy);
W32_BOOL w32_gdi_fill_rect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush);
/* Brush colour decode for the A-5 raw-colour convention (class
 * backgrounds): table handle -> its colour, else the value itself. */
W32_DWORD w32_gdi_brush_color(W32_HBRUSH brush);
/* The loader records the manifest's dpiAware fact here (W32A-1 parsed,
 * W32A-7 consumes: unaware apps see LOGPIXELSX/Y = 96). */
void w32_gdi_set_dpi_aware(int aware);
W32_UINT w32_gdi_dpi_for(W32_HDC hdc);
/* Host-test hook: the object table's type for a handle, 0 if none. */
int w32_gdi_obj_type(void *h);

#endif /* AURALITE_W32_GDI32_H */
