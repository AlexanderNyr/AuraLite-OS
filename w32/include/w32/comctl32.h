/* comctl32.h — W32APP_PLAN.md phase W32A-8: the common-controls DLL.
 *
 * Everything in this header is an interface fact about the documented
 * Win32 common controls (message numbers, notification codes, struct
 * layouts), pinned from published documentation the same way the A-1
 * ordinal map was (see w32/PROVENANCE.md): the values below are the
 * documented constants, not copied code.  No Microsoft header text,
 * code, or binary is included.  The message/notification tables were
 * cross-checked against the published commctrl/prsht constant tables
 * (the MSDN message pages and a second independent published constant
 * mirror for the PSM_* block); every W32A-8 value matches the
 * documented numbering.
 *
 * Scope (decision D1): exactly the 27 ladder-measured symbols --
 * 21 named (InitCommonControlsEx, CreateToolbarEx, CreateStatusWindowW,
 * PropertySheetW, the ImageList_* set, _TrackMouseEvent) plus ordinals
 * 17 (InitCommonControls), 381 (LoadIconWithScaleDown) and 410-413
 * (SetWindowSubclass/GetWindowSubclass/RemoveWindowSubclass/
 * DefSubclassProc; w32/ordinal_map.tsv).  DllGetVersion and the
 * animation/hotkey/datetime/monthcalendar/ipaddress/pager/nativetext
 * controls are NOT in any ledger and are deliberately absent.
 *
 * The windowed controls are not imports: applications create them with
 * CreateWindowExW using the WC_* class names below (registered by
 * InitCommonControlsEx) and drive them with SendMessage -- the W32A-5
 * message path, end to end.  Notifications come back as WM_NOTIFY with
 * an NMHDR at lParam, exactly as documented.
 *
 * Versioning (W32A-1's manifest record): a v6 SxS dependency makes
 * w32run call w32_comctl_set_version(6) before the image starts; v5
 * (or no manifest) leaves the default.  v6 controls draw through the
 * theme palette (the UxTheme seam W32A-11 will back with real
 * uxtheme.dll exports); v5 controls draw unthemed, classic syscolors.
 * The integration gate asserts the two renderings differ, so the
 * selection is honoured, not parsed-and-ignored.
 */

#ifndef AURALITE_W32_COMCTL32_H
#define AURALITE_W32_COMCTL32_H

#include "w32_abi.h"
#include "user32.h"
#include "gdi32.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- handles ------------------------------------------------------- */

typedef void *W32_HIMAGELIST;
typedef void *W32_HPROPSHEETPAGE;

/* ---- the W32A-1 manifest record ------------------------------------- */

/* w32run calls this with mf.comctl_major before the image's first
 * import resolves.  Default is 5 (the no-manifest classic look). */
void w32_comctl_set_version(int major);
int  w32_comctl_version(void);

/* The theme palette every control paints through.  v6: the live
 * compositor theme (win_content / border / accent); v5: the classic
 * syscolor triple.  This is the seam W32A-11 replaces with real
 * OpenThemeData/DrawThemeBackground calls -- the structure is the
 * state those calls need (class + part), so the swap is mechanical. */
typedef struct {
    uint32_t face;      /* control background      */
    uint32_t text;      /* label text              */
    uint32_t frame;     /* border / separator      */
    uint32_t hot;       /* hovered chrome          */
    uint32_t sel;       /* selected item fill      */
} w32_comctl_palette_t;

const w32_comctl_palette_t *w32_comctl_palette(void);

/* ---- InitCommonControls --------------------------------------------- */

typedef struct {
    uint32_t dwSize;
    uint32_t dwICC;
} W32_INITCOMMONCONTROLSEX;

#define W32_ICC_LISTVIEW_CLASSES   0x00000001u
#define W32_ICC_TREEVIEW_CLASSES   0x00000002u
#define W32_ICC_BAR_CLASSES        0x00000004u
#define W32_ICC_TAB_CLASSES        0x00000008u
#define W32_ICC_UPDOWN_CLASS       0x00000010u
#define W32_ICC_PROGRESS_CLASS     0x00000020u
#define W32_ICC_HOTKEY_CLASS       0x00000040u
#define W32_ICC_ANIMATE_CLASS      0x00000080u
#define W32_ICC_WIN95_CLASSES      0x000000FFu
#define W32_ICC_DATE_CLASSES       0x00000100u
#define W32_ICC_USEREX_CLASSES     0x00000200u
#define W32_ICC_COOL_CLASSES       0x00000400u
#define W32_ICC_INTERNET_CLASSES   0x00000800u
#define W32_ICC_PAGESCROLLER_CLASS 0x00001000u
#define W32_ICC_NATIVEFNTCTL_CLASS 0x00002000u
#define W32_ICC_STANDARD_CLASSES   0x00004000u
#define W32_ICC_LINK_CLASS         0x00008000u

W32ABI void InitCommonControls(void);
W32ABI W32_BOOL InitCommonControlsEx(const W32_INITCOMMONCONTROLSEX *icc);

/* Ordinal 381: LoadIconWithScaleDown.  Loads the icon resource `name`
 * from `inst` and hands back an HICON scaled (nearest neighbour) to
 * cx x cy.  S_OK / a FAILED HRESULT. */
W32ABI int32_t LoadIconWithScaleDown(W32_HINSTANCE inst, const uint16_t *name,
                                     int32_t cx, int32_t cy, W32_HICON *out);

/* ---- common-notification header -------------------------------------- */

