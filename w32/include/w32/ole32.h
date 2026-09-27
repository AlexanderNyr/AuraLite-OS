/* W32A-11 COM-lite subset: single in-process MTA-like init model, no STA,
 * apartments, marshalling, class activation or external servers yet.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef AURALITE_W32_OLE32_H
#define AURALITE_W32_OLE32_H
#include "w32_abi.h"
#include <stddef.h>
#define W32_COM_S_OK 0u
#define W32_COM_S_FALSE 1u
#define W32_COM_E_INVALIDARG 0x80070057u
#define W32_COM_E_OUTOFMEMORY 0x8007000Eu

typedef struct {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
} W32_GUID;

W32ABI W32_DWORD CoInitialize(void *reserved);
W32ABI void CoUninitialize(void);
W32ABI W32_DWORD OleInitialize(void *reserved);
W32ABI void OleUninitialize(void);
W32ABI void *CoTaskMemAlloc(size_t bytes);
W32ABI void *CoTaskMemRealloc(void *ptr, size_t bytes);
W32ABI void CoTaskMemFree(void *ptr);
#endif
