/*
 * errno.h — POSIX.1-2017 error number definitions (kernel side).
 *
 * AuraLite adopts the Linux in-band negative-errno syscall ABI: kernel
 * functions and syscall handlers return 0 / a non-negative result on success
 * and a *negative* errno value (e.g. -ENOENT) on failure.  The syscall
 * dispatcher places this signed value verbatim into RAX; userspace libc then
 * decodes the reserved error band [-4095, -1] into errno + a -1 return.
 *
 * Numeric values match the Linux asm-generic ABI
 * (include/uapi/asm-generic/errno-base.h and errno.h) so that programs and
 * tooling sharing the Linux convention behave identically.  POSIX.1-2017
 * (IEEE Std 1003.1) §2.3 "Error Numbers" defines the semantics; it does not
 * mandate the numeric values, but pinning them to the Linux ABI keeps the
 * AuraLite syscall ABI stable and self-consistent.
 *
 * This header is intentionally definition-only (no functions, no errno
 * variable): errno itself is userspace state, never kernel state.  The kernel
 * only ever *produces* negative errno values; it never stores a per-process
 * errno.
 */
#ifndef AURALITE_KERNEL_LIB_ERRNO_H
#define AURALITE_KERNEL_LIB_ERRNO_H

/* --- asm-generic errno-base (1..34): the classic Unix V7/POSIX core --- */
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

/* --- asm-generic errno (35..133): everything else --- */
#define EDEADLK         35   /* Resource deadlock would occur */
#define ENAMETOOLONG    36   /* File name too long */
#define ENOLCK          37   /* No record locks available */
#define ENOSYS          38   /* Function not implemented */
#define ENOTEMPTY       39   /* Directory not empty */
#define ELOOP           40   /* Too many symbolic links encountered */
/* 41 deliberately unused: EWOULDBLOCK aliases EAGAIN (11) on Linux. */
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

/* POSIX-mandated aliases.  These MUST be defined as aliases (same numeric
 * value) and MUST NOT be listed as separate designated initializers in any
 * errno-string table (doing so is a duplicate-index initializer). */
#define EWOULDBLOCK     EAGAIN      /* POSIX: may equal EAGAIN; Linux: == 11 */
#define EDEADLOCK       EDEADLK     /* Linux alias: == 35 */
#define ENOTSUP         EOPNOTSUPP  /* POSIX name; Linux: == 95 */

/*
 * MAX_ERRNO — the largest errno value the in-band ABI can encode.
 *
 * The unsigned RAX range [(unsigned long)-MAX_ERRNO, (unsigned long)-1],
 * i.e. [0xFFFFFFFFFFFFF001, 0xFFFFFFFFFFFFFFFF], is reserved for errors.
 * A successful syscall must never return a value in that band.  Because
 * mmap() hands back page-aligned addresses and the kernel never maps the
 * top page, no legitimate pointer or size can fall inside it.
 *
 * Linux include/linux/err.h uses the same constant.
 */
#define MAX_ERRNO       4095

/*
 * errno_is_err() — does an in-band signed kernel return value @ret denote a
 * failure (i.e. lie in the reserved band)?  Compare as unsigned to avoid the
 * classic `ret < 0` bug when the carrier type is unsigned.
 */
static inline int errno_is_err(long ret)
{
    return (unsigned long)ret >= (unsigned long)(-MAX_ERRNO);
}

#endif /* AURALITE_KERNEL_LIB_ERRNO_H */
