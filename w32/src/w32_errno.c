/* w32_errno.c — the last-error slot and errno translation.
 * WIN32_PLAN.md phase W32-4.
 */

#include "w32/w32_errno.h"

/* Win32 makes this thread-local.  A w32 process is single-threaded today, so
 * a process-wide slot is exact rather than an approximation; when threads
 * arrive this moves to TLS.  Recorded in the plan rather than pre-built. */
static W32_DWORD last_error = W32_ERROR_SUCCESS;

void w32_set_last_error(W32_DWORD code) { last_error = code; }
W32_DWORD w32_get_last_error_raw(void)  { return last_error; }

/* AuraLite returns negative errno values from its syscalls.  Mapping them in
 * one place stops each wrapper inventing its own translation, which is how
 * two functions end up reporting different codes for the same failure. */
W32_DWORD w32_error_from_errno(long err) {
    if (err >= 0) return W32_ERROR_SUCCESS;
    switch (-err) {
    case 1:  return W32_ERROR_INVALID_FUNCTION;    /* EPERM  */
    case 2:  return W32_ERROR_FILE_NOT_FOUND;      /* ENOENT */
    case 9:  return W32_ERROR_INVALID_HANDLE;      /* EBADF  */
    case 12: return W32_ERROR_NOT_ENOUGH_MEMORY;   /* ENOMEM */
    case 13: return W32_ERROR_ACCESS_DENIED;       /* EACCES */
    case 14: return W32_ERROR_INVALID_DATA;        /* EFAULT */
    case 20: return W32_ERROR_PATH_NOT_FOUND;      /* ENOTDIR */
    case 22: return W32_ERROR_INVALID_PARAMETER;   /* EINVAL */
    case 24: return W32_ERROR_TOO_MANY_OPEN_FILES; /* EMFILE */
    case 28: return W32_ERROR_NOT_ENOUGH_MEMORY;   /* ENOSPC */
    case 38: return W32_ERROR_NOT_SUPPORTED;       /* ENOSYS */
    default: return W32_ERROR_INVALID_FUNCTION;
    }
}