/* W32_NMHDR comes from user32.h (it predates this phase); the control
 * codes below extend it. */

#define W32_NM_FIRST       0u
#define W32_NM_OUTOFMEMORY (W32_NM_FIRST - 1u)   /* 0xFFFFFFFF */
#define W32_NM_CLICK       (W32_NM_FIRST - 2u)
#define W32_NM_DBLCLK      (W32_NM_FIRST - 3u)
#define W32_NM_RETURN      (W32_NM_FIRST - 4u)
#define W32_NM_RCLICK      (W32_NM_FIRST - 5u)
#define W32_NM_RDBLCLK     (W32_NM_FIRST - 6u)
#define W32_NM_SETFOCUS    (W32_NM_FIRST - 7u)
#define W32_NM_KILLFOCUS   (W32_NM_FIRST - 8u)

/* ---- the class names -------------------------------------------------- */

/* Registered by InitCommonControls(Ex); created with CreateWindowExW,
 * parented, driven by the TB_/SB_/LVM_/TVM_/TCM_/TTM_/PBM_/HDM_
 * messages below.  WS_CHILD is accepted for exactly these classes
 * (user32_win.c gates it on the comctl marker).
 *
 * The names are UTF-16 arrays defined in comctl32.c, NOT L"" literals:
 * this toolchain's wchar_t is 4 bytes (host gcc/clang and the guest
 * build alike -- only the UEFI build uses -fshort-wchar), so an L""
 * cast to uint16_t* would interleave NULs and truncate every name to
 * its first character.  Every name starts with a different letter only
 * by coincidence; the arrays keep them whole. */
extern const uint16_t W32_WCN_TOOLBAR[];
extern const uint16_t W32_WCN_STATUSBAR[];
extern const uint16_t W32_WCN_LISTVIEW[];
extern const uint16_t W32_WCN_TREEVIEW[];
extern const uint16_t W32_WCN_TABCONTROL[];
extern const uint16_t W32_WCN_TOOLTIP[];
extern const uint16_t W32_WCN_PROGRESS[];
extern const uint16_t W32_WCN_HEADER[];
#define W32_WC_TOOLBARW     W32_WCN_TOOLBAR
#define W32_WC_STATUSBARW   W32_WCN_STATUSBAR
#define W32_WC_LISTVIEWW    W32_WCN_LISTVIEW
#define W32_WC_TREEVIEWW    W32_WCN_TREEVIEW
#define W32_WC_TABCONTROLW  W32_WCN_TABCONTROL
#define W32_WC_TOOLTIPW     W32_WCN_TOOLTIP
#define W32_WC_PROGRESSW    W32_WCN_PROGRESS
#define W32_WC_HEADERW      W32_WCN_HEADER

/* ---- toolbar ---------------------------------------------------------- */

/* TBBUTTON (x64: 32 bytes -- fsState/fsStyle then 6 bytes of padding
 * before the two pointer-width fields, exactly the documented x64
 * layout; uStructSize callers pass sizeof). */
typedef struct {
    int32_t   iBitmap;      /* image index, -1 none           */
    int32_t   idCommand;    /* WM_COMMAND id                   */
    uint8_t   fsState;      /* W32_TBSTATE_*                   */
    uint8_t   fsStyle;      /* W32_TBSTYLE_*                   */
    uint8_t   _pad[6];
    uint64_t  dwData;
    int64_t   iString;      /* string pool index or LPCSTR     */
} W32_TBBUTTON;

#define W32_TBSTATE_ENABLED  0x04u
#define W32_TBSTATE_CHECKED  0x01u
#define W32_TBSTATE_PRESSED  0x02u
#define W32_TBSTATE_HIDDEN   0x08u

#define W32_TBSTYLE_BUTTON   0x0000u
#define W32_TBSTYLE_SEP      0x0001u
#define W32_TBSTYLE_CHECK    0x0002u
#define W32_TBSTYLE_CHECKGROUP (W32_TBSTYLE_CHECK | 0x0004u)

/* WM_USER-based, documented values. */
#define W32_TB_ENABLEBUTTON      (W32_WM_USER + 1)
#define W32_TB_CHECKBUTTON       (W32_WM_USER + 2)
#define W32_TB_PRESSBUTTON       (W32_WM_USER + 3)
#define W32_TB_HIDEBUTTON        (W32_WM_USER + 4)
#define W32_TB_ISBUTTONENABLED   (W32_WM_USER + 9)
#define W32_TB_ISBUTTONCHECKED   (W32_WM_USER + 10)
#define W32_TB_ISBUTTONPRESSED   (W32_WM_USER + 11)
#define W32_TB_ISBUTTONHIDDEN    (W32_WM_USER + 12)
#define W32_TB_SETSTATE          (W32_WM_USER + 17)
#define W32_TB_GETSTATE          (W32_WM_USER + 18)
#define W32_TB_ADDBUTTONSA       (W32_WM_USER + 20)
#define W32_TB_INSERTBUTTONA     (W32_WM_USER + 21)
#define W32_TB_DELETEBUTTON      (W32_WM_USER + 22)
#define W32_TB_GETBUTTON         (W32_WM_USER + 23)
#define W32_TB_BUTTONCOUNT       (W32_WM_USER + 24)
#define W32_TB_COMMANDTOINDEX    (W32_WM_USER + 25)
#define W32_TB_CUSTOMIZE         (W32_WM_USER + 27)   /* refused: no customisation */
#define W32_TB_GETITEMRECT       (W32_WM_USER + 29)
#define W32_TB_BUTTONSTRUCTSIZE  (W32_WM_USER + 30)
#define W32_TB_SETIMAGELIST      (W32_WM_USER + 48)
#define W32_TB_GETIMAGELIST      (W32_WM_USER + 49)
#define W32_TB_GETBUTTONSIZE     (W32_WM_USER + 58)
#define W32_TB_SETBUTTONSIZE     (W32_WM_USER + 31)
#define W32_TB_SETBITMAPSIZE     (W32_WM_USER + 32)
#define W32_TB_ADDBUTTONSW       (W32_WM_USER + 68)
#define W32_TB_INSERTBUTTONW     (W32_WM_USER + 67)

