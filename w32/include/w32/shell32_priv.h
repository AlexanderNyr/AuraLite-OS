/* w32/include/w32/shell32_priv.h — WR-1 (W32RUN_PLAN.md) the shell namespace.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The COM ABI for the minimal-but-real shell namespace that closes SHELL32
 * "plan section 7": IShellFolder / IEnumIDList / a PIDL model, rooted at
 *
 *     Desktop  ->  My Computer  ->  C:  ->  the filesystem (CFSFolder)
 *
 * scoped by measurement to the surface the pinned 7-Zip FM imports
 * (SHGetDesktopFolder + PIDL info; W32RUN_PLAN D-WR1: this surface, no more).
 * The implementation is w32/src/shell32_ns.c; shell32.c calls
 * ns_get_desktop_folder() (the SHGetDesktopFolder body) and ns_pidl_to_path()
 * (the SHGetFileInfoW PIDL branch). The vtable layouts below match the
 * documented shobjidl ABI so a real consumer can call through them by index.
 */
#ifndef AURALITE_W32_SHELL32_PRIV_H
#define AURALITE_W32_SHELL32_PRIV_H

#include "w32/shell32.h"      /* W32_LONG, W32_DWORD, W32_UINT, W32_HWND, ... */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- STRRET (GetDisplayNameOf out) — the documented x64 layout ---------- */
typedef struct {
    W32_UINT uType;                 /* STRRET_WSTR only (we always return WSTR) */
    union {
        uint16_t *pOleStr;          /* CoTaskMemFree-shaped (plain free) */
        W32_UINT  uOffset;
        char      cStr[260];
    };
} W32_STRRET;

#define W32_STRRET_WSTR   0x0000u

/* ---- SFGAO attribute flags (GetAttributesOf) --------------------------- */
#define W32_SFGAO_CANRENAME       0x00000010u
#define W32_SFGAO_CANDELETE       0x00000020u
#define W32_SFGAO_STREAM          0x00400000u
#define W32_SFGAO_BROWSABLE       0x08000000u
#define W32_SFGAO_FILESYSANCESTOR 0x10000000u
#define W32_SFGAO_FOLDER          0x20000000u
#define W32_SFGAO_FILESYSTEM      0x40000000u
#define W32_SFGAO_HASSUBFOLDER    0x80000000u

/* ---- SHGDN (GetDisplayNameOf flags) ------------------------------------ */
#define W32_SHGDN_NORMAL      0x0000u
#define W32_SHGDN_INFOLDER    0x0001u
#define W32_SHGDN_FORPARSING  0x8000u

/* ---- SHCONTF (EnumObjects flags) --------------------------------------- */
#define W32_SHCONTF_FOLDERS       0x0020u
#define W32_SHCONTF_NONFOLDERS    0x0040u
#define W32_SHCONTF_INCLUDEHIDDEN 0x0080u

/* ---- our PIDL item kinds (stored in the item's second u16) -------------
 * Byte-compatible with shell32.c's existing single-item model:
 *   { u16 cb, u16 kind, u16 path_units, path[units], u16 0, u16 0 }
 * A filesystem item carries its full Win32 path; My Computer carries none. */
#define W32_NS_PIDL_COMPUTER  0xF001u
#define W32_NS_PIDL_NETWORK   0xF002u   /* Network root: real, but empty offline */
#define W32_NS_PIDL_FS        0xF003u

/* ---- IEnumIDList -------------------------------------------------------- */
typedef struct W32_IEnumIDListVtbl {
    W32_LONG  (W32ABI *QueryInterface)(void *self, const void *riid, void **out);
    W32_DWORD (W32ABI *AddRef)(void *self);
    W32_DWORD (W32ABI *Release)(void *self);
    W32_LONG  (W32ABI *Next)(void *self, W32_UINT celt, void **rgelt,
                             W32_UINT *pceltFetched);
    W32_LONG  (W32ABI *Skip)(void *self, W32_UINT celt);
    W32_LONG  (W32ABI *Reset)(void *self);
    W32_LONG  (W32ABI *Clone)(void *self, void **ppenum);
} W32_IEnumIDListVtbl;

