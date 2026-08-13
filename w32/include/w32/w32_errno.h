/* w32_errno.h — Win32 error codes and the last-error slot.
 *
 * WIN32_PLAN.md phase W32-4: "Win32 error codes, set on every failure path."
 *
 * The values are from the published Win32 error list.  They are facts about an
 * interface -- a program that checks for ERROR_INVALID_HANDLE is checking for
 * 6 -- and are written here from documentation rather than copied from any
 * SDK header (w32/LICENSING.md).
 *
 * Only the codes the implemented functions can actually return are defined.
 * Adding one is cheap; a wall of unused constants would obscure which failures
 * this personality genuinely produces.
 */

#ifndef AURALITE_W32_ERRNO_H
#define AURALITE_W32_ERRNO_H

#include "w32/w32_abi.h"

#define W32_ERROR_SUCCESS                0u
#define W32_ERROR_INVALID_FUNCTION       1u
#define W32_ERROR_FILE_NOT_FOUND         2u
#define W32_ERROR_PATH_NOT_FOUND         3u
#define W32_ERROR_ACCESS_DENIED          5u
#define W32_ERROR_INVALID_HANDLE         6u
#define W32_ERROR_NOT_ENOUGH_MEMORY      8u
#define W32_ERROR_INVALID_DATA          13u
#define W32_ERROR_NOT_SUPPORTED         50u
#define W32_ERROR_INVALID_PARAMETER     87u
#define W32_ERROR_INSUFFICIENT_BUFFER  122u
#define W32_ERROR_TOO_MANY_OPEN_FILES    4u
#define W32_ERROR_HANDLE_EOF            38u
#define W32_ERROR_WRITE_FAULT           29u
#define W32_ERROR_READ_FAULT            30u
#define W32_ERROR_PROC_NOT_FOUND       127u

/* The per-process last-error slot.  Win32 makes this thread-local; AuraLite's
 * w32 personality is single-threaded per process for now, so a process-wide
 * slot is exact rather than approximate.  When threads arrive it moves to TLS,
 * and that is recorded in the plan rather than pre-built. */
void      w32_set_last_error(W32_DWORD code);
W32_DWORD w32_get_last_error_raw(void);

/* Translate an AuraLite negative errno into the closest Win32 code.  Kept in
 * one place so a new syscall wrapper cannot invent its own mapping. */
W32_DWORD w32_error_from_errno(long err);

#endif /* AURALITE_W32_ERRNO_H */