#define W32_TBN_FIRST   (0u - 700u)
#define W32_TBN_QUERYINSERT (W32_TBN_FIRST - 5)   /* customisation: not sent */

W32ABI W32_HWND CreateToolbarEx(W32_HWND hwnd, W32_DWORD ws, W32_UINT wID,
                                int32_t nBitmaps, W32_HINSTANCE hBMInst,
                                uint64_t wBMID, const W32_TBBUTTON *buttons,
                                int32_t nButtons, int32_t dxButton,
                                int32_t dyButton, int32_t dxBitmap,
                                int32_t dyBitmap, W32_UINT uStructSize);

/* ---- status bar -------------------------------------------------------- */

#define W32_SB_SETTEXTA        (W32_WM_USER + 1)
#define W32_SB_SETTEXTW        (W32_WM_USER + 11)
#define W32_SB_GETTEXTA        (W32_WM_USER + 2)
#define W32_SB_GETTEXTW        (W32_WM_USER + 13)
#define W32_SB_GETTEXTLENGTHA  (W32_WM_USER + 3)
#define W32_SB_GETTEXTLENGTHW  (W32_WM_USER + 12)
#define W32_SB_SETPARTS        (W32_WM_USER + 4)
#define W32_SB_GETPARTS        (W32_WM_USER + 6)
#define W32_SB_GETBORDERS      (W32_WM_USER + 7)
#define W32_SB_SETMINHEIGHT    (W32_WM_USER + 8)
#define W32_SB_SIMPLE          (W32_WM_USER + 9)
#define W32_SB_GETRECT         (W32_WM_USER + 10)

/* SB_SETTEXT/GETTEXT low byte of wParam is the part index; the drawing
 * operation bits above it are documented but only SBT_NOBORDERS is
 * honoured here (drawn without the part frame). */
#define W32_SBT_NOBORDERS      0x0100u

W32ABI W32_HWND CreateStatusWindowW(int32_t style, const uint16_t *text,
                                    W32_HWND parent, W32_UINT id);

/* ---- listview ------------------------------------------------------------ */

#define W32_LVM_FIRST 0x1000u
#define W32_LVM_GETITEMCOUNT     (W32_LVM_FIRST + 4)
#define W32_LVM_INSERTITEMA      (W32_LVM_FIRST + 7)
#define W32_LVM_INSERTITEMW      (W32_LVM_FIRST + 77)
#define W32_LVM_DELETEITEM       (W32_LVM_FIRST + 8)
#define W32_LVM_DELETEALLITEMS   (W32_LVM_FIRST + 9)
#define W32_LVM_GETITEMA         (W32_LVM_FIRST + 5)
#define W32_LVM_GETITEMW         (W32_LVM_FIRST + 75)
#define W32_LVM_SETITEMA         (W32_LVM_FIRST + 6)
#define W32_LVM_SETITEMW         (W32_LVM_FIRST + 76)
#define W32_LVM_GETITEMTEXTA     (W32_LVM_FIRST + 45)
#define W32_LVM_GETITEMTEXTW     (W32_LVM_FIRST + 115)
#define W32_LVM_GETITEMSTATE     (W32_LVM_FIRST + 44)
#define W32_LVM_SETITEMSTATE     (W32_LVM_FIRST + 47)
#define W32_LVM_GETNEXTITEM      (W32_LVM_FIRST + 12)
#define W32_LVM_GETSELECTEDCOUNT (W32_LVM_FIRST + 50)
#define W32_LVM_GETSELECTIONMARK (W32_LVM_FIRST + 66)
#define W32_LVM_SETSELECTIONMARK (W32_LVM_FIRST + 67)
#define W32_LVM_INSERTCOLUMNA    (W32_LVM_FIRST + 27)
#define W32_LVM_INSERTCOLUMNW    (W32_LVM_FIRST + 97)
#define W32_LVM_DELETECOLUMN     (W32_LVM_FIRST + 28)
#define W32_LVM_GETCOLUMNA       (W32_LVM_FIRST + 25)
#define W32_LVM_GETCOLUMNW       (W32_LVM_FIRST + 95)
#define W32_LVM_ENSUREVISIBLE    (W32_LVM_FIRST + 19)
#define W32_LVM_HITTEST          (W32_LVM_FIRST + 18)
#define W32_LVM_REDRAWITEMS      (W32_LVM_FIRST + 21)
#define W32_LVM_SETEXTENDEDLISTVIEWSTYLE (W32_LVM_FIRST + 54)

/* LVITEMW (x64): the pointer forces alignment padding after stateMask. */
typedef struct {
    uint32_t     mask;         /* W32_LVIF_*                    */
    int32_t      iItem;
    int32_t      iSubItem;
    uint32_t     state;        /* W32_LVIS_*                    */
    uint32_t     stateMask;
    const uint16_t *pszText;   /* in: text; out: buffer          */
    int32_t      cchTextMax;
    int32_t      iImage;
    int64_t      lParam;
    int32_t      iIndent;
} W32_LVITEMW;

