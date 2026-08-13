/* kernel32.h — the bounded first import set.  WIN32_PLAN.md phase W32-4.
 *
 * Per decision D7, this is not "implement KERNEL32".  It is exactly the
 * functions the phase's own gate binaries import, discovered by dumping their
 * import tables, and it grows only when a new gate binary needs something.
 * That keeps the surface auditable and stops the phase becoming endless.
 *
 * Every export is W32ABI.  A missing annotation is silent -- see
 * tests/unit/test_w32_abi.c and its negative control.
 */

#ifndef AURALITE_W32_KERNEL32_H
#define AURALITE_W32_KERNEL32_H

#include "w32/w32_abi.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"

/* --- process ------------------------------------------------------------- */
W32ABI void      ExitProcess(unsigned int exit_code) __attribute__((noreturn));
W32ABI W32_DWORD GetLastError(void);
W32ABI void      SetLastError(W32_DWORD code);

/* --- handles and I/O ----------------------------------------------------- */
W32ABI W32_HANDLE GetStdHandle(W32_DWORD which);
W32ABI W32_BOOL   CloseHandle(W32_HANDLE h);
W32ABI W32_BOOL   WriteFile(W32_HANDLE h, const void *buf, W32_DWORD len,
                            W32_DWORD *written, void *overlapped);
W32ABI W32_BOOL   ReadFile(W32_HANDLE h, void *buf, W32_DWORD len,
                           W32_DWORD *got, void *overlapped);

/* Desired-access and creation-disposition values used by CreateFileA. */
#define W32_GENERIC_READ   0x80000000u
#define W32_GENERIC_WRITE  0x40000000u
#define W32_CREATE_NEW        1u
#define W32_CREATE_ALWAYS     2u
#define W32_OPEN_EXISTING     3u
#define W32_OPEN_ALWAYS       4u
#define W32_TRUNCATE_EXISTING 5u

W32ABI W32_HANDLE CreateFileA(const char *path, W32_DWORD access,
                              W32_DWORD share, void *sa,
                              W32_DWORD disposition, W32_DWORD flags,
                              W32_HANDLE tmpl);

/* --- memory --------------------------------------------------------------- */
#define W32_MEM_COMMIT   0x1000u
#define W32_MEM_RESERVE  0x2000u
#define W32_MEM_RELEASE  0x8000u
#define W32_PAGE_READWRITE 0x04u

W32ABI void    *VirtualAlloc(void *addr, unsigned long long size,
                             W32_DWORD type, W32_DWORD protect);
W32ABI W32_BOOL VirtualFree(void *addr, unsigned long long size, W32_DWORD type);

/* A minimal process heap.  HeapAlloc/HeapFree are what a CRT actually calls;
 * GetProcessHeap returns an opaque token this implementation does not need to
 * distinguish, because there is only one heap. */
W32ABI W32_HANDLE GetProcessHeap(void);
W32ABI void      *HeapAlloc(W32_HANDLE heap, W32_DWORD flags,
                            unsigned long long size);
W32ABI W32_BOOL   HeapFree(W32_HANDLE heap, W32_DWORD flags, void *mem);

/* --- time ----------------------------------------------------------------- */
W32ABI void          Sleep(W32_DWORD ms);
W32ABI W32_ULONGLONG GetTickCount64(void);

/* --- command line --------------------------------------------------------- */
W32ABI const char *GetCommandLineA(void);

/* Called by the CRT stub before anything else; not a Win32 export. */
void w32_kernel32_init(int argc, char **argv);

#endif /* AURALITE_W32_KERNEL32_H */
