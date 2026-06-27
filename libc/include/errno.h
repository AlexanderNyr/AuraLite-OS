#ifndef AURALITE_LIBC_ERRNO_H
#define AURALITE_LIBC_ERRNO_H

/*
 * errno.h — POSIX.1-2017 <errno.h> for AuraLite user programs.
 *
 * `errno` is a modifiable lvalue of type int (POSIX.1-2017 §<errno.h>).  It is
 * set by libc syscall wrappers when a syscall returns a value in the reserved
 * negative-errno band; it is NOT cleared on success.  Applications must only
 * inspect errno after a function has returned its error-indicating value.
 *
 * errno is exposed through a function accessor so that the storage can be made
 * thread-local in Phase P9 (pthreads/TLS) without touching any caller: at that
 * point __errno_location() returns &tcb->errno instead of a global.  Because
 * errno is a macro, application code must never take its address across that
 * change-over (i.e. avoid `&errno`).
 */

/* Returns the address of the current thread's errno cell.
 * Single-threaded for now (returns a global); becomes TLS-backed in P9. */
int *__errno_location(void);

#define errno (*__errno_location())

/* Numeric values match the Linux asm-generic ABI and the kernel-side
 * kernel/lib/errno.h.  Keep the two headers in lock-step. */

/* --- errno-base (1..34) --- */
#define EPERM            1   /* Operation not permitted */
#define ENOENT           2   /* No such file or directory */
#define ESRCH            3   /* No such process */
#define EINTR            4   /* Interrupted system call */
#define EIO              5   /* I/O error */
#define ENXIO            6   /* No such device or address */
#define E2BIG            7   /* Argument list too long */
#define ENOEXEC          8   /* Exec format error */
#define EBADF            9   /* Bad file descriptor */
#define ECHILD          10   /* No child processes */
#define EAGAIN          11   /* Resource temporarily unavailable */
#define ENOMEM          12   /* Cannot allocate memory */
#define EACCES          13   /* Permission denied */
#define EFAULT          14   /* Bad address */
#define ENOTBLK         15   /* Block device required */
#define EBUSY           16   /* Device or resource busy */
#define EEXIST          17   /* File exists */
#define EXDEV           18   /* Invalid cross-device link */
#define ENODEV          19   /* No such device */
#define ENOTDIR         20   /* Not a directory */
#define EISDIR          21   /* Is a directory */
#define EINVAL          22   /* Invalid argument */
#define ENFILE          23   /* Too many open files in system */
#define EMFILE          24   /* Too many open files */
#define ENOTTY          25   /* Inappropriate ioctl for device */
#define ETXTBSY         26   /* Text file busy */
#define EFBIG           27   /* File too large */
#define ENOSPC          28   /* No space left on device */
#define ESPIPE          29   /* Illegal seek */
#define EROFS           30   /* Read-only file system */
#define EMLINK          31   /* Too many links */
#define EPIPE           32   /* Broken pipe */
#define EDOM            33   /* Numerical argument out of domain */
#define ERANGE          34   /* Numerical result out of range */

/* --- generic (35..133, subset implemented so far) --- */
#define EDEADLK         35   /* Resource deadlock would occur */
#define ENAMETOOLONG    36   /* File name too long */
#define ENOLCK          37   /* No record locks available */
#define ENOSYS          38   /* Function not implemented */
#define ENOTEMPTY       39   /* Directory not empty */
#define ELOOP           40   /* Too many symbolic links encountered */
#define ENOMSG          42   /* No message of desired type */
#define EIDRM           43   /* Identifier removed */
#define EOVERFLOW       75   /* Value too large for defined data type */
#define EILSEQ          84   /* Illegal byte sequence */
#define EOPNOTSUPP      95   /* Operation not supported */
#define EADDRINUSE      98   /* Address already in use */
#define EADDRNOTAVAIL   99   /* Cannot assign requested address */
#define ENETDOWN       100   /* Network is down */
#define ENETUNREACH    101   /* Network is unreachable */
#define ECONNRESET     104   /* Connection reset by peer */
#define ENOTCONN       107   /* Transport endpoint is not connected */
#define ETIMEDOUT      110   /* Connection timed out */
#define ECONNREFUSED   111   /* Connection refused */
#define EHOSTUNREACH   113   /* No route to host */
#define EALREADY       114   /* Operation already in progress */
#define EINPROGRESS    115   /* Operation now in progress */
#define ECANCELED      125   /* Operation canceled */

/* POSIX-mandated aliases (same numeric value as their target). */
#define EWOULDBLOCK     EAGAIN
#define EDEADLOCK       EDEADLK
#define ENOTSUP         EOPNOTSUPP

#endif /* AURALITE_LIBC_ERRNO_H */