typedef struct {
    uint32_t     mask;         /* W32_LVCF_*                    */
    int32_t      fmt;          /* W32_LVCFMT_*                  */
    int32_t      cx;           /* pixel width                   */
    const uint16_t *pszText;
    int32_t      cchTextMax;
    int32_t      iSubItem;
} W32_LVCOLUMNW;

typedef struct {
    W32_NMHDR    hdr;
    int32_t      iItem;
    int32_t      iSubItem;
    uint32_t     uNewState;
    uint32_t     uOldState;
    uint32_t     uChanged;     /* W32_LVIF_* of what changed    */
    int64_t      lParam;
} W32_NMLISTVIEW;

typedef struct {
    W32_POINT    pt;
    uint32_t     flags;        /* out: W32_LVHT_*               */
    int32_t      iItem;
} W32_LVHITTESTINFO;

#define W32_LVIF_TEXT       0x0001u
#define W32_LVIF_IMAGE      0x0002u
#define W32_LVIF_PARAM      0x0004u
#define W32_LVIF_STATE      0x0008u
#define W32_LVIF_INDENT     0x0010u

#define W32_LVIS_FOCUSED        0x0001u
#define W32_LVIS_SELECTED       0x0002u
#define W32_LVIS_CUT            0x0004u
#define W32_LVIS_DROPHILITED    0x0008u

#define W32_LVNI_SELECTED       0x0002u

#define W32_LVCF_FMT    0x0001u
#define W32_LVCF_WIDTH  0x0002u
#define W32_LVCF_TEXT   0x0004u
#define W32_LVCF_SUBITEM 0x0008u

#define W32_LVCFMT_LEFT 0x0000u

#define W32_LVHT_ONITEMICON  0x0002u
#define W32_LVHT_ONITEMLABEL 0x0004u
#define W32_LVHT_ONITEM      (W32_LVHT_ONITEMICON | W32_LVHT_ONITEMLABEL)

/* Styles (the LVS_* bits ride in CreateWindowExW's style). */
#define W32_LVS_ICON           0x0000u
#define W32_LVS_REPORT         0x0001u
#define W32_LVS_SMALLICON      0x0002u
#define W32_LVS_LIST           0x0003u
#define W32_LVS_SINGLESEL      0x0004u
#define W32_LVS_SHOWSELALWAYS  0x0008u
#define W32_LVS_NOSORTHEADER   0x8000u

#define W32_LVN_FIRST (0u - 100u)
#define W32_LVN_ITEMCHANGING (W32_LVN_FIRST - 0u)
#define W32_LVN_ITEMCHANGED  (W32_LVN_FIRST - 1u)
#define W32_LVN_INSERTITEM   (W32_LVN_FIRST - 2u)
#define W32_LVN_DELETEITEM   (W32_LVN_FIRST - 3u)

/* ---- treeview ------------------------------------------------------------ */

#define W32_TVM_FIRST 0x1100u
#define W32_TVM_INSERTITEMA  (W32_TVM_FIRST + 0)
#define W32_TVM_INSERTITEMW  (W32_TVM_FIRST + 50)
#define W32_TVM_DELETEITEM   (W32_TVM_FIRST + 1)
#define W32_TVM_EXPAND       (W32_TVM_FIRST + 2)
#define W32_TVM_GETITEMRECT  (W32_TVM_FIRST + 4)
#define W32_TVM_GETCOUNT     (W32_TVM_FIRST + 5)
#define W32_TVM_GETNEXTITEM  (W32_TVM_FIRST + 10)
#define W32_TVM_SELECTITEM   (W32_TVM_FIRST + 11)
#define W32_TVM_GETITEMA     (W32_TVM_FIRST + 12)
#define W32_TVM_GETITEMW     (W32_TVM_FIRST + 62)
#define W32_TVM_SETITEMA     (W32_TVM_FIRST + 13)
#define W32_TVM_SETITEMW     (W32_TVM_FIRST + 63)
#define W32_TVM_HITTEST      (W32_TVM_FIRST + 17)
#define W32_TVM_ENSUREVISIBLE   (W32_TVM_FIRST + 20)
#define W32_TVM_GETVISIBLECOUNT (W32_TVM_FIRST + 16)

/* TVITEMW (x64): pointers force padding after mask/state/stateMask. */
typedef struct {
    uint32_t     mask;         /* W32_TVIF_*                    */
    uint32_t     _pad0;
    W32_HWND     hItem;        /* HTREEITEM (an opaque handle)  */
    uint32_t     state;        /* W32_TVIS_*                    */
    uint32_t     stateMask;
    uint32_t     _pad1;
    uint16_t    *pszText;      /* in: text; out: buffer         */
    int32_t      cchTextMax;
    int32_t      iImage;
    int32_t      iSelectedImage;
    int32_t      cChildren;
    uint32_t     _pad2;
    int64_t      lParam;
} W32_TVITEMW;

typedef struct {
    W32_HWND     hParent;      /* TVI_ROOT for a root item      */
    W32_HWND     hInsertAfter; /* TVI_FIRST/LAST/SORT           */
    W32_TVITEMW  item;
} W32_TVINSERTSTRUCTW;

