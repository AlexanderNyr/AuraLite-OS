/* W32A-11: in-process COM-lite and OLE drag/drop interfaces.
 * Win64 vtable layouts, TYMED values and HRESULTs from published OLE2,
 * oleidl.h and objidl.h documentation. No STA, marshalling, or external OLE
 * source is claimed. SPDX-License-Identifier: Apache-2.0 */
#ifndef AURALITE_W32_OLE32_H
#define AURALITE_W32_OLE32_H
#include "w32_abi.h"
#include "user32.h"
#include <stddef.h>
#define W32_COM_S_OK 0u
#define W32_COM_S_FALSE 1u
#define W32_COM_E_INVALIDARG 0x80070057u
#define W32_COM_E_OUTOFMEMORY 0x8007000Eu
#define W32_COM_E_NOTINITIALIZED 0x800401F0u
#define W32_DRAGDROP_E_NOTREGISTERED 0x80040100u
#define W32_DRAGDROP_E_ALREADYREGISTERED 0x80040101u
#define W32_DRAGDROP_E_INVALIDHWND 0x80040102u
#define W32_DRAGDROP_S_DROP 0x00040100u
#define W32_DRAGDROP_S_CANCEL 0x00040101u
#define W32_DRAGDROP_S_USEDEFAULTCURSORS 0x00040102u
#define W32_DROPEFFECT_NONE 0u
#define W32_DROPEFFECT_COPY 1u
#define W32_DROPEFFECT_MOVE 2u
#define W32_DROPEFFECT_LINK 4u
#define W32_TYMED_NULL 0u
#define W32_TYMED_HGLOBAL 1u
#define W32_TYMED_FILE 2u
#define W32_TYMED_ISTREAM 4u
#define W32_TYMED_ISTORAGE 8u
#define W32_TYMED_GDI 16u
#define W32_TYMED_MFPICT 32u
#define W32_TYMED_ENHMF 64u

typedef struct {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
} W32_GUID;
typedef struct W32_IDropTarget W32_IDropTarget;
typedef struct W32_IDropSource W32_IDropSource;
typedef struct { int32_t x, y; } W32_POINTL;
typedef struct {
    W32_DWORD (W32ABI *QueryInterface)(W32_IDropTarget *, const W32_GUID *, void **);
    W32_DWORD (W32ABI *AddRef)(W32_IDropTarget *);
    W32_DWORD (W32ABI *Release)(W32_IDropTarget *);
    W32_DWORD (W32ABI *DragEnter)(W32_IDropTarget *, void *, W32_DWORD,
                                   W32_POINTL, W32_DWORD *);
    W32_DWORD (W32ABI *DragOver)(W32_IDropTarget *, W32_DWORD,
                                  W32_POINTL, W32_DWORD *);
    W32_DWORD (W32ABI *DragLeave)(W32_IDropTarget *);
    W32_DWORD (W32ABI *Drop)(W32_IDropTarget *, void *, W32_DWORD,
                              W32_POINTL, W32_DWORD *);
} W32_IDropTargetVtbl;
struct W32_IDropTarget { const W32_IDropTargetVtbl *vt; };
typedef struct {
    W32_DWORD (W32ABI *QueryInterface)(W32_IDropSource *, const W32_GUID *, void **);
    W32_DWORD (W32ABI *AddRef)(W32_IDropSource *);
    W32_DWORD (W32ABI *Release)(W32_IDropSource *);
    W32_DWORD (W32ABI *QueryContinueDrag)(W32_IDropSource *, W32_BOOL,
                                           W32_DWORD);
    W32_DWORD (W32ABI *GiveFeedback)(W32_IDropSource *, W32_DWORD);
} W32_IDropSourceVtbl;
struct W32_IDropSource { const W32_IDropSourceVtbl *vt; };
typedef struct {
    W32_DWORD tymed;
    W32_DWORD _pad;
    void *medium;           /* Win64 STGMEDIUM union: one pointer-width slot */
    void *pUnkForRelease;
} W32_STGMEDIUM;

W32ABI W32_DWORD CoInitialize(void *reserved);
W32ABI void CoUninitialize(void);
W32ABI W32_DWORD OleInitialize(void *reserved);
W32ABI void OleUninitialize(void);
W32ABI void *CoTaskMemAlloc(size_t bytes);
W32ABI void *CoTaskMemRealloc(void *ptr, size_t bytes);
W32ABI void CoTaskMemFree(void *ptr);
int w32_com_ole_ready(void);
W32ABI W32_DWORD RegisterDragDrop(W32_HWND hwnd, W32_IDropTarget *target);
W32ABI W32_DWORD RevokeDragDrop(W32_HWND hwnd);
void w32_ole_drag_window_destroyed(W32_HWND hwnd);
W32ABI W32_DWORD DoDragDrop(void *data, W32_IDropSource *source,
                            W32_DWORD allowed, W32_DWORD *effect);
W32ABI void ReleaseStgMedium(W32_STGMEDIUM *m);
#endif
