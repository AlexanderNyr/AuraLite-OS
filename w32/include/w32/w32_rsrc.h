/* w32_rsrc.h — PE resource access (W32A-6).
 *
 * W32APP_PLAN.md phase W32A-6: FindResource/LoadResource/LockResource/
 * SizeofResource/LoadString/LoadIcon/LoadCursor/LoadImage walk the
 * .rsrc directory parsed by w32_pe.c.  See w32/src/w32_rsrc.c for
 * implementation notes and documented limitations.
 */
#ifndef AURALITE_W32_RSRC_H
#define AURALITE_W32_RSRC_H

#include "w32/w32_abi.h"
#include <stdint.h>

/* w32_rsrc.h is included before w32/kernel32.h in some translation units,
 * so forward-declare the small Win32 typedefs it needs here rather than
 * pull in kernel32.h (which would create a circular header dependency). */
#ifndef W32_UINT_DEFINED
#define W32_UINT_DEFINED
typedef uint32_t W32_UINT;
#endif

/* W32 RT_* resource types.  Values match the PE/Win32 spec. */
#define W32_RT_CURSOR       1u
#define W32_RT_BITMAP       2u
#define W32_RT_ICON         3u
#define W32_RT_MENU         4u
#define W32_RT_DIALOG       5u
#define W32_RT_STRING       6u
#define W32_RT_FONTDIR      7u
#define W32_RT_FONT         8u
#define W32_RT_ACCELERATOR  9u
#define W32_RT_RCDATA      10u
#define W32_RT_MESSAGETABLE 11u
#define W32_RT_GROUP_CURSOR 12u
#define W32_RT_GROUP_ICON   14u
#define W32_RT_VERSION     16u
#define W32_RT_DLGINCLUDE  17u
#define W32_RT_PLUGPLAY    19u
#define W32_RT_VXD         20u
#define W32_RT_ANICURSOR   21u
#define W32_RT_ANIICON     22u
#define W32_RT_HTML        23u
#define W32_RT_MANIFEST    24u

/* W32A-6 FindResourceExW helper (internal) -- language-walking form. */
void *w32_FindResourceExW(void *hModule, const uint16_t *type,
                          const uint16_t *name, uint16_t lang);

/* Public prototypes for the resource APIs; the symbols are exported from
 * both KERNEL32.dll and USER32.dll via w32_bind.c, and are consumed from
 * w32_dlg.c for dialog/menu/accel template loading. */
W32ABI void       *FindResourceW(void *hModule, const uint16_t *name, const uint16_t *type);
W32ABI void       *FindResourceA(void *hModule, const char *name, const char *type);
W32ABI void       *FindResourceExW(void *hModule, const uint16_t *type, const uint16_t *name, uint16_t lang);
W32ABI void       *FindResourceExA(void *hModule, const char *type, const char *name, uint16_t lang);
W32ABI void       *LoadResource(void *hModule, void *hResInfo);
W32ABI const void *LockResource(void *hResData);
W32ABI W32_DWORD   SizeofResource(void *hModule, void *hResInfo);
W32ABI W32_BOOL    FreeResource(void *hResData);
W32ABI int         LoadStringW(void *hModule, W32_UINT id, uint16_t *buf, int cch);
W32ABI int         LoadStringA(void *hModule, W32_UINT id, char *buf, int cch);
W32ABI void       *LoadIconW(void *hModule, const uint16_t *name);
W32ABI void       *LoadIconA(void *hModule, const char *name);
W32ABI void       *LoadCursorW(void *hModule, const uint16_t *name);
W32ABI void       *LoadCursorA(void *hModule, const char *name);
W32ABI void       *LoadImageW(void *hModule, const uint16_t *name, W32_UINT type, int32_t cx, int32_t cy, W32_UINT flags);
W32ABI void       *LoadImageA(void *hModule, const char *name, W32_UINT type, int32_t cx, int32_t cy, W32_UINT flags);
W32ABI W32_BOOL    DestroyIcon(void *hIcon);
W32ABI W32_BOOL    DestroyCursor(void *hCursor);
W32ABI int         EnumResourceNamesW(void *hModule, const uint16_t *type,
                                      W32_BOOL (W32ABI *cb)(void *, const uint16_t *, uintptr_t), uintptr_t lp);
W32ABI int         EnumResourceNamesA(void *hModule, const char *type,
                                      W32_BOOL (W32ABI *cb)(void *, const char *, uintptr_t), uintptr_t lp);

#endif /* AURALITE_W32_RSRC_H */
