/* shell32.h — W32APP_PLAN.md phase W32A-10: the shell furniture DLL.
 *
 * Everything in this header is an interface fact about the documented
 * shell API, pinned from published documentation exactly the way the
 * A-8 control constants were (see w32/PROVENANCE.md): the values below
 * are documented constants, not copied code.  No Microsoft header
 * text, code, or binary is included.
 *
 * Scope (decision D1): exactly the 19 ladder-measured shell32.dll
 * symbols (w32/stub_map.tsv at the W32A-9 baseline) plus SHELL32#165
 * (SHCreateDirectory, resolved by ordinal in the A-1 map):
 *   - known folders (3) — the CSIDL mapping table is ours and
 *     documented below and in docs/win32.md;
 *   - the PIDL pair — a minimal but REAL model: the ITEMIDLIST carries
 *     the CSIDL and the resolved path, and the two functions
 *     round-trip; full PIDL algebra is plan §7 and nothing else
 *     accepts one;
 *   - SHCreateItemFromParsingName — a minimal REAL IShellItem whose
 *     interface answers path queries and says what it does not do;
 *   - SHGetFileInfoW / ExtractIconExW — REAL type info from our
 *     extension map and icons decoded from PE resources (W32A-6/A-8
 *     machinery);
 *   - SHFileOperationW — copy/move/delete/rename over the VFS through
 *     the W32A-2 file primitives; no undo (FOF_ALLOWUNDO refused by
 *     flag, the reason named);
 *   - ShellExecuteA/W/ExW — verb "open" executes PE/ELF targets via
 *     spawn; every other verb is refused by name with the documented
 *     SE_ERR_* code;
 *   - Shell_NotifyIconW — REAL onto the compositor notification
 *     engine (ag_notify);
 *   - SHChangeNotify — accepted; the file dialogs refresh (the narrow
 *     REAL behaviour, not a broadcast system);
 *   - the drag trio (DragQueryFileW/DragQueryPoint/DragFinish) — the
 *     drop-list model W32A-11 will feed; REAL against it;
 *   - SHBrowseForFolderW — the folder picker over the W32A-6 dialog
 *     engine;
 *   - SHGetDesktopFolder — E_NOTIMPL, the namespace object is §7.
 *
 * Licensed Apache-2.0; interface facts only.
 */

#ifndef AURALITE_W32_SHELL32_H
#define AURALITE_W32_SHELL32_H

#include "w32_abi.h"
#include "kernel32.h"
#include "user32.h"            /* W32_HWND */
#include "gdi32.h"             /* W32_HICON */