typedef struct {
    W32_NMHDR    hdr;
    uint32_t     action;
    uint32_t     _pad;
    W32_TVITEMW  itemOld;
    W32_TVITEMW  itemNew;
    W32_POINT    ptDrag;
} W32_NMTREEVIEWW;

typedef struct {
    W32_POINT    pt;
    uint32_t     flags;        /* out: W32_TVHT_*               */
    W32_HWND     hItem;
} W32_TVHITTESTINFO;

#define W32_TVIF_TEXT          0x0001u
#define W32_TVIF_CHILDREN      0x0040u
#define W32_TVIF_SELECTEDIMAGE 0x0020u
#define W32_TVIF_STATE         0x0008u
#define W32_TVIF_PARAM         0x0004u

#define W32_TVIS_SELECTED      0x0002u
#define W32_TVIS_EXPANDED      0x0020u
#define W32_TVIS_EXPANDEDONCE  0x0040u

/* HTREEITEM sentinels (documented). */
#define W32_TVI_ROOT   ((W32_HWND)(uintptr_t)0xFFFF0000ull)
#define W32_TVI_FIRST  ((W32_HWND)(uintptr_t)0xFFFF0001ull)
#define W32_TVI_LAST   ((W32_HWND)(uintptr_t)0xFFFF0002ull)
#define W32_TVI_SORT   ((W32_HWND)(uintptr_t)0xFFFF0003ull)

/* TVM_EXPAND wParam. */
#define W32_TVE_COLLAPSE 0x0001u
#define W32_TVE_EXPAND   0x0002u
#define W32_TVE_TOGGLE   0x0003u

/* TVM_GETNEXTITEM wParam. */
#define W32_TVGN_ROOT          0x0000u
#define W32_TVGN_NEXT          0x0001u
#define W32_TVGN_PREVIOUS      0x0002u
#define W32_TVGN_PARENT        0x0003u
#define W32_TVGN_CHILD         0x0004u
#define W32_TVGN_FIRSTVISIBLE  0x0005u
#define W32_TVGN_NEXTVISIBLE   0x0006u
#define W32_TVGN_PREVIOUSVISIBLE 0x0007u
#define W32_TVGN_CARET         0x0009u

#define W32_TVHT_ONITEMICON   0x0002u
#define W32_TVHT_ONITEMLABEL  0x0004u
#define W32_TVHT_ONITEM       (W32_TVHT_ONITEMICON | W32_TVHT_ONITEMLABEL)

/* Styles. */
#define W32_TVS_HASLINES      0x0002u
#define W32_TVS_HASBUTTONS    0x0001u
#define W32_TVS_LINESATROOT   0x0004u
#define W32_TVS_SHOWSELALWAYS 0x0020u

#define W32_TVN_FIRST (0u - 400u)
#define W32_TVN_SELCHANGINGA (W32_TVN_FIRST - 1u)
#define W32_TVN_SELCHANGEDA  (W32_TVN_FIRST - 2u)
#define W32_TVN_SELCHANGINGW (W32_TVN_FIRST - 50u)
#define W32_TVN_SELCHANGEDW  (W32_TVN_FIRST - 51u)
#define W32_TVN_ITEMEXPANDINGA (W32_TVN_FIRST - 5u)
#define W32_TVN_ITEMEXPANDEDA  (W32_TVN_FIRST - 6u)
#define W32_TVN_ITEMEXPANDINGW (W32_TVN_FIRST - 54u)
#define W32_TVN_ITEMEXPANDEDW  (W32_TVN_FIRST - 55u)

/* ---- tab control ---------------------------------------------------------- */

#define W32_TCM_FIRST 0x1300u
#define W32_TCM_GETITEMCOUNT (W32_TCM_FIRST + 4)
#define W32_TCM_GETITEMA     (W32_TCM_FIRST + 5)
#define W32_TCM_GETITEMW     (W32_TCM_FIRST + 60)
#define W32_TCM_SETITEMA     (W32_TCM_FIRST + 6)
#define W32_TCM_SETITEMW     (W32_TCM_FIRST + 61)
#define W32_TCM_INSERTITEMA  (W32_TCM_FIRST + 7)
#define W32_TCM_INSERTITEMW  (W32_TCM_FIRST + 62)
#define W32_TCM_DELETEITEM   (W32_TCM_FIRST + 8)
#define W32_TCM_DELETEALLITEMS (W32_TCM_FIRST + 9)
#define W32_TCM_GETITEMRECT  (W32_TCM_FIRST + 10)
#define W32_TCM_GETCURSEL    (W32_TCM_FIRST + 11)
#define W32_TCM_SETCURSEL    (W32_TCM_FIRST + 12)
#define W32_TCM_GETCURFOCUS  (W32_TCM_FIRST + 47)
#define W32_TCM_SETCURFOCUS  (W32_TCM_FIRST + 48)
#define W32_TCM_ADJUSTRECT   (W32_TCM_FIRST + 40)

typedef struct {
    uint32_t     mask;         /* W32_TCIF_*                    */
    uint32_t     dwState;      /* in/out (reserved 0 here)      */
    uint32_t     dwStateMask;
    uint32_t     _pad0;
    uint16_t    *pszText;
    int32_t      cchTextMax;
    int32_t      iImage;
    uint32_t     _pad1;
    int64_t      lParam;
} W32_TCITEMW;

#define W32_TCIF_TEXT       0x0001u
#define W32_TCIF_IMAGE      0x0002u
#define W32_TCIF_RTLREADING 0x0004u
#define W32_TCIF_PARAM      0x0008u

