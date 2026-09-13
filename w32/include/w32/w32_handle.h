/* w32_handle.h — the HANDLE table.
 *
 * WIN32_PLAN.md phase W32-4: "A HANDLE table per process, mapping to AuraLite
 * fds."
 *
 * Win32 HANDLEs and POSIX file descriptors are not interchangeable, and
 * pretending otherwise is a trap:
 *
 *   - a HANDLE is an opaque pointer-width value, an fd is a small int;
 *   - failure is INVALID_HANDLE_VALUE ((HANDLE)-1) for CreateFile but NULL for
 *     most other producers, and (HANDLE)0 is a perfectly valid fd 0;
 *   - the standard handles are negative pseudo-values (-11, -10, -12) that are
 *     never real table entries.
 *
 * So handles are minted from a table rather than by casting an fd.  Casting
 * would make fd 0 indistinguishable from a NULL handle, and would let a
 * program forge a handle by making one up -- the table means an unknown value
 * is rejected instead of being dereferenced.
 */

#ifndef AURALITE_W32_HANDLE_H
#define AURALITE_W32_HANDLE_H

#include "w32/w32_abi.h"

/* Standard handle selectors, as passed to GetStdHandle(). */
#define W32_STD_INPUT_HANDLE   ((W32_DWORD)-10)
#define W32_STD_OUTPUT_HANDLE  ((W32_DWORD)-11)
#define W32_STD_ERROR_HANDLE   ((W32_DWORD)-12)

#define W32_HANDLE_MAX 64

/* Slot kinds.  FD slots carry an fd; the rest carry an opaque payload.
 * W32A-2: CloseHandle owns FD/MAP/PROC; FIND/CHANGE refuse it and close
 * through their own FindClose/FindCloseChangeNotification. */
#define W32_HANDLE_KIND_FD     0
#define W32_HANDLE_KIND_FIND   1
#define W32_HANDLE_KIND_CHANGE 2
#define W32_HANDLE_KIND_MAP    3
#define W32_HANDLE_KIND_PROC   4

/* w32_handle_release's report for a payload slot CloseHandle owns (MAP,
 * PROC): the payload is already freed and there is no fd to close.  -1
 * keeps meaning "not live, or refused". */
#define W32_HANDLE_FREED (-2)

/* Reset the table to its boot state: the three standard handles bound to fds
 * 0/1/2, nothing else open.  Called from the CRT startup path. */
void w32_handle_init(void);

/* Bind an fd, returning a fresh HANDLE, or NULL when the table is full.
 * `closable` records whether CloseHandle may close the underlying fd: the
 * standard handles must survive being closed by a careless program. */
W32_HANDLE w32_handle_alloc(int fd, int closable);

/* Resolve a HANDLE to an fd, or -1 if it is not a live entry.  Accepts the
 * three standard pseudo-handles as well, because a program may pass the value
 * GetStdHandle returned or the selector itself. */
int w32_handle_to_fd(W32_HANDLE h);

/* Release a HANDLE.  Returns the fd that should be closed, or -1 when the
 * handle was not live or must not be closed. */
int w32_handle_release(W32_HANDLE h);

/* Bind an opaque payload under a kind, returning a fresh HANDLE, or NULL
 * when the table is full.  `free_obj` runs on CloseHandle for the kinds
 * CloseHandle owns (MAP, PROC); FIND/CHANGE refuse CloseHandle and are
 * detached with w32_handle_release_obj by their own closer instead. */
W32_HANDLE w32_handle_alloc_obj(int kind, void *obj, void (*free_obj)(void *));

/* The kind of a live table entry, or -1.  Pseudo-selectors (-11 etc.) are
 * not table entries and report -1; resolve them with w32_handle_to_fd. */
int w32_handle_kind(W32_HANDLE h);

/* Borrow a live payload, or NULL unless the handle is live and the kind
 * matches.  The table keeps ownership. */
void *w32_handle_get_obj(W32_HANDLE h, int kind);

/* Detach a live payload of the expected kind and return it, or NULL.  The
 * caller owns the result and must free it; the slot is cleared. */
void *w32_handle_release_obj(W32_HANDLE h, int kind);

/* Number of live, non-standard entries -- for tests and leak checks. */
int w32_handle_live_count(void);

#endif /* AURALITE_W32_HANDLE_H */