typedef struct { const W32_IEnumIDListVtbl *vtbl; } W32_IEnumIDList;

/* ---- IShellFolder (the documented method order) ------------------------ */
typedef struct W32_IShellFolderVtbl {
    W32_LONG  (W32ABI *QueryInterface)(void *self, const void *riid, void **out);
    W32_DWORD (W32ABI *AddRef)(void *self);
    W32_DWORD (W32ABI *Release)(void *self);
    W32_LONG  (W32ABI *ParseDisplayName)(void *self, W32_HWND hwnd, void *pbc,
                                         uint16_t *name, W32_UINT *pchEaten,
                                         void **ppidl, W32_UINT *pdwAttributes);
    W32_LONG  (W32ABI *EnumObjects)(void *self, W32_HWND hwnd, W32_DWORD grfFlags,
                                    void **ppenumIDList);
    W32_LONG  (W32ABI *BindToObject)(void *self, const void *pidl, void *pbc,
                                     const void *riid, void **ppv);
    W32_LONG  (W32ABI *BindToStorage)(void *self, const void *pidl, void *pbc,
                                      const void *riid, void **ppv);
    W32_LONG  (W32ABI *CompareIDs)(void *self, intptr_t lParam,
                                   const void *pidl1, const void *pidl2);
    W32_LONG  (W32ABI *CreateViewObject)(void *self, W32_HWND hwnd,
                                         const void *riid, void **ppv);
    W32_LONG  (W32ABI *GetAttributesOf)(void *self, W32_UINT cidl,
                                        const void **apidl, W32_UINT *rgfInOut);
    W32_LONG  (W32ABI *GetUIObjectOf)(void *self, W32_HWND hwnd, W32_UINT cidl,
                                      const void **apidl, const void *riid,
                                      W32_UINT *rgfReserved, void **ppv);
    W32_LONG  (W32ABI *GetDisplayNameOf)(void *self, const void *pidl,
                                         W32_UINT uFlags, W32_STRRET *pName);
    W32_LONG  (W32ABI *SetNameOf)(void *self, W32_HWND hwnd, const void *pidl,
                                  const uint16_t *pszName, W32_UINT uFlags,
                                  void **ppidlOut);
} W32_IShellFolderVtbl;

typedef struct { const W32_IShellFolderVtbl *vtbl; } W32_IShellFolder;

/* The documented IIDs (little-endian in memory), exported for consumers. */
extern const uint8_t W32_IID_IShellFolder[16];
extern const uint8_t W32_IID_IEnumIDList[16];
extern const uint8_t W32_IID_IUnknown[16];

/* ---- the shell32.c entry points --------------------------------------- */

/* SHGetDesktopFolder body: hands back the desktop IShellFolder. S_OK / *out. */
W32_LONG ns_get_desktop_folder(void **out);

/* Decode one of our namespace PIDLs to its Win32 filesystem path (UTF-16,
 * NUL-terminated into buf). Returns the length in code units, 0 for a virtual
 * PIDL (My Computer — no filesystem path), or -1 on a malformed PIDL. */
int ns_pidl_to_path(const void *pidl, uint16_t *buf, size_t cap);

/* SHGetSpecialFolderLocation body for the *virtual* roots the namespace models
 * (My Computer / CSIDL_DRIVES — no filesystem path). On S_OK, *out is a freshly
 * allocated namespace PIDL byte-compatible with SHGetPathFromIDListW/BindToObject.
 * Returns S_FALSE (1) for a CSIDL this namespace does not model as a virtual
 * root, so the caller falls back to its filesystem CSIDL mapping. */
W32_LONG ns_special_pidl(W32_DWORD csidl, void **out);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_W32_SHELL32_PRIV_H */