#define W32_TCS_TABS       0x0000u
#define W32_TCS_BUTTONS    0x0100u
#define W32_TCS_MULTILINE  0x0200u

#define W32_TCN_FIRST (0u - 550u)
#define W32_TCN_SELCHANGE   (W32_TCN_FIRST - 1u)
#define W32_TCN_SELCHANGING (W32_TCN_FIRST - 2u)

/* ---- tooltips ---------------------------------------------------------------- */

/* TTM_* sit at WM_USER themselves (documented), not WM_USER+n from a
 * separate TTM_FIRST. */
#define W32_TTM_ACTIVATE          (W32_WM_USER + 1)
#define W32_TTM_SETDELAYTIME      (W32_WM_USER + 3)
#define W32_TTM_ADDTOOLA          (W32_WM_USER + 4)
#define W32_TTM_ADDTOOLW          (W32_WM_USER + 50)
#define W32_TTM_DELTOOLA          (W32_WM_USER + 5)
#define W32_TTM_DELTOOLW          (W32_WM_USER + 51)
#define W32_TTM_NEWTOOLRECTA      (W32_WM_USER + 6)
#define W32_TTM_NEWTOOLRECTW      (W32_WM_USER + 52)
#define W32_TTM_RELAYEVENT        (W32_WM_USER + 7)
#define W32_TTM_GETTOOLINFOA      (W32_WM_USER + 8)
#define W32_TTM_GETTOOLINFOW      (W32_WM_USER + 53)
#define W32_TTM_HITTESTA          (W32_WM_USER + 10)
#define W32_TTM_HITTESTW          (W32_WM_USER + 55)
#define W32_TTM_GETTEXTA          (W32_WM_USER + 11)
#define W32_TTM_GETTEXTW          (W32_WM_USER + 56)
#define W32_TTM_UPDATETIPTEXTA    (W32_WM_USER + 12)
#define W32_TTM_UPDATETIPTEXTW    (W32_WM_USER + 57)
#define W32_TTM_GETTOOLCOUNT      (W32_WM_USER + 13)
#define W32_TTM_ENUMTOOLSA        (W32_WM_USER + 14)
#define W32_TTM_ENUMTOOLSW        (W32_WM_USER + 58)
#define W32_TTM_GETCURRENTTOOLA   (W32_WM_USER + 15)
#define W32_TTM_GETCURRENTTOOLW   (W32_WM_USER + 59)
#define W32_TTM_WINDOWFROMPOINT   (W32_WM_USER + 16)

/* TOOLINFOW (x64): rect is inline; the pointer fields align to 8. */
typedef struct {
    uint32_t     cbSize;
    uint32_t     uFlags;       /* W32_TTF_*                     */
    uint32_t     _pad0;
    W32_HWND     hwnd;         /* the tool window               */
    uint64_t     uId;          /* tool id (or hwnd if IDISHWND) */
    W32_RECT     rect;         /* tool rect in hwnd coords      */
    W32_HINSTANCE hinst;
    uint16_t    *lpszText;     /* text buffer (out) / in        */
    int64_t      lParam;
} W32_TOOLINFOW;

#define W32_TTF_IDISHWND   0x0001u
#define W32_TTF_CENTERTIP  0x0002u
#define W32_TTF_SUBCLASS   0x0010u

#define W32_TTN_FIRST (0u - 520u)
#define W32_TTN_GETDISPINFOA (W32_TTN_FIRST - 0u)
#define W32_TTN_GETDISPINFOW (W32_TTN_FIRST - 10u)
#define W32_TTN_SHOW         (W32_TTN_FIRST - 1u)
#define W32_TTN_POP          (W32_TTN_FIRST - 2u)

#define W32_TTS_ALWAYSTIP  0x0001u
#define W32_TTS_NOPREFIX   0x0002u

/* ---- progress bar ------------------------------------------------------------- */

#define W32_PBM_SETRANGE    (W32_WM_USER + 1)
#define W32_PBM_SETPOS      (W32_WM_USER + 2)
#define W32_PBM_DELTAPOS    (W32_WM_USER + 3)
#define W32_PBM_SETSTEP     (W32_WM_USER + 4)
#define W32_PBM_STEPIT      (W32_WM_USER + 5)
#define W32_PBM_SETRANGE32  (W32_WM_USER + 6)
#define W32_PBM_GETRANGE    (W32_WM_USER + 7)
#define W32_PBM_GETPOS      (W32_WM_USER + 8)

#define W32_PBS_SMOOTH 0x0001u

/* ---- header -------------------------------------------------------------------- */

#define W32_HDM_FIRST 0x1200u
#define W32_HDM_GETITEMCOUNT (W32_HDM_FIRST + 0)
#define W32_HDM_INSERTITEMA  (W32_HDM_FIRST + 1)
#define W32_HDM_INSERTITEMW  (W32_HDM_FIRST + 10)
#define W32_HDM_DELETEITEM   (W32_HDM_FIRST + 2)
#define W32_HDM_GETITEMA     (W32_HDM_FIRST + 3)
#define W32_HDM_GETITEMW     (W32_HDM_FIRST + 11)
#define W32_HDM_SETITEMA     (W32_HDM_FIRST + 4)
#define W32_HDM_SETITEMW     (W32_HDM_FIRST + 12)
#define W32_HDM_LAYOUT       (W32_HDM_FIRST + 5)
#define W32_HDM_HITTEST      (W32_HDM_FIRST + 6)
#define W32_HDM_GETITEMRECT  (W32_HDM_FIRST + 7)

