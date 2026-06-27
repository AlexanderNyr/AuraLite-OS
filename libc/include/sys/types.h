#ifndef AURALITE_LIBC_SYS_TYPES_H
#define AURALITE_LIBC_SYS_TYPES_H

/*
 * sys/types.h — POSIX system data types for AuraLite user programs.
 *
 * ssize_t and pid_t are also typedef'd in <unistd.h>; the guard macros below
 * ensure each type is defined exactly once regardless of include order.
 */

#include <stdint.h>
#include <stddef.h>

#ifndef AURALITE_TYPE_SSIZE_T
#define AURALITE_TYPE_SSIZE_T
typedef int64_t ssize_t;
#endif

#ifndef AURALITE_TYPE_PID_T
#define AURALITE_TYPE_PID_T
typedef int64_t pid_t;
#endif

typedef uint32_t mode_t;
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int64_t  off_t;
typedef int64_t  blksize_t;
typedef int64_t  blkcnt_t;

#endif /* AURALITE_LIBC_SYS_TYPES_H */
