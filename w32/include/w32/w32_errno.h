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
/* W32A-1: the mpr fail-clean stubs (w32/src/w32_stubs_gen.c).  There is no
 * network provider, so the WNet* surface reports it: opens and queries
 * fail with NO_NETWORK, enumeration is empty (NO_MORE_ITEMS).  Values from
 * the published Win32 error list, like every code above. */
#define W32_ERROR_NO_MORE_ITEMS         259u
#define W32_ERROR_NO_NETWORK           1222u
/* W32A-2: file/process/locale breadth.  Every code below is returned by at
 * least one implemented function (the group tag says which); w32/src/w32_msg.c
 * carries a message for every one of them, or the code is unreturnable.
 * Values from the published Win32 error list, like every code above. */
#define W32_ERROR_NOT_SAME_DEVICE        17u   /* fs: MoveFile across mounts */
#define W32_ERROR_NO_MORE_FILES          18u   /* fs: FindNext exhausts */
#define W32_ERROR_WRITE_PROTECT          19u   /* fs: read-only filesystem */
#define W32_ERROR_SEEK                   25u   /* fs: SetFilePointer fails */
#define W32_ERROR_SHARING_VIOLATION      32u   /* fs: CreateFile vs open share */
#define W32_ERROR_BAD_NETPATH            53u   /* fs: UNC, no provider */
#define W32_ERROR_FILE_EXISTS            80u   /* fs: CopyFile fail-if-exists */
#define W32_ERROR_BROKEN_PIPE           109u   /* fs: EPIPE */
#define W32_ERROR_DISK_FULL             112u   /* fs: ENOSPC */
#define W32_ERROR_INVALID_NAME          123u   /* fs: bad path spelling */
#define W32_ERROR_DIR_NOT_EMPTY         145u   /* fs: ENOTEMPTY */
#define W32_ERROR_ALREADY_EXISTS        183u   /* fs: EEXIST */
#define W32_ERROR_BAD_EXE_FORMAT        193u   /* ps: CreateProcess, not PE/ELF */
#define W32_ERROR_ENVVAR_NOT_FOUND      203u   /* ps: missing variable */
#define W32_ERROR_FILENAME_EXCED_RANGE  206u   /* fs: ENAMETOOLONG */
#define W32_ERROR_PIPE_BUSY             231u   /* fs: WaitNamedPipe, no instance */
#define W32_ERROR_PIPE_NOT_CONNECTED    233u   /* fs: pipe op before connect */
#define W32_ERROR_WAIT_TIMEOUT          258u   /* fs: change-poll, nothing yet */
#define W32_STILL_ACTIVE                259u   /* ps: exit code, not an error */
#define W32_ERROR_DIRECTORY             267u   /* ps: SetCurrentDirectory(file) */
#define W32_ERROR_NO_UNICODE_TRANSLATION 1113u /* loc: strict conversion fails */
#define W32_ERROR_OPERATION_ABORTED 995u  /* fs: copy/move cancelled */
#define W32_ERROR_SEM_TIMEOUT 121u        /* fs: WaitNamedPipe timed out */
#define W32_ERROR_PIPE_CONNECTED 535u     /* fs: ConnectNamedPipe, already */

/* The per-process last-error slot.  Win32 makes this thread-local; AuraLite's
 * w32 personality is single-threaded per process for now, so a process-wide
 * slot is exact rather than approximate.  When threads arrive it moves to TLS,
 * and that is recorded in the plan rather than pre-built. */
void      w32_set_last_error(W32_DWORD code);
W32_DWORD w32_get_last_error_raw(void);

/* Translate an AuraLite negative errno into the closest Win32 code.  Kept in
 * one place so a new syscall wrapper cannot invent its own mapping. */
W32_DWORD w32_error_from_errno(long err);

/* Map a LIBC call result to Win32.  -1 consults errno (POSIX libc, guest
 * and host alike); any other negative is a raw errno off the syscall.
 * >= 0 yields SUCCESS.  W32A-2: personality code uses this for libc. */
W32_DWORD w32_error_from_c(long r);

#endif /* AURALITE_W32_ERRNO_H */