/* HDITEMW (x64). */
typedef struct {
    uint32_t     mask;         /* W32_HDI_*                     */
    int32_t      cxy;          /* column width                  */
    uint32_t     _pad0;
    uint16_t    *pszText;
    W32_HBITMAP  hbm;
    int32_t      cchTextMax;
    int32_t      fmt;          /* W32_HDF_*                     */
    uint32_t     _pad1;
    int64_t      lParam;
    int32_t      iImage;
    int32_t      iOrder;
} W32_HDITEMW;

#define W32_HDI_WIDTH  0x0001u
#define W32_HDI_TEXT   0x0002u
#define W32_HDI_FORMAT 0x0004u
#define W32_HDI_LPARAM 0x0008u

#define W32_HDF_LEFT   0x0000u
#define W32_HDF_STRING 0x4000u

#define W32_HDN_FIRST (0u - 300u)
#define W32_HDN_ITEMCLICKA (W32_HDN_FIRST - 2u)
#define W32_HDN_ITEMCLICKW (W32_HDN_FIRST - 22u)
#define W32_HDN_ITEMCHANGEDA (W32_HDN_FIRST - 1u)
#define W32_HDN_ITEMCHANGEDW (W32_HDN_FIRST - 21u)

#define W32_HDS_BUTTONS 0x0002u

/* ---- ImageList ------------------------------------------------------------------- */

#define W32_ILC_MASK      0x00000001u
#define W32_ILC_COLOR     0x00000000u
#define W32_ILC_COLOR32   0x00000020u

#define W32_ILD_NORMAL      0x0000u
#define W32_ILD_TRANSPARENT 0x0001u
#define W32_ILD_MASK        0x0004u

typedef struct {
    W32_HBITMAP hbmImage;      /* the strip (opaque handle here) */
    W32_HBITMAP hbmMask;
    W32_RECT    rcImage;       /* this image's cell in the strip */
} W32_IMAGEINFO;

W32ABI W32_HIMAGELIST ImageList_Create(int32_t cx, int32_t cy, W32_UINT flags,
                                       int32_t initial, int32_t grow);
W32ABI W32_BOOL ImageList_Destroy(W32_HIMAGELIST himl);
W32ABI int32_t  ImageList_AddMasked(W32_HIMAGELIST himl, W32_HBITMAP hbm,
                                    uint32_t crMask);
W32ABI int32_t  ImageList_ReplaceIcon(W32_HIMAGELIST himl, int32_t i,
                                      W32_HICON icon);
W32ABI int32_t  ImageList_GetImageCount(W32_HIMAGELIST himl);
W32ABI W32_HICON ImageList_GetIcon(W32_HIMAGELIST himl, int32_t i,
                                   W32_UINT flags);
W32ABI W32_BOOL ImageList_GetIconSize(W32_HIMAGELIST himl, int32_t *cx,
                                      int32_t *cy);
W32ABI W32_BOOL ImageList_GetImageInfo(W32_HIMAGELIST himl, int32_t i,
                                       W32_IMAGEINFO *info);
W32ABI W32_BOOL ImageList_Draw(W32_HIMAGELIST himl, int32_t i, W32_HDC hdc,
                               int32_t x, int32_t y, W32_UINT style);
W32ABI W32_BOOL ImageList_SetIconSize(W32_HIMAGELIST himl, int32_t cx,
                                      int32_t cy);
W32ABI W32_BOOL ImageList_Remove(W32_HIMAGELIST himl, int32_t i);

/* The drag set (REAL drag images: the drag window draws the tracked
 * image at the drag point; receipts come from GetPixel on the lock
 * window's DC). */
W32ABI W32_BOOL ImageList_BeginDrag(W32_HIMAGELIST himl, int32_t track,
                                    int32_t dxHotspot, int32_t dyHotspot);
W32ABI W32_BOOL ImageList_DragEnter(W32_HWND lock, int32_t x, int32_t y);
W32ABI W32_BOOL ImageList_DragMove(int32_t x, int32_t y);
W32ABI W32_BOOL ImageList_DragShowNolock(W32_BOOL show);
W32ABI void    ImageList_EndDrag(void);

/* ---- subclassing (ordinals 410-413) -------------------------------------------- */

typedef W32_LRESULT (W32ABI *W32_SUBCLASSPROC)(W32_HWND, W32_UINT,
                                               W32_WPARAM, W32_LPARAM,
                                               uint64_t id, int64_t data);

W32ABI W32_BOOL    SetWindowSubclass(W32_HWND hwnd, W32_SUBCLASSPROC proc,
                                     uint64_t id, int64_t data);
W32ABI W32_BOOL    RemoveWindowSubclass(W32_HWND hwnd, uint64_t id);
W32ABI W32_BOOL    GetWindowSubclass(W32_HWND hwnd, W32_SUBCLASSPROC proc,
                                     uint64_t id, int64_t *data);
W32ABI W32_LRESULT DefSubclassProc(W32_HWND hwnd, W32_UINT msg,
                                   W32_WPARAM wp, W32_LPARAM lp);

/* ---- _TrackMouseEvent (forwards to the W32A-5 USER32 entry) --------------------- */

typedef struct {
    uint32_t cbSize;
    uint32_t dwFlags;
    W32_HWND hwndTrack;
    uint32_t dwHoverTime;
} W32_TRACKMOUSEEVENT;

#define W32_TME_HOVER  0x00000001u
#define W32_TME_LEAVE  0x00000002u

