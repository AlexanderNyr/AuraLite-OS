/* comdlg32.h — W32APP_PLAN.md phase W32A-10: the common dialogs.
 *
 * Everything in this header is an interface fact about the documented
 * common-dialog API, pinned from published documentation exactly the
 * way the A-8 control constants were (see w32/PROVENANCE.md): the
 * values below are documented constants, not copied code.  No
 * Microsoft header text, code, or binary is included.
 *
 * Scope (decision D1): exactly the 9 ladder-measured comdlg32.dll
 * symbols.  Eight REAL over the W32A-6 dialog engine (the open/save
 * quartet in A and W, the ChooseColor/ChooseFont quartet in A and W,
 * and CommDlgExtendedError); PrintDlgW is FAIL-CLEAN — there are no
 * printers, the dialog refuses with PDERR_NODEFAULTPRN and the reason
 * is named, exactly like the W32A-7 printing refusals.
 *
 * The open/save dialogs are real modal dialogs: a path Edit, a file
 * list (the directory listing filtered by the selected filter), a
 * filter selector, OK and Cancel.  They are driven the way real
 * dialogs are driven — messages (LB_SETCURSEL then WM_COMMAND IDOK)
 * or the OFN hook (OFN_ENABLEHOOK), which receives WM_INITDIALOG with
 * the dialog handle and can post those messages itself.  The exotic
 * flags are refused by flag with CDERR/FNERR codes, not ignored.
 *
 * Licensed Apache-2.0; interface facts only.
 */

#ifndef AURALITE_W32_COMDLG32_H
#define AURALITE_W32_COMDLG32_H

#include "w32_abi.h"
#include "kernel32.h"
#include "user32.h"
#include "gdi32.h"             /* W32_LOGFONTW */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- OPENFILENAMEW ----------------------------------------------------------- */

#define W32_OFN_READONLY           0x00000001u
#define W32_OFN_OVERWRITEPROMPT    0x00000002u
#define W32_OFN_HIDEREADONLY       0x00000004u
#define W32_OFN_NOCHANGEDIR        0x00000008u
#define W32_OFN_SHOWHELP           0x00000010u   /* refused: no help UI */
#define W32_OFN_ENABLEHOOK         0x00000020u
#define W32_OFN_ENABLETEMPLATE     0x00000040u   /* refused: CDERR_NOTEMPLATE */
#define W32_OFN_ENABLETEMPLATEHANDLE 0x00000080u /* refused: CDERR_NOTEMPLATE */
#define W32_OFN_NOVALIDATE         0x00000100u
#define W32_OFN_ALLOWMULTISELECT   0x00000200u
#define W32_OFN_EXTENSIONDIFFERENT 0x00000400u
#define W32_OFN_PATHMUSTEXIST      0x00000800u
#define W32_OFN_FILEMUSTEXIST      0x00001000u
#define W32_OFN_CREATEPROMPT       0x00002000u
#define W32_OFN_SHAREAWARE         0x00004000u
#define W32_OFN_NOREADONLYRETURN   0x00008000u
#define W32_OFN_NOTESTFILECREATE   0x00010000u
#define W32_OFN_NONETWORKBUTTON    0x00020000u
#define W32_OFN_NOLONGNAMES        0x00040000u
#define W32_OFN_EXPLORER           0x00080000u   /* accepted: the only style */
#define W32_OFN_NODEREFERENCELINKS 0x00100000u
#define W32_OFN_LONGNAMES          0x00200000u
#define W32_OFN_ENABLEINCLUDENOTIFY 0x00400000u  /* refused: no include hook */
#define W32_OFN_ENABLESIZING       0x00800000u   /* accepted: sizing is free */

typedef struct {
    W32_DWORD     lStructSize;
    W32_HWND      hwndOwner;
    W32_HINSTANCE hInstance;
    W32_LPCWSTR   lpstrFilter;      /* "name\0pattern\0...\0\0" */
    W32_LPWSTR    lpstrCustomFilter;
    W32_UINT      nMaxCustFilter;
    W32_UINT      nFilterIndex;     /* 1-based */
    W32_LPWSTR    lpstrFile;        /* in: default; out: result */
    W32_DWORD     nMaxFile;
    W32_LPWSTR    lpstrFileTitle;
    W32_DWORD     nMaxFileTitle;
    W32_LPCWSTR   lpstrInitialDir;
    W32_LPCWSTR   lpstrTitle;
    W32_DWORD     Flags;
    W32_WORD      nFileOffset;      /* out: units to the name */
    W32_WORD      nFileExtension;   /* out: units to the extension */
    W32_LPCWSTR   lpstrDefExt;
    W32_LPARAM    lCustData;
    void         *lpfnHook;      /* documented union of pointers: one pointer wide */
    W32_LPCWSTR   lpTemplateName;   /* refused with CDERR_NOTEMPLATE+HOOK */
    void         *pvReserved;
    W32_DWORD     dwReserved;
    W32_DWORD     FlagsEx;
} W32_OPENFILENAMEW;

W32_BOOL W32ABI GetOpenFileNameW(W32_OPENFILENAMEW *ofn);
W32_BOOL W32ABI GetSaveFileNameW(W32_OPENFILENAMEW *ofn);
W32_BOOL W32ABI GetOpenFileNameA(void *ofn);   /* byte-identical layout */
W32_BOOL W32ABI GetSaveFileNameA(void *ofn);

/* ---- CHOOSECOLORW / CHOOSEFONTW ---------------------------------------------- */