#ifdef __cplusplus
extern "C" {
#endif

typedef W32_HANDLE W32_HINSTANCE;   /* the documented handle-only type */

/* ---- CSIDL (the documented constants; the mapping is ours) ------------------- */

#define W32_CSIDL_DESKTOP               0x0000u   /* -> "/" */
#define W32_CSIDL_PROGRAMS              0x0002u   /* -> "/apps" */
#define W32_CSIDL_PERSONAL              0x0005u   /* -> the data root (see below) */
#define W32_CSIDL_STARTMENU             0x000Bu   /* -> "/apps" */
#define W32_CSIDL_STARTUP               0x0007u   /* -> "/apps" */
#define W32_CSIDL_DESKTOPDIRECTORY      0x0010u   /* -> "/" */
#define W32_CSIDL_FONTS                 0x0014u   /* refused: the font is baked in */
#define W32_CSIDL_APPDATA               0x001Au   /* -> <data>/w32/appdata */
#define W32_CSIDL_FLAG_CREATE           0x8000u   /* create the directory */

/* The data root mirrors the A-9 hive policy: /disk when the scratch
 * disk is mounted (settings survive a reboot), else /tmp with the
 * volatility logged once.  CSIDL_PERSONAL is the root itself,
 * CSIDL_APPDATA is <root>/w32/appdata. */
W32_BOOL W32ABI SHGetFolderPathW(W32_HWND owner, W32_DWORD csidl,
                                 W32_HANDLE token, W32_DWORD flags,
                                 W32_LPWSTR path);
W32_BOOL W32ABI SHGetSpecialFolderPathW(W32_HWND owner, W32_LPWSTR path,
                                        W32_DWORD csidl, W32_BOOL create);
/* Fills a PIDL for the CSIDL (see the model below).  S_OK(0) on
 * success, E_FAIL(0x80004005) for a refused CSIDL. */
W32_LONG W32ABI SHGetSpecialFolderLocation(W32_HWND owner, W32_DWORD csidl,
                                           void **pidl);
/* Decodes a PIDL back to its path.  TRUE + the path in buf. */
W32_BOOL W32ABI SHGetPathFromIDListW(const void *pidl, W32_LPWSTR buf);
/* The desktop folder object (IShellFolder) is plan §7. */
W32_LONG W32ABI SHGetDesktopFolder(void **out);

/* ---- the PIDL model (ours, documented; not byte-compatible) ------------------
 *
 * An ITEMIDLIST here is one item: { u16 cb (total bytes after cb),
 * u16 csidl, u16 path_len_units, UTF-16 path, u16 0 terminator }.
 * The two functions above round-trip it; nothing else accepts one
 * (full PIDL algebra is §7).  Malformed PIDLs are refused with
 * ERROR_INVALID_PARAMETER, not guessed at. */

/* ---- IShellItem (minimal, REAL) ----------------------------------------------
 *
 * SHCreateItemFromParsingName builds one for a filesystem path.  The
 * interface is a real COM shape (vtable + refcount).  It answers:
 *   - QueryInterface(IID_IShellItem) -> S_OK, else E_NOINTERFACE;
 *   - GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING) -> the path;
 *     every other SIGDN -> E_INVALIDARG (says so, no guessing).
 * Nothing else.  The IID is the documented GUID. */
W32_LONG W32ABI SHCreateItemFromParsingName(W32_LPCWSTR path, void *bd,
                                                     void *riid, void **out);
W32_LONG W32ABI SHCreateDirectory(W32_HWND owner, W32_LPCWSTR path);
void     W32ABI SHChangeNotify(W32_LONG id, W32_UINT flags,
                               W32_LPCWSTR data1, W32_LPCWSTR data2);

/* ---- SHGetFileInfoW ---------------------------------------------------------- */

typedef struct {
    W32_HICON  hIcon;
    int32_t    iIcon;
    W32_DWORD  dwAttributes;
    W32_WCHAR  szDisplayName[260];
    W32_WCHAR  szTypeName[80];
} W32_SHFILEINFOW;

#define W32_SHGFI_ICON              0x000000100u
#define W32_SHGFI_DISPLAYNAME       0x000000200u
#define W32_SHGFI_TYPENAME          0x000000400u
#define W32_SHGFI_ATTRIBUTES        0x000000800u
#define W32_SHGFI_ICONLOCATION      0x000001000u
#define W32_SHGFI_EXETYPE           0x000002000u
#define W32_SHGFI_SYSICONINDEX      0x000004000u
#define W32_SHGFI_LINKOVERLAY       0x000008000u
#define W32_SHGFI_SELECTED          0x000010000u
#define W32_SHGFI_ATTR_SPECIFIED    0x000020000u
#define W32_SHGFI_LARGEICON         0x000000000u
#define W32_SHGFI_SMALLICON         0x000000001u
#define W32_SHGFI_OPENICON          0x000000002u
#define W32_SHGFI_SHELLICONSIZE     0x000000004u
#define W32_SHGFI_PIDL              0x000000008u
#define W32_SHGFI_USEFILEATTRIBUTES 0x000000010u

/* Typenames come from our extension map (documented): ".exe" (and a
 * loadable PE) -> "Application"; ".txt" -> "Text Document"; ".c" ->
 * "C Source"; directories -> "File Folder"; everything else -> "File".
 * Icons are decoded from the target's PE icon resources; a file with
 * no icons gets the generic document icon (index 0, a real handle).
 * Returns the DWORD the flags ask for (see the documented contract) —
 * 0 on refusal with the reason set. */
W32_DWORD_PTR W32ABI SHGetFileInfoW(W32_LPCWSTR path, W32_DWORD attrs,
                                    W32_SHFILEINFOW *sfi, W32_UINT cb,
                                    W32_UINT flags);
/* nIcons extracted from index; -1 when the file is not a PE we can
 * read.  Icons ride the W32A-6 decode machinery. */
W32_UINT W32ABI ExtractIconExW(W32_LPCWSTR file, int index,
                                        W32_HICON *large, W32_HICON *small,
                                        W32_UINT count);

/* ---- SHFileOperationW -------------------------------------------------------- */

#define W32_FO_MOVE       1u
#define W32_FO_COPY       2u
#define W32_FO_DELETE     3u
#define W32_FO_RENAME     4u

#define W32_FOF_MULTIDESTFILES     0x0001u
#define W32_FOF_CONFIRMMOUSE       0x0002u
#define W32_FOF_SILENT             0x0004u
#define W32_FOF_RENAMEONCOLLISION  0x0008u
#define W32_FOF_NOCONFIRMATION     0x0010u
#define W32_FOF_ALLOWUNDO          0x0040u   /* refused by flag: no undo */
#define W32_FOF_FILESONLY          0x0080u
#define W32_FOF_SIMPLEPROGRESS     0x0100u
#define W32_FOF_NOCONFIRMMKDIR     0x0200u
#define W32_FOF_NOERRORUI          0x0400u

typedef struct {
    W32_HWND   hwnd;
    W32_UINT   wFunc;
    W32_LPCWSTR pFrom;         /* double-null-terminated list */
    W32_LPCWSTR pTo;           /* ditto (not for FO_DELETE) */
    W32_WORD   fFlags;
    W32_BOOL   fAnyOperationsAborted;
    void      *hNameMappings;  /* always NULL (documented: no mapping) */
    W32_LPCWSTR lpszProgressTitle;
} W32_SHFILEOPSTRUCTW;

/* Returns 0 on success.  Refusals: FOF_ALLOWUNDO answers
 * ERROR_CALL_NOT_IMPLEMENTED (120) with the reason logged once — there
 * is no recycle bin; a missing source answers ERROR_FILE_NOT_FOUND
 * (2); a collision without FOF_NOCONFIRMATION answers ERROR_ALREADY_EXISTS
 * (183).  Operations run per element through the W32A-2 file
 * primitives; wildcards in pFrom are expanded with PathMatchSpecW.
 * fAnyOperationsAborted is only ever FALSE (nothing prompts). */

/* Error constant used for FOF_ALLOWUNDO. */
#define W32_DE_CALL_NOT_IMPLEMENTED 120u
#define W32_DE_INVALIDFILES      0x7Cu   /* 124: the documented DE_ code */

W32_INT W32ABI SHFileOperationW(W32_SHFILEOPSTRUCTW *op);

/* ---- ShellExecute ------------------------------------------------------------ */

/* The documented SE_ERR_* values this engine can answer. */
#define W32_SE_ERR_FNF           2    /* file not found */
#define W32_SE_ERR_PNF           3    /* path not found */
#define W32_SE_ERR_ACCESSDENIED  5    /* "runas": no elevation exists */
#define W32_SE_ERR_OOM           8
#define W32_SE_ERR_NOASSOC      31    /* no associations, by design */

/* Verb contract: NULL and "open" execute the target when it is a
 * loadable PE or an ELF (spawned via the W32A-2 process machinery) and
 * answer 33; every other verb — "runas" (no elevation), "print",
 * "edit", anything — answers its documented SE_ERR_* with the reason
 * logged once.  Documents refuse as NOASSOC (there is no association
 * table).  Directories refuse as NOASSOC too (the desktop namespace is
 * §7). */
W32_HINSTANCE W32ABI ShellExecuteW(W32_HWND owner, W32_LPCWSTR verb,
                                            W32_LPCWSTR file, W32_LPCWSTR params,
                                            W32_LPCWSTR dir, W32_INT show);
W32_HINSTANCE W32ABI ShellExecuteA(W32_HWND owner, const char *verb,
                                            const char *file, const char *params,
                                            const char *dir, W32_INT show);

typedef struct {
    W32_DWORD     cbSize;
    W32_DWORD     fMask;
    W32_HWND      hwnd;
    W32_LPCWSTR   lpVerb;
    W32_LPCWSTR   lpFile;
    W32_LPCWSTR   lpParameters;
    W32_LPCWSTR   lpDirectory;
    W32_INT       nShow;
    W32_HINSTANCE hInstApp;
    void         *lpIDList;
    W32_LPCWSTR   lpClass;
    W32_HANDLE    hkeyClass;
    W32_DWORD     dwHotKey;
    void         *hIcon;      /* union hIcon/hMonitor in the real header:
                              * one pointer either way, layout identical */
    W32_HANDLE    hProcess;
} W32_SHELLEXECUTEINFOW;

#define W32_SEE_MASK_DEFAULT        0x00000000u
#define W32_SEE_MASK_NOCLOSEPROCESS 0x00000040u

/* TRUE + hInstApp set on the same contract as ShellExecuteW.
 * SEE_MASK_NOCLOSEPROCESS is honoured: hProcess carries the real
 * process handle the spawn produced (the W32A-2 handle table). */
W32_BOOL W32ABI ShellExecuteExW(W32_SHELLEXECUTEINFOW *info);

/* ---- Shell_NotifyIconW ------------------------------------------------------- */

#define W32_NIM_ADD        0u
#define W32_NIM_MODIFY     1u
#define W32_NIM_DELETE     2u

#define W32_NIF_MESSAGE    0x00000001u
#define W32_NIF_ICON       0x00000002u
#define W32_NIF_TIP        0x00000004u

typedef struct {
    W32_DWORD  cbSize;
    W32_HWND   hWnd;
    W32_UINT   uID;
    W32_UINT   uFlags;
    W32_UINT   uCallbackMessage;
    W32_HICON  hIcon;
    W32_WCHAR  szTip[128];
    W32_DWORD  dwState;
    W32_DWORD  dwStateMask;
    W32_WCHAR  szInfo[256];
    union { W32_UINT uTimeout; W32_UINT uVersion; } DUMMYUNIONNAME;
    W32_WCHAR  szInfoTitle[64];
    W32_DWORD  dwInfoFlags;
} W32_NOTIFYICONDATAW;

/* 8 slots keyed (hWnd, uID).  ADD creates and shows the notification
 * (ag_notify, the compositor engine — visible in screenshots);
 * MODIFY updates it; DELETE removes it.  Unknown key on MODIFY/DELETE
 * and duplicate ADD answer FALSE + ERROR_INVALID_PARAMETER. */

W32_BOOL W32ABI Shell_NotifyIconW(W32_DWORD msg, W32_NOTIFYICONDATAW *data);

/* ---- drag/drop (the model W32A-11 feeds) -------------------------------------
 *
 * The engine keeps one drop list of at most 16 paths, set by the
 * W32A-11 drag source (or, today, by nothing).  DragQueryFileW reads
 * it, DragQueryPoint reports the drop point, DragFinish releases.
 * An empty list answers 0 / FALSE — every call is REAL against the
 * model, the model is just empty until A-11 wires the source. */

W32_UINT W32ABI DragQueryFileW(W32_HANDLE hDrop, W32_UINT index,
                                        W32_LPWSTR buf, W32_UINT cch);
W32_BOOL W32ABI DragQueryPoint(W32_HANDLE hDrop, W32_POINT *pt);
void     W32ABI DragFinish(W32_HANDLE hDrop);

/* ---- SHBrowseForFolderW ------------------------------------------------------ */

typedef struct {
    W32_HWND   hwndOwner;
    void      *pidlRoot;         /* ignored: the tree starts at / */
    W32_LPWSTR pszDisplayName;
    W32_LPCWSTR lpszTitle;
    W32_UINT   ulFlags;
    void      *lpfnCallback;
    W32_LPARAM lParam;
    W32_INT    iImage;
} W32_BROWSEINFOW;

#define W32_BIF_RETURNONLYFSDIRS 0x0001u
#define W32_BIF_NEWDIALOGSTYLE   0x0040u

/* The folder picker over the W32A-6 dialog engine: a list of the
 * directories under the start root (two levels), driven with the
 * keyboard/messages like the file dialogs; pszDisplayName carries the
 * chosen path.  Returns a PIDL (the SHGetSpecialFolderLocation model)
 * or NULL on cancel. */
void    *W32ABI SHBrowseForFolderW(W32_BROWSEINFOW *bi);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_W32_SHELL32_H */