W32ABI W32_BOOL _TrackMouseEvent(W32_TRACKMOUSEEVENT *evt);

/* ---- PropertySheetW -------------------------------------------------------------- */

typedef W32_INT_PTR (W32ABI *W32_DLGPROCP)(W32_HWND, W32_UINT, W32_WPARAM,
                                           W32_LPARAM);

typedef struct {
    uint32_t     dwSize;
    uint32_t     dwFlags;      /* W32_PSH_*                     */
    uint32_t     _pad0;
    W32_HWND     hwndParent;
    W32_HINSTANCE hInstance;
    const uint16_t *pszIcon;   /* union with hIcon              */
    const uint16_t *pszCaption;
    uint32_t     nPages;
    uint32_t     _pad1;
    const void  *ppsp;         /* array of PROPSHEETPAGEW (by value,
                                  PSH_PROPSHEETPAGE) or of
                                  HPROPSHEETPAGE handles         */
    void        *pfnCallback;  /* optional; ignored (documented) */
} W32_PROPSHEETHEADERW;

typedef struct {
    uint32_t     dwSize;
    uint32_t     dwFlags;      /* W32_PSP_*                     */
    uint32_t     _pad0;
    W32_HINSTANCE hInstance;
    const void  *pResource;    /* in-memory DLGTEMPLATE (indirect) */
    const uint16_t *pszIcon;
    const uint16_t *pszTitle;  /* tab label                     */
    W32_DLGPROCP pfnDlgProc;   /* the page proc                 */
    int64_t      lParam;
} W32_PROPSHEETPAGEW;

#define W32_PSH_DEFAULT        0x00000000u
#define W32_PSH_PROPSHEETPAGE  0x00000008u
#define W32_PSH_NOAPPLYNOW     0x00000080u

#define W32_PSP_DEFAULT        0x00000000u
#define W32_PSP_DLGINDIRECT    0x00000010u   /* pResource is an in-memory template */

/* Sheet messages (documented Prsht.h values). */
#define W32_PSM_SETCURSEL      (W32_WM_USER + 101)   /* 0x0465 */
#define W32_PSM_PRESSBUTTON    1137                  /* 0x0471 */

/* PSM_PRESSBUTTON wParam. */
#define W32_PSBTN_BACK      0u
#define W32_PSBTN_NEXT      1u
#define W32_PSBTN_FINISH    2u
#define W32_PSBTN_OK        3u
#define W32_PSBTN_APPLYNOW  4u
#define W32_PSBTN_CANCEL    5u
#define W32_PSBTN_MAX       6u

/* Page notifications (WM_NOTIFY from the sheet; PSHNOTIFY at lParam). */
typedef struct {
    W32_NMHDR hdr;             /* hwndFrom = the SHEET          */
    int32_t   iPage;
    uint32_t  _pad;
    int64_t   lParam;          /* PSN_APPLY: TRUE=OK/Close, FALSE=Apply */
} W32_PSHNOTIFY;

#define W32_PSN_FIRST        (0u - 200u)
#define W32_PSN_SETACTIVE    (W32_PSN_FIRST - 0u)
#define W32_PSN_KILLACTIVE   (W32_PSN_FIRST - 1u)
#define W32_PSN_APPLY        (W32_PSN_FIRST - 2u)
#define W32_PSN_RESET        (W32_PSN_FIRST - 3u)
#define W32_PSN_HELP         (W32_PSN_FIRST - 5u)
#define W32_PSN_QUERYCANCEL  (W32_PSN_FIRST - 9u)

#define W32_PSNRET_NOERROR             0
#define W32_PSNRET_INVALID             1
#define W32_PSNRET_INVALID_NOCHANGEPAGE 2
#define W32_PSNRET_MESSAGEHANDLED      3

/* The sheet's own buttons (WM_COMMAND ids the fixture can drive). */
#define W32_IDOK        1
#define W32_IDCANCEL    2
#define W32_ID_APPLY_NOW 0x3021

W32ABI W32_INT_PTR PropertySheetW(W32_PROPSHEETHEADERW *header);

/* ---- internal exports (personality-internal, not bound) -------------------------- */

/* user32_win.c calls these from CreateWindowExW/DestroyWindow so the
 * WS_CHILD gate and teardown stay in the window core.  Returns 1 when
 * `clsname` is a common-control class (compares by W-string). */
int w32_comctl_is_class(const uint16_t *clsname);

/* Register every W32A-8 control class (idempotent; called by
 * InitCommonControls/Ex and by CreateToolbarEx/CreateStatusWindowW so
 * the BAR classes exist even without an explicit init call, matching
 * the documented "call the create function, it initialises itself"). */
void w32_comctl_register_classes(void);

/* A comctl-class window is going away: drop its control state. */
void w32_comctl_window_destroyed(W32_HWND hwnd);

/* The control id of a window (comctl windows carry one; mirrors
 * GWLP_ID). */
int32_t w32_comctl_ctrl_id(W32_HWND hwnd);

/* Host-suite hook: the fake compositor's window pixels (the host test
 * supplies it; the guest path never calls it). */
#ifdef AURALITE_W32_HOST_TEST
struct w32_comctl_host_hooks {
    int  (*win_dc_pixels)(int win_index, uint32_t *out_w, uint32_t *out_h,
                          uint32_t **out_px);
};
extern const struct w32_comctl_host_hooks *w32_comctl_host;
#endif

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_W32_COMCTL32_H */