#define W32_CC_RGBINIT        0x00000001u
#define W32_CC_FULLOPEN       0x00000002u
#define W32_CC_PREVENTFULLOPEN 0x00000004u
#define W32_CC_SHOWHELP       0x00000008u   /* refused: no help UI */
#define W32_CC_ENABLEHOOK     0x00000010u
#define W32_CC_ENABLETEMPLATE 0x00000020u   /* refused with CDERR_NOTEMPLATE */
#define W32_CC_SOLIDCOLOR     0x00000080u
#define W32_CC_ANYCOLOR       0x00000100u

typedef struct {
    W32_DWORD     lStructSize;
    W32_HWND      hwndOwner;
    W32_HINSTANCE hInstance;
    W32_DWORD     rgbResult;
    W32_DWORD    *lpCustColors;     /* 16 COLORREFs */
    W32_DWORD     Flags;
    W32_LPARAM    lCustData;
    void         *lpfnHook;
    W32_LPCWSTR   lpTemplateName;
} W32_CHOOSECOLORW;

#define W32_CF_SCREENFONTS    0x00000001u
#define W32_CF_PRINTERFONTS   0x00000002u   /* refused: no printers */
#define W32_CF_EFFECTS        0x00000100u
#define W32_CF_INITTOLOGFONTSTRUCT 0x00000040u
#define W32_CF_ENABLEHOOK     0x00000008u
#define W32_CF_ENABLETEMPLATE 0x00000010u   /* refused with CDERR_NOTEMPLATE */
#define W32_CF_LIMITSIZE      0x00002000u

typedef struct {
    W32_DWORD     lStructSize;
    W32_HWND      hwndOwner;
    W32_HANDLE    hDC;              /* ignored: no printer DCs */
    W32_LOGFONTW *lpLogFont;
    W32_INT       iPointSize;       /* out: tenths of a point */
    W32_DWORD     Flags;
    W32_DWORD     rgbColors;
    W32_LPARAM    lCustData;
    void         *lpfnHook;
    W32_LPCWSTR   lpTemplateName;
    W32_HINSTANCE hInstance;
    W32_LPWSTR    lpszStyle;
    W32_WORD      nFontType;
    W32_WORD      nSizeMin;
    W32_WORD      nSizeMax;
} W32_CHOOSEFONTW;

W32_BOOL W32ABI ChooseColorW(W32_CHOOSECOLORW *cc);
W32_BOOL W32ABI ChooseColorA(void *cc);
W32_BOOL W32ABI ChooseFontW(W32_CHOOSEFONTW *cf);
W32_BOOL W32ABI ChooseFontA(void *cf);

/* ---- PrintDlgW (FAIL-CLEAN) --------------------------------------------------- */

typedef struct {
    W32_DWORD lStructSize;
    W32_HWND  hwndOwner;
    W32_HANDLE hDevMode;
    W32_HANDLE hDevNames;
    W32_HANDLE hDC;
    W32_DWORD Flags;
    W32_WORD  nFromPage, nToPage, nMinPage, nMaxPage;
    W32_WORD  nCopies;
    W32_HINSTANCE hInstance;
    W32_LPARAM lCustData;
    void         *lpfnHook;
    W32_LPCWSTR lpTemplateName;
    W32_LPWSTR lpPrintTemplateName;
} W32_PRINTDLGW;

#define W32_PDERR_NODEFAULTPRN  0x2006u

W32_BOOL W32ABI PrintDlgW(W32_PRINTDLGW *pd);

/* ---- the listbox driver protocol -----------------------------------------------
 *
 * The file/folder lists are listboxes: the documented LB_* message
 * numbers and LBN_* notifications are how a driver (or an OFN hook)
 * addresses them — LB_SETCURSEL then WM_COMMAND IDOK selects and
 * accepts, LB_SETSEL/LB_GETSELITEMS drive multiselect, LB_DIR fills a
 * list from a file pattern. */

#define W32_LB_ADDSTRING   0x0180u
#define W32_LB_INSERTSTRING 0x0181u
#define W32_LB_DELETESTRING 0x0182u
#define W32_LB_RESETCONTENT 0x0184u
#define W32_LB_SETSEL      0x0185u
#define W32_LB_SETCURSEL   0x0186u
#define W32_LB_GETCURSEL   0x0188u
#define W32_LB_GETTEXT     0x0189u
#define W32_LB_GETTEXTLEN  0x018Au
#define W32_LB_GETCOUNT    0x018Bu
#define W32_LB_DIR         0x018Du
#define W32_LB_GETSELCOUNT 0x0190u
#define W32_LB_GETSELITEMS 0x0191u
#define W32_LBN_SELCHANGE  1u
#define W32_LBN_DBLCLK     2u
#define W32_DDL_READWRITE  0x0000u
#define W32_DDL_DIRECTORY  0x0010u
#define W32_DDL_DRIVES     0x4000u

/* ---- extended error ----------------------------------------------------------- */

/* 0 after a normal return or cancel — the documented contract.  The
 * error codes this engine can set, with the reason logged once: */
#define W32_CDERR_DIALOGFAILURE   0xFFFFu
#define W32_CDERR_STRUCTSIZE      0x0001u
#define W32_CDERR_INITIALIZATION  0x0002u
#define W32_CDERR_NOTEMPLATE      0x0009u
#define W32_CDERR_NOHINSTANCE     0x0004u
#define W32_FNERR_BUFFERTOOSMALL  0x3001u
#define W32_FNERR_INVALIDFILENAME 0x3002u

W32_DWORD W32ABI CommDlgExtendedError(void);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_W32_COMDLG32_H */
