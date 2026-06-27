#ifndef AURALITE_LIBC_SYS_STAT_H
#define AURALITE_LIBC_SYS_STAT_H

/*
 * sys/stat.h — file mode bits and stat() declaration (POSIX.1-2017 subset).
 *
 * The `struct stat` layout itself is declared in <unistd.h> (matching the
 * kernel's `struct vfs_stat`); this header adds the mode_t type and the
 * standard S_* permission/type macros plus the stat-family prototypes.
 */

#include <sys/types.h>   /* mode_t */

struct stat;             /* full definition in <unistd.h> */

/* File type bits (st_mode high bits) — octal, POSIX/Linux layout. */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Permission bits. */
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

int stat(const char *path, struct stat *out);
/* NOTE: AuraLite's mkdir() currently takes only a path (see <unistd.h>); the
 * POSIX mode argument arrives with permissions in Phase P7. */

#endif /* AURALITE_LIBC_SYS_STAT_H */
