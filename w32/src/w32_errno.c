/* w32_errno.c — the last-error slot and errno translation.
 * WIN32_PLAN.md phase W32-4.
 */

#include "w32/w32_errno.h"

#include <errno.h>

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
    case 13: return W32_ERROR_ACCESS_DENIED;      /* EACCES */
    case 14: return W32_ERROR_INVALID_DATA;        /* EFAULT */
    /* W32A-2: the file/system surface leans on errno heavily, so the map
     * grows teeth.  ENOSPC moves from NOT_ENOUGH_MEMORY (a W32-4
     * approximation from before DISK_FULL existed) to DISK_FULL. */
    case 17: return W32_ERROR_ALREADY_EXISTS;      /* EEXIST */
    case 18: return W32_ERROR_NOT_SAME_DEVICE;     /* EXDEV */
    case 21: return W32_ERROR_ACCESS_DENIED;       /* EISDIR */
    case 23: return W32_ERROR_TOO_MANY_OPEN_FILES; /* ENFILE */
    case 28: return W32_ERROR_DISK_FULL;           /* ENOSPC */
    case 29: return W32_ERROR_SEEK;                /* ESPIPE */
    case 30: return W32_ERROR_WRITE_PROTECT;       /* EROFS */
    case 32: return W32_ERROR_BROKEN_PIPE;         /* EPIPE */
    case 36: return W32_ERROR_FILENAME_EXCED_RANGE;/* ENAMETOOLONG */
    case 39: return W32_ERROR_DIR_NOT_EMPTY;       /* ENOTEMPTY */
    case 20: return W32_ERROR_PATH_NOT_FOUND;      /* ENOTDIR */
    case 22: return W32_ERROR_INVALID_PARAMETER;   /* EINVAL */
    case 24: return W32_ERROR_TOO_MANY_OPEN_FILES; /* EMFILE */
    case 38: return W32_ERROR_NOT_SUPPORTED;       /* ENOSYS */
    default: return W32_ERROR_INVALID_FUNCTION;
    }
}

/* The libc-aware wrapper.  Both AuraLite's libc and the host's return -1
 * with errno set, so one translation serves the guest build and the host
 * unit tests.  (A caller holding a raw negative from a direct syscall --
 * none in-tree -- uses w32_error_from_errno instead.) */
W32_DWORD w32_error_from_c(long r) {
    if (r >= 0)
        return W32_ERROR_SUCCESS;
    if (r == -1)
        return w32_error_from_errno(-(long)errno);
    /* Any other negative is a raw errno straight off the syscall (no libc
     * returns one, but direct callers and test doubles do). */
    return w32_error_from_errno(r);
}
