#include "kernel/proc/clone_decls.h"
#include "kernel/fs/p10_decls.h"
#include <stdint.h>
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/process.h"
#include "kernel/proc/signal.h"
#include "kernel/arch/x86_64/isr.h"
#include "kernel/proc/usercopy.h"
#include "kernel/fs/vfs.h"
#include "kernel/tty/termios.h"
#include "kernel/tty/tty.h"
#include "kernel/net/net.h"
#include "kernel/net/tcp.h"
#include "kernel/net/socket.h"
#include "kernel/net/dns.h"
#include "kernel/net/ipv6.h"
#include "drivers/uart/uart.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/keyboard/keymap.h"
#include "drivers/timer/pit.h"
#include "kernel/gui/gui_syscalls.h"
#include "kernel/gpu/gpu_syscalls.h"
#include "kernel/time.h"
#include "kernel/sync/futex.h"
// clone.c compiled separately
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/mprotect.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/vma.h"
#include "kernel/boot_info.h"
#include "kernel/rng.h"
#include "kernel/ipc/sysvipc.h"
#include "kernel/mm/shmem.h"
#include "kernel/mm/page_cache.h"

/* P10 types */
typedef struct {
    uint64_t fds_bits[64 / 64];
} fd_set;

/* struct kernel_timeval is provided by kernel/time.h (included above). */

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_GETPID 39
#define SYS_FORK   57
#define SYS_EXECVE 59
#define SYS_EXIT   60
#define SYS_WAIT4  61
#define SYS_SPAWN  81   /* non-standard: spawn a program in a new address space */
#define SYS_DNS    82   /* non-standard: resolve a hostname */
#define SYS_NET_CONNECT 83  /* non-standard: TCP connect */
#define SYS_NET_SEND    84  /* non-standard: TCP send */
#define SYS_NET_RECV    85  /* non-standard: TCP recv */
#define SYS_NET_CLOSE   86  /* non-standard: TCP close */
#define SYS_NET_PING    87  /* non-standard: ICMP ping */
#define SYS_PING6       610 /* X7: ICMPv6 echo to a link-local neighbour */
#define SYS_DNSCTL     107  /* X3: DNS cache/server control (see kernel/net/dns.h) */
#define SYS_LISTDIR 80
/* Filesystem extensions (non-standard numbers). */
#define SYS_MKDIR    100
#define SYS_RMDIR    101
#define SYS_UNLINK   102
#define SYS_RENAME   103
#define SYS_TRUNCATE 104
#define SYS_STAT     105
#define SYS_MKFIFO   106  /* N5.2: named FIFO */
#define SYS_MMAP     9
#define SYS_MUNMAP   11
#define SYS_MPROTECT  10
#define SYS_MSYNC     26   /* A6: Linux ABI number */
#define SYS_BRK      12
#define SYS_LSEEK    8    /* P3 */
#define SYS_IOCTL   16    /* P5 */
#define SYS_SIGACTION   13   /* P4 */
#define SYS_SIGPROCMASK 14   /* P4 */
#define SYS_SIGRETURN   15   /* P4 */
#define SYS_KILL        62   /* P4 */
#define SYS_SIGPENDING 127   /* P4 */
#define SYS_PAUSE       34   /* P4 */
#define SYS_ALARM       37   /* P4 */
#define SYS_SENDTO      44   /* N2.3b: UDP sockets */
#define SYS_RECVFROM    45   /* N2.3b: UDP sockets */
#define SYS_SIGSUSPEND 130   /* P4 */
#define SYS_SETPGID    109   /* P6 */
#define SYS_GETPGID    121   /* P6 */
#define SYS_SETSID     112   /* P6 */
#define SYS_GETSID     124   /* P6 */
#define SYS_PREAD64  17   /* P3 */
#define SYS_PWRITE64 18   /* P3 */
#define SYS_READV    19   /* P3 */
#define SYS_WRITEV   20   /* P3 */
#define SYSCALL_IOV_MAX 1024
/* Q16: Issue-8 tail.  318 = SYS_GETENTROPY (Q11); 319..321 are free. */
#define SYS_GETRANDOM   319
#define SYS_PSELECT6    320
#define SYS_PPOLL       321
/* Q14: System V IPC — Linux syscall numbers (64..71, 29..31, 67). */
#define SYS_SEMGET   64
#define SYS_SEMOP    65
#define SYS_SEMCTL   66
#define SYS_MSGGET   68
#define SYS_MSGSND   69
#define SYS_MSGRCV   70
#define SYS_MSGCTL   71
#define SYS_SHMGET   29
#define SYS_SHMAT    30
#define SYS_SHMCTL   31
#define SYS_SHMDT    67
/* Socket-style networking API. */
#define SYS_SOCKET        300
#define SYS_SOCKET_CONNECT 301
#define SYS_SOCKET_SEND    302
#define SYS_SOCKET_RECV    303
#define SYS_SOCKET_CLOSE   304
#define SYS_SOCKET_BIND    305
#define SYS_SOCKET_LISTEN  306
#define SYS_SOCKET_ACCEPT  307
#define SYS_SOCKET_CONNECT6 308   /* Y3: connect(sockaddr_in6) */
#define SYS_DNS_AAAA        309   /* Y4: resolve AAAA into 16 octets */

/* File-descriptor extensions. */
#define SYS_DUP    32
#define SYS_DUP2   33
#define SYS_PIPE   22
#define SYS_FCNTL  72
#define SYS_PIPE2  293   /* P2: pipe with O_CLOEXEC/O_NONBLOCK */

/* P7: User & Group Credentials */
#define SYS_GETUID    500
#define SYS_GETEUID   501
#define SYS_GETGID    502
#define SYS_GETEGID   503
#define SYS_SETUID    504
#define SYS_SETGID    505
#define SYS_SETREUID  506
#define SYS_SETREGID  507
#define SYS_GETGROUPS 508
#define SYS_SETGROUPS 509
#define SYS_CHMOD     510
#define SYS_CHOWN     511
#define SYS_UMASK     512
#define SYS_ACCESS    513
#define SYS_FCHMOD    514
#define SYS_FCHOWN    515

/* P10: Additional syscalls */
#define SYS_SELECT        23
#define SYS_POLL           7
#define SYS_FSTAT          5
#define SYS_LSTAT          6
#define SYS_SYMLINK       88
#define SYS_READLINK      89
/* SYS_LINK was erroneously 86, colliding with SYS_NET_CLOSE; moved to the free
 * slot 90 next to its symlink/readlink siblings.  Still undispatched (link(2)
 * returns -ENOSYS via the symlink.c stub), but the number is now collision-free
 * for when link(2) is wired up. */
#define SYS_LINK          90
#define SYS_FTRUNCATE     77
#define SYS_FSYNC         74
#define SYS_GETDENTS64   217
#define SYS_GETCWD       540
#define SYS_CHDIR        541
#define SYS_FCHDIR       542
#define SYS_UNAME         63
#define SYS_GETRLIMIT     97

/* P8: Clocks & Timers */
#define SYS_NANOSLEEP      35
#define SYS_GETITIMER      36
#define SYS_SETITIMER      38
#define SYS_GETTIMEOFDAY   96
#define SYS_CLOCK_GETTIME  228
#define SYS_CLOCK_GETRES   229
#define SYS_TIME           520

/* P9: Threads */
#define SYS_CLONE          56
#define SYS_ARCH_PRCTL     158
#define SYS_FUTEX          530
#define SYS_TKILL          531
#define SYS_MADVISE        28
#define SYS_MINCORE        27
#define SYS_MLOCK          149
#define SYS_MUNLOCK        150
#define SYS_MEMINFO        600   /* non-standard: returns pmm_get_free_frames() to userspace */
#define SYS_KBD_LAYOUT     601   /* non-standard: select keyboard layout (FIXES_PLAN R8) */

/* fcntl command numbers and the open-flag / FD_CLOEXEC values come from
 * kernel/fs/vfs.h (Linux/asm-generic ABI). */

#define SYSCALL_PATH_MAX 256

/* Q12: AT_* flags mirroring lib/libc/include/fcntl.h. */
#define AT_FDCWD           (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000

/* Q13: AT-family completion syscall numbers (Linux-compatible, free here). */
#define SYS_MKNODAT       259
#define SYS_LINKAT        265
#define SYS_SYMLINKAT     266
#define SYS_UTIMENSAT     280

/* Q13: POSIX st_mode file-type bits for mknodat (octal, Linux layout). */
#define Q13_S_IFMT   0170000u
#define Q13_S_IFIFO  0010000u
#define Q13_S_IFREG  0100000u
#define Q13_S_IFCHR  0020000u
#define Q13_S_IFBLK  0060000u

/* Q13: utimensat tv_nsec sentinels (POSIX.1-2024, Linux values). */
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

/* Q13: resolve the user times[2] array (or NULL == both "now") into Unix
 * seconds, honoring UTIME_NOW / UTIME_OMIT.  The VFS stores second-
 * granularity timestamps (vfs_now()); nanosecond values are validated and
 * rounded, never silently truncated.  (uint64_t)-1 means "keep" (OMIT). */
struct q13_timespec { long tv_sec; long tv_nsec; };
static int utimens_resolve(uint64_t user_times, uint64_t *atime, uint64_t *mtime) {
    if (user_times == 0) {
        uint64_t now = vfs_now();
        *atime = now;
        *mtime = now;
        return 0;
    }
    struct q13_timespec ts[2];
    if (copy_from_user(ts, (const uint8_t *)(uintptr_t)user_times, sizeof(ts)) != 0)
        return -EFAULT;
    for (int i = 0; i < 2; i++) {
        if (ts[i].tv_nsec != UTIME_NOW && ts[i].tv_nsec != UTIME_OMIT &&
            (ts[i].tv_nsec < 0 || ts[i].tv_nsec >= 1000000000L))
            return -EINVAL;
    }
    uint64_t now = vfs_now();
    for (int i = 0; i < 2; i++) {
        uint64_t *out = (i == 0) ? atime : mtime;
        if (ts[i].tv_nsec == UTIME_OMIT)       *out = (uint64_t)-1;
        else if (ts[i].tv_nsec == UTIME_NOW)   *out = now;
        else {
            *out = (uint64_t)ts[i].tv_sec;
            if (ts[i].tv_nsec >= 500000000L) (*out)++;   /* round, don't drop */
        }
    }
    return 0;
}
#define SYSCALL_IO_CHUNK 256

/* Keep the heap well below the user stack so brk growth cannot collide with
 * the fixed high-address stack mapping. */
#define USER_STACK_TOP        0x7FFFF0000000ULL
/* SELFHOST SH1: 4 MiB usable user stack (was 1 MiB); see guard.c. */
#define USER_STACK_SIZE       0x400000ULL  /* 4 MiB usable user stack */
#define USER_STACK_GUARD_SIZE 0x1000ULL
#define USER_MMAP_BASE        0x0000400000000000ULL
#define USER_MMAP_MAX         0x0000700000000000ULL
#define USER_BRK_GUARD_GAP    (2ULL * 1024ULL * 1024ULL)
#define USER_BRK_MAX          (USER_MMAP_BASE - USER_BRK_GUARD_GAP)

#define PROT_READ             0x1
#define PROT_WRITE            0x2
#define PROT_EXEC             0x4
#define MAP_SHARED            0x01
#define MAP_PRIVATE           0x02
#define MAP_FIXED             0x10
#define MAP_ANONYMOUS         0x20

static int copy_user_path(char *dst, uint64_t user_path) {
    return copy_string_from_user(dst, (const char *)(uintptr_t)user_path,
                                 SYSCALL_PATH_MAX);
}

/* Q12 (POSIX2024_PLAN.md phase Q12): the stat-family syscalls must deliver
 * the POSIX st_mode type bits (S_IFMT), not just the permission bits the
 * VFS stores.  The struct vfs_stat layout is shared with user space
 * (lib/libc/include/unistd.h), so the type field stays, and st_mode gains
 * the high bits S_ISREG/S_ISDIR/... test for. */
static uint32_t stat_posix_mode(const struct vfs_stat *st) {
    uint32_t m = st->mode & 07777u;
    switch (st->type) {
        case VFS_TYPE_FILE:    m |= 0100000u; break;
        case VFS_TYPE_DIR:     m |= 0040000u; break;
        case VFS_TYPE_CHARDEV: m |= 0020000u; break;
        case VFS_TYPE_SYMLINK: m |= 0120000u; break;
        case VFS_TYPE_FIFO:    m |= 0010000u; break;
        default: break;
    }
    return m;
}

/* Q12: AT-family path resolution.  POSIX allows a relative path with
 * dirfd == AT_FDCWD to resolve against the caller's working directory.  The
 * VFS layer has no cwd-relative open path, so the dispatcher joins the
 * thread's cwd here.  Real directory fds (dirfd != AT_FDCWD) with relative
 * paths remain ENOSYS until the VFS grows open-directory resolution --
 * declared honestly in the plan rather than silently faked.  Returns 0 on
 * success (dst filled), -EFAULT on a bad user pointer, -ENOSYS on a real
 * dirfd with a relative path. */
static int copy_at_path(char *dst, uint64_t user_path, int dirfd) {
    char tmp[SYSCALL_PATH_MAX];
    if (copy_user_path(tmp, user_path) != 0) return -EFAULT;
    if (tmp[0] == '/') {
        memcpy(dst, tmp, strlen(tmp) + 1);
        return 0;
    }
    if (dirfd != AT_FDCWD) return -ENOSYS;
    tcb_t *cur = sched_current();
    const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
    size_t cl = strlen(cwd);
    size_t tl = strlen(tmp);
    if (cl + 1 + tl + 1 > SYSCALL_PATH_MAX) return -ENAMETOOLONG;
    if (cwd[cl - 1] == '/') {
        memcpy(dst, cwd, cl);
        memcpy(dst + cl, tmp, tl + 1);
    } else {
        memcpy(dst, cwd, cl);
        dst[cl] = '/';
        memcpy(dst + cl + 1, tmp, tl + 1);
    }
    return 0;
}

/*
 * vfs_errno() — map a generic kernel return @ret to an in-band errno value.
 *
 * The current VFS/process/net layers report failure with a bare -1 and do not
 * yet distinguish causes.  Until those layers grow specific errno returns
 * (tracked in TODO.md, "errno granularity"), the dispatcher substitutes a
 * caller-supplied @fallback errno for any generic negative return so userspace
 * sees a meaningful errno instead of a raw -1.  A return that is already a
 * proper negative errno (in the reserved band) is passed through unchanged.
 *
 * TRAP: EPERM IS 1, SO -EPERM IS -1
 *
 * This function cannot distinguish "operation not permitted" from the generic
 * failure sentinel, and will replace the former with @fallback.  Any caller
 * whose callee can return -EPERM must NOT be wrapped — return the value
 * directly.  This cost real debugging time when the installation policy
 * (FSLAYOUT_PLAN F1) started returning EPERM from vfs_open() and userspace
 * saw ENOENT, which reads as "no such file" for a file the caller was in the
 * middle of creating.
 *
 * @ret      kernel return value (>= 0 success, < 0 failure)
 * @fallback positive errno to use when @ret is the generic -1
 * Returns @ret on success, or a negative errno on failure.
 */
static int64_t vfs_errno(int64_t ret, int fallback) {
    if (ret >= 0) return ret;
    if (ret == -1) return -(int64_t)fallback;
    /* Already a specific negative errno (e.g. -ENOENT). */
    if (errno_is_err((long)ret)) return ret;
    return -(int64_t)fallback;
}

/* Returns bytes written (>= 0) or a negative errno (-EFAULT / ...). */
static int64_t syscall_vfs_write(int fd, const void *user_buf, uint64_t len) {
    if (len == 0) return 0;
    if (!validate_user_range(user_buf, len, 0)) return -EFAULT;

    char tmp[SYSCALL_IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        if (copy_from_user(tmp, (const uint8_t *)user_buf + done, n) != 0) {
            return -EFAULT;
        }
        int64_t wr = vfs_write(fd, tmp, n);
        if (wr < 0) return (done > 0) ? (int64_t)done : wr;
        done += (uint64_t)wr;
        if ((uint64_t)wr < n) break;
    }
    return (int64_t)done;
}

/* Returns bytes read (>= 0) or a negative errno (-EFAULT / ...). */
static int64_t syscall_vfs_read(int fd, void *user_buf, uint64_t len) {
    if (len == 0) return 0;
    if (!validate_user_range(user_buf, len, 1)) return -EFAULT;

    char tmp[SYSCALL_IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        int64_t rd = vfs_read(fd, tmp, n);
        if (rd < 0) return (done > 0) ? (int64_t)done : rd;
        if (rd == 0) break;
        if (copy_to_user((uint8_t *)user_buf + done, tmp, (uint64_t)rd) != 0) {
            return -EFAULT;
        }
        done += (uint64_t)rd;
        if ((uint64_t)rd < n) break;
    }
    return (int64_t)done;
}

/* Positional read: copy from VFS @offset into a user buffer, no pos change. */
static int64_t syscall_vfs_pread(int fd, void *user_buf, uint64_t len,
                                 int64_t offset) {
    if (len == 0) return 0;
    if (!validate_user_range(user_buf, len, 1)) return -EFAULT;
    char tmp[SYSCALL_IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        int64_t rd = vfs_pread(fd, tmp, n, offset + (int64_t)done);
        if (rd < 0) return (done > 0) ? (int64_t)done : rd;
        if (rd == 0) break;
        if (copy_to_user((uint8_t *)user_buf + done, tmp, (uint64_t)rd) != 0) {
            return -EFAULT;
        }
        done += (uint64_t)rd;
        if ((uint64_t)rd < n) break;
    }
    return (int64_t)done;
}

/* Positional write: copy from a user buffer to VFS @offset, no pos change. */
static int64_t syscall_vfs_pwrite(int fd, const void *user_buf, uint64_t len,
                                  int64_t offset) {
    if (len == 0) return 0;
    if (!validate_user_range(user_buf, len, 0)) return -EFAULT;
    char tmp[SYSCALL_IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        if (copy_from_user(tmp, (const uint8_t *)user_buf + done, n) != 0) {
            return -EFAULT;
        }
        int64_t wr = vfs_pwrite(fd, tmp, n, offset + (int64_t)done);
        if (wr < 0) return (done > 0) ? (int64_t)done : wr;
        done += (uint64_t)wr;
        if ((uint64_t)wr < n) break;
    }
    return (int64_t)done;
}

static uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static int user_mmap_range_ok(uint64_t addr, uint64_t len) {
    if (len == 0) return 0;
    if (addr & (PAGE_SIZE_BYTES - 1ULL)) return 0;
    if (addr < USER_MMAP_BASE || addr >= USER_MMAP_MAX) return 0;
    if (len > USER_MMAP_MAX - addr) return 0;
    return 1;
}

static int user_range_is_free(uint64_t addr, uint64_t len) {
    for (uint64_t off = 0; off < len; off += PAGE_SIZE_BYTES) {
        if (paging_get_phys(addr + off) != 0) return 0;
    }
    return 1;
}

static uint64_t syscall_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                             uint64_t flags, uint64_t fd, uint64_t off) {
    tcb_t *cur = sched_current();
    if (!cur || len == 0) return (uint64_t)-EINVAL;

    /* Validation of prot and flags (as before). */
    int anonymous = (flags & MAP_ANONYMOUS) ? 1 : 0;
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 || prot == 0) {
        return (uint64_t)-EINVAL;
    }
    if (!(flags & (MAP_SHARED | MAP_PRIVATE))) return (uint64_t)-EINVAL;
    /* A6: MAP_SHARED + file-backed is supported now.  It had been ENOSYS
     * "pending page cache writeback from M9", but the page cache and the
     * fault path were both already there -- the only thing missing was
     * anything that set the dirty bit (see page_cache_mark_dirty()).
     * MAP_SHARED + anonymous continues to go through shmem. */
    if (anonymous) {
        if (fd != (uint64_t)-1) return (uint64_t)-EINVAL;
    } else {
        if (fd == (uint64_t)-1 || (off & (PAGE_SIZE_BYTES - 1ULL)) ||
            off > 0x7FFFFFFFFFFFFFFFULL) {
            return (uint64_t)-EINVAL;
        }
    }

    len = align_up_u64(len, PAGE_SIZE_BYTES);
    if (len == 0 || len > (USER_MMAP_MAX - USER_MMAP_BASE)) {
        return (uint64_t)-EINVAL;
    }

    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE_BYTES - 1ULL)) return (uint64_t)-EINVAL;
        if (!user_mmap_range_ok(addr, len) || !user_range_is_free(addr, len)) {
            return (uint64_t)-ENOMEM;
        }
    } else {
        uint64_t start = cur->mmap_next ? cur->mmap_next : USER_MMAP_BASE;
        if (addr >= USER_MMAP_BASE && addr < USER_MMAP_MAX) start = addr;
        start = align_up_u64(start, PAGE_SIZE_BYTES);

        addr = 0;
        for (uint64_t candidate = start;
             candidate >= USER_MMAP_BASE && candidate + len <= USER_MMAP_MAX;
             candidate += len) {
            if (user_range_is_free(candidate, len)) {
                addr = candidate;
                break;
            }
            if (candidate > USER_MMAP_MAX - len - PAGE_SIZE_BYTES) break;
        }
        if (addr == 0) return (uint64_t)-ENOMEM;
        cur->mmap_next = addr + len;
    }

    /* Determine VMA flags. */
    uint32_t vflags = anonymous ? VMA_ANON : VMA_FILE;
    if (prot & PROT_READ)  vflags |= VMA_READ;
    if (prot & PROT_WRITE) vflags |= VMA_WRITE;
    if (prot & PROT_EXEC)  vflags |= VMA_EXEC;
    if (flags & MAP_SHARED) vflags |= VMA_SHARED;

    struct ofd *file_ofd = NULL;
    if (!anonymous) {
        if (fd >= VFS_MAX_FDS || cur->fd_table[fd] == NULL) {
            return (uint64_t)-EBADF;
        }
        file_ofd = cur->fd_table[fd];
    }

    /* Create the VMA descriptor. No physical allocation here (lazy fault). */
    {
        uint64_t vf = spinlock_acquire_irqsave(&cur->vma_lock);
        int vr;
        if (anonymous && (flags & MAP_SHARED)) {
            /* M4: MAP_SHARED|MAP_ANONYMOUS — create a shmem object so
             * multiple processes can share the same physical frames.
             * The shmid is stored in the VMA; the fault handler resolves
             * pages through shmem_get_or_alloc(). */
            int shmid = shmem_create(len);
            if (shmid < 0) {
                spinlock_release_irqrestore(&cur->vma_lock, vf);
                return (uint64_t)-ENOMEM;
            }
            vr = vma_insert_shmem(&cur->vma_list, addr, addr + len,
                                  vflags, shmid);
        } else {
            vr = vma_insert(&cur->vma_list, addr, addr + len,
                            vflags, file_ofd, off);
        }
        spinlock_release_irqrestore(&cur->vma_lock, vf);
        if (vr != 0) {
            return (uint64_t)-ENOMEM;
        }
    }

    /* MAP_POPULATE: eager allocation (optional). */
    if (flags & 0x4000) { /* MAP_POPULATE */
        uint64_t hhdm = boot_get_hhdm_offset();
        uint64_t mapped = 0;
        uint64_t pte_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
        if (prot & PROT_WRITE) pte_flags |= PAGE_FLAG_WRITABLE;
        if (!(prot & PROT_EXEC)) pte_flags |= PAGE_FLAG_NO_EXEC;

        for (; mapped < len; mapped += PAGE_SIZE_BYTES) {
            uint64_t phys = pmm_alloc_frame();
            if (!phys) {
                /* Partial populate: we just stop and return the addr. */
                break;
            }
            memset((void *)(uintptr_t)(hhdm + phys), 0, PAGE_SIZE_BYTES);
            if (!anonymous && file_ofd) {
                vfs_read_at_phys(file_ofd, off + mapped, phys, PAGE_SIZE_BYTES);
            }
            paging_map(addr + mapped, phys, pte_flags);
        }
    }

    return addr;
}

static uint64_t syscall_munmap(uint64_t addr, uint64_t len) {
    tcb_t *cur = sched_current();
    if (!cur || len == 0) return (uint64_t)-EINVAL;
    if (addr & (PAGE_SIZE_BYTES - 1ULL)) return (uint64_t)-EINVAL;
    len = align_up_u64(len, PAGE_SIZE_BYTES);
    if (!user_mmap_range_ok(addr, len)) return (uint64_t)-EINVAL;

    /* A6: flush a shared file mapping before it goes away, and remember
     * whether these frames belong to the page cache.  Freeing a page-cache
     * frame here would hand a frame that other mappings (and the cache
     * itself) still reference back to the PMM. */
    int shared_file = 0;
    {
        uint64_t vf = spinlock_acquire_irqsave(&cur->vma_lock);
        vma_t *v = vma_find(cur->vma_list, addr);
        struct ofd *shared_ofd = NULL;
        uint64_t file_off = 0;
        if (v && (v->flags & VMA_SHARED) && (v->flags & VMA_FILE) &&
            !(v->flags & VMA_SHMEM) && v->file) {
            shared_file = 1;
            shared_ofd = v->file;
            file_off = v->file_off + (addr - v->va_start);
            vfs_ofd_get(shared_ofd);
        }
        vma_remove_range(&cur->vma_list, addr, addr + len);
        spinlock_release_irqrestore(&cur->vma_lock, vf);

        if (shared_ofd) {
            page_cache_flush_range(shared_ofd, file_off, file_off + len);
            vfs_ofd_put(shared_ofd);
        }
    }

    for (uint64_t off = 0; off < len; off += PAGE_SIZE_BYTES) {
        uint64_t virt = addr + off;
        uint64_t phys = paging_get_phys(virt);
        if (phys) {
            paging_unmap(virt);
            if (!shared_file) pmm_free_frame(phys);
        }
    }

    return 0;
}

static uint64_t syscall_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    tcb_t *cur = sched_current();
    if (!cur || len == 0) return (uint64_t)-EINVAL;
    if (addr & (PAGE_SIZE_BYTES - 1ULL)) return (uint64_t)-EINVAL;
    len = align_up_u64(len, PAGE_SIZE_BYTES);

    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 || prot == 0) {
        return (uint64_t)-EINVAL;
    }

    uint64_t vf = spinlock_acquire_irqsave(&cur->vma_lock);
    if (mprotect_update_vma_range(cur->vma_list, addr, len, prot) != 0) {
        spinlock_release_irqrestore(&cur->vma_lock, vf);
        return (uint64_t)-ENOMEM;
    }
    spinlock_release_irqrestore(&cur->vma_lock, vf);

    mprotect_remap_present_pages(addr, len, prot);
    return 0;
}

/* Saved user-mode RIP/RFLAGS from the syscall_entry asm stub.  The stub
 * stores them into the current CPU's struct cpu_local slots (they used to
 * be .data globals in syscall_entry.asm -- see the SMP MODEL comment there);
 * the syscall_saved_* macros in syscall.h expand to those per-CPU slots.
 * We read them once at the top of every dispatch and copy them into the
 * current TCB so that a nested syscall from a context-switch partner can
 * safely overwrite the slots.  On the way out syscall_restore_user_frame()
 * copies them back so the asm sysret prologue lands at the right user RIP.
 * This is also what makes a thread that was preempted mid-syscall and
 * resumed on a DIFFERENT cpu safe: it re-enters the exit path here, which
 * republishes its frame from the TCB into whatever cpu it now runs on. */

/* Publish the kernel stack top the SYSCALL entry stub switches to, into the
 * CURRENT cpu's per-CPU slot (was an asm routine writing a single global,
 * which let a thread switch on one CPU clobber the other CPU's entry stack
 * pointer under real SMP).  Called on every context switch (schedule()) and
 * at thread first-run (user.c/clone.c/process.c). */
void set_syscall_stack(uint64_t stack_top) {
    struct cpu_local *c = get_cpu_local();
    if (c) c->syscall_kernel_rsp = stack_top;
}

/* Called from syscall_entry.asm just before sysret.  Refreshes the current
 * CPU's per-CPU syscall slots from the current TCB's per-thread copies.
 * Uses the default SysV C ABI: no args, no return. */
void syscall_restore_user_frame(void) {
    tcb_t *cur = sched_current();
    if (!cur) return;
    if (cur->saved_user_rip) {
        syscall_saved_rcx = cur->saved_user_rip;
        syscall_saved_r11 = cur->saved_user_rflags;
        syscall_saved_rsp = cur->saved_user_rsp;
        syscall_saved_rbx = cur->saved_user_rbx;
        syscall_saved_rbp = cur->saved_user_rbp;
        syscall_saved_r12 = cur->saved_user_r12;
        syscall_saved_r13 = cur->saved_user_r13;
        syscall_saved_r14 = cur->saved_user_r14;
        syscall_saved_r15 = cur->saved_user_r15;
    }
}

/* iretq slow path (kernel/arch/x86_64/syscall_sigreturn.asm). */
extern void syscall_iret_to_user(struct registers *frame) __attribute__((noreturn));

/*
 * syscall_check_signals() — called from syscall_entry.asm just before SYSRET,
 * with the syscall return value @retval.  If the current thread has a pending
 * unblocked signal, synthesise a register frame from the saved syscall-return
 * state, set up the handler frame, and return to user via IRETQ (this does not
 * return).  Otherwise return normally and let the asm SYSRET fast path run.
 *
 * USER_CS/USER_SS match the syscall ABI's SYSRET target selectors.
 */
#define SYSCALL_USER_CS 0x23
#define SYSCALL_USER_SS 0x1B

void syscall_check_signals(uint64_t retval) {
    tcb_t *cur = sched_current();
    if (!cur) return;
    if (!signal_pending_current()) return;

    /* Synthesise the user-return frame the SYSRET fast path would have used. */
    struct registers r;
    memset(&r, 0, sizeof(r));
    r.rip    = syscall_saved_rcx;        /* user RIP after SYSCALL */
    r.rflags = syscall_saved_r11;        /* user RFLAGS */
    r.rsp    = syscall_saved_rsp;        /* user RSP */
    r.rax    = retval;                   /* syscall return value */
    r.cs     = SYSCALL_USER_CS;
    r.ss     = SYSCALL_USER_SS;
    /* Restore the live user callee-saved (SysV-preserved) registers.  The
     * SYSCALL ABI does not clobber RBX/RBP/R12-R15, so userspace may hold live
     * values in them across the syscall; they were captured at the syscall
     * boundary (and survive yields via the TCB snapshot).  Recording them in
     * the signal frame is essential: sigreturn restores exactly these values
     * to the interrupted context, and zeroing them would corrupt the program
     * (e.g. a pointer kept in RBP/R15 across sigaction()).  Caller-saved GPRs
     * (RAX/RCX/RDX/RSI/RDI/R8-R11) are genuinely dead across a syscall and may
     * stay zeroed. */
    r.rbx = syscall_saved_rbx;
    r.rbp = syscall_saved_rbp;
    r.r12 = syscall_saved_r12;
    r.r13 = syscall_saved_r13;
    r.r14 = syscall_saved_r14;
    r.r15 = syscall_saved_r15;

    if (signal_deliver_iret(&r)) {
        /* A handler frame was installed in @r; enter it via IRETQ. */
        syscall_iret_to_user(&r);        /* noreturn */
    }
    /* No deliverable signal after all (e.g. default-ignore): fall through to
     * the normal SYSRET fast path — which reloads rcx/r11/rsp from the
     * PER-CPU saved-user slots (syscall_entry.asm).
     *
     * FIX_R6: control can also reach here on the way OUT of a job-control
     * stop: DFL_STOP parks the thread inside signal_deliver_iret() for an
     * unbounded time, and while it is parked any syscall another thread
     * makes on this cpu overwrites the per-CPU slots with ITS frame.  A
     * SIGCONT later wakes the stopped thread, it returns here, and the
     * SYSRET would land it in the foreign user context (observed: a
     * Ctrl+Z-then-fg'ed ticker resumed into the shell's frame, executed
     * garbage, corrupted its own TLS self-pointer, and page-faulted storing
     * errno).  Every OTHER in-syscall park (nanosleep/select/...) blocks
     * inside syscall_dispatch and therefore passes back through
     * syscall_restore_user_frame() above when it wakes; the signal-check
     * tail is the one boundary that has no such re-publish, so do it here.
     * Idempotent for the no-stop paths. */
    syscall_restore_user_frame();
}

int is_restartable(uint64_t num) {
    switch (num) {
        case SYS_READ:
        case SYS_WRITE:
        case SYS_WAIT4:
        case SYS_NANOSLEEP:
        case SYS_SELECT:
        case SYS_FUTEX:
        case SYS_SOCKET_RECV:
        case SYS_SOCKET_SEND:
        case SYS_SOCKET_CONNECT:
        case SYS_SOCKET_CONNECT6:
        case SYS_SOCKET_ACCEPT:
        case SYS_SENDTO:
        case SYS_RECVFROM:
            return 1;
        default:
            return 0;
    }
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {

    /* Capture the user-mode return frame into the current TCB so subsequent
     * context switches in this syscall cannot corrupt it via the globals. */
    tcb_t *cur = sched_current();
    if (cur) {
        cur->saved_user_rip    = syscall_saved_rcx;
        cur->saved_user_rflags = syscall_saved_r11;
        cur->saved_user_rsp    = syscall_saved_rsp;
        cur->saved_user_rbx    = syscall_saved_rbx;
        cur->saved_user_rbp    = syscall_saved_rbp;
        cur->saved_user_r12    = syscall_saved_r12;
        cur->saved_user_r13    = syscall_saved_r13;
        cur->saved_user_r14    = syscall_saved_r14;
        cur->saved_user_r15    = syscall_saved_r15;
        if (num != SYS_SIGRETURN) {
            cur->syscall_restart_num = num;
            cur->syscall_restart_args[0] = a1;
            cur->syscall_restart_args[1] = a2;
            cur->syscall_restart_args[2] = a3;
            cur->syscall_restart_args[3] = a4;
            cur->syscall_restart_args[4] = a5;
            cur->syscall_restart_args[5] = a6;
        }
    }

    switch (num) {
    case SYS_WRITE: {
        /* a1 = fd, a2 = buffer, a3 = length. fd 1/2 go to console; fd >= 3
         * writes through the VFS (tmpfs/devfs/etc.). */
        const void *user_buf = (const void *)(uintptr_t)a2;
        if (a3 != 0 && !validate_user_range(user_buf, a3, 0)) {
            return (uint64_t)-EFAULT;
        }
        if (a1 == 1 || a1 == 2) {
            /* POSIX redirection: if dup2() wired a PIPE into fd 1/2, the
             * bytes must go to that pipe, not to the console (this is how
             * gterm captures a spawned program's stdout).  Every other case
             * — /dev/tty0 from init, /dev/null from vfs_ensure_std_fds() —
             * keeps the historical locked console path, so shell-visible
             * behavior is unchanged. */
            if (vfs_fd_is_pipe((int)a1)) {
                return (uint64_t)syscall_vfs_write((int)a1, user_buf, a3);
            }
            char tmp[SYSCALL_IO_CHUNK];
            uint64_t done = 0;
            while (done < a3) {
                uint64_t n = a3 - done;
                if (n > sizeof(tmp)) n = sizeof(tmp);
                if (copy_from_user(tmp, (const uint8_t *)user_buf + done, n) != 0) {
                    return (uint64_t)-EFAULT;
                }
                /* Write each chunk atomically under print_lock so kernel
                 * kprintf() cannot splice into a user-space line (fixes
                 * flaky console-marker integration tests on SMP). */
                kputs_locked(tmp, n);
                done += n;
            }
            return a3;
        }
        return (uint64_t)syscall_vfs_write((int)a1, user_buf, a3);
    }
    case SYS_READ: {
        /* a1 = fd, a2 = buffer, a3 = count. */
        int fd = (int)a1;
        void *user_buf = (void *)(uintptr_t)a2;
        if (a3 != 0 && !validate_user_range(user_buf, a3, 1)) {
            return (uint64_t)-EFAULT;
        }
        if (fd == 0) {
            /* stdin: line input from PS/2 keyboard and/or serial UART. */
            uint64_t count = a3;
            uint64_t got = 0;
            while (got < count) {
                int have = 0;
                unsigned char raw = 0;

                int kc = keyboard_getchar();
                if (kc >= 0) {
                    raw = (unsigned char)kc;
                    have = 1;
                } else if (uart_has_data()) {
                    raw = (unsigned char)uart_getchar();
                    have = 1;
                }

                if (!have) {
                    /* A pending unblocked signal interrupts the blocking read:
                     * return the partial line if any bytes were read, else
                     * -EINTR (POSIX read()).  The signal is delivered at the
                     * syscall-exit boundary. */
                    if (signal_interrupted()) {
                        return got ? got : (uint64_t)-EINTR;
                    }
                    /* SYSCALL entry masks IF, so a blocking stdin read must not
                     * spin forever on the shell's kernel stack with interrupts
                     * disabled.  That starves the PIT-driven scheduler and the
                     * GUI/USB-HID polling threads; on QEMU/Windows this made
                     * mouse motion appear only after serial/keyboard input
                     * "kicked" the guest.  Restore interrupts while waiting.
                     *
                     * Genuinely block for one PIT tick (via sleep_deadline,
                     * the same mechanism kernel_nanosleep() uses) instead of
                     * calling sched_yield() in a spin loop: sched_yield()
                     * only marks the CURRENT thread READY and reschedules,
                     * so with nothing else runnable the scheduler hands the
                     * CPU straight back to this very thread -- it never
                     * actually reaches the idle loop, so every /proc
                     * consumer (loadavg, a system monitor, ...) sees the CPU
                     * as 100% "busy" even while the shell is simply sitting
                     * at an empty prompt. Arming sleep_deadline and entering
                     * THREAD_BLOCKED makes schedule() run the idle thread
                     * (hlt) for that tick instead, which both accounts
                     * correctly in sched_get_idle_ticks() and saves real
                     * host CPU time under emulation. */
                    {
                        uint64_t rflags2;
                        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags2));
                        tcb_t *cur = sched_current();
                        if (cur) {
                            cur->sleep_deadline = timer_get_ticks() + 1;
                            cur->state = THREAD_BLOCKED;
                        }
                        schedule();
                        if (rflags2 & 0x200ULL) {
                            __asm__ volatile ("sti" ::: "memory");
                        }
                    }
                    continue;
                }

                /* ISIG: terminal signal characters (^C/^\/^Z) on the console
                 * tty generate signals and are not added to the input line.
                 * (Keeps the existing fd-0 stdin path; full /dev/tty0 line
                 * discipline is used by programs that open it directly.) */
                {
                    struct tty *con = tty_console();
                    if (con->termios.c_lflag & ISIG) {
                        int sig = 0;
                        if (raw == con->termios.c_cc[VINTR]) sig = SIGINT;
                        else if (raw == con->termios.c_cc[VQUIT]) sig = SIGQUIT;
                        else if (raw == con->termios.c_cc[VSUSP]) sig = SIGTSTP;
                        if (sig) {
                            /* Route to the console terminal's foreground process
                             * group (P6); falls back to the current task. */
                            tty_send_signal_fg(con, sig);
                            /* Echo ^X then interrupt the read with -EINTR (or a
                             * partial line if bytes were already typed). */
                            kputchar('^'); kputchar((char)(raw + 0x40));
                            return got ? got : (uint64_t)-EINTR;
                        }
                    }
                }

                if (raw == 0x00 || raw == 0xFF || raw > 0x7E) continue;

                char c = (char)raw;
                if (c == '\r') c = '\n';

                if (c == '\b' || raw == 0x7F) {
                    if (got > 0) {
                        got--;
                        kputchar('\b'); kputchar(' '); kputchar('\b');
                    }
                    continue;
                }

                if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) continue;

                if (copy_to_user((uint8_t *)user_buf + got, &c, 1) != 0) {
                    return (uint64_t)-EFAULT;
                }
                got++;
                kputchar(c);   /* echo */
                if (c == '\n') break;
            }
            return got;
        }
        return (uint64_t)syscall_vfs_read(fd, user_buf, a3);
    }
    case SYS_OPEN: {
        /* a1 = path, a2 = flags, a3 = mode.
         *
         * The result is returned WITHOUT vfs_errno(), and that is deliberate.
         *
         * vfs_errno() substitutes a fallback errno for a generic -1.  EPERM
         * is 1, so -EPERM IS -1: routing vfs_open() through vfs_errno() turns
         * every "operation not permitted" into ENOENT.  The comment that used
         * to sit here called vfs_errno() "an idempotent safety net"; it is
         * idempotent for every errno except the one whose value collides with
         * the sentinel, and that went unnoticed because nothing in vfs_open()
         * returned EPERM until the installation policy did.
         *
         * The substitution is unnecessary anyway: every failure path in
         * vfs_open() returns a specific negative errno.  There is no generic
         * -1 left for a fallback to rescue. */
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_open(path, (int)a2, (int)a3);
    }
    case SYS_FSYNC: {
        /* M9: fsync(fd).
         *
         * SYS_FSYNC 74 was defined but had NO case arm, so every fsync()
         * fell through to default and returned -ENOSYS.  That matters more
         * since A6: a file-backed MAP_SHARED page is only written back by
         * msync() or munmap(), so a program that wrote through a mapping
         * and called fsync() -- the ordinary thing to do -- got an error
         * and no writeback.
         *
         * Flush the page cache for this descriptor, then let the
         * filesystem push its own metadata if it can. */
        tcb_t *fc = sched_current();
        if (!fc) return (uint64_t)-EBADF;
        if (a1 >= VFS_MAX_FDS || fc->fd_table[a1] == NULL)
            return (uint64_t)-EBADF;
        struct ofd *fo = fc->fd_table[a1];
        page_cache_flush(fo);
        return 0;
    }
    case SYS_CLOSE:
        return (uint64_t)vfs_errno(vfs_close((int)a1), EBADF);
    case SYS_EXIT:
        thread_exit_with_code((int)a1);
        return 0;   /* unreachable */
    case SYS_GETPID: {
        tcb_t *cur = sched_current();
        return cur ? cur->id : 0;
    }
    case SYS_FORK:
        return do_fork();
    case SYS_EXECVE: {
        /* a1 = path, a2 = argv (char* const*), a3 = envp (char* const*). */
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno((int64_t)do_execve(path, a2, a3), ENOENT);
    }
    case SYS_WAIT4: {
        /* New ABI: a1 = pid (int64_t, -1 = any child), a2 = *exit_code (or NULL).
         * Old ABI: a1 = *exit_code, a2 = 0.
         * For backwards compatibility, treat a1 as a pid if it is small
         * (canonical PIDs fit in 32 bits) or negative.  If a1 looks like a
         * userspace pointer (>= 0x1000 and < USER_VADDR_TOP) and a2 == 0 we
         * fall back to the legacy meaning. */
        int64_t pid = (int64_t)a1;
        void *user_status = (void *)(uintptr_t)a2;
        int options = (int)a3;
        if (a2 == 0 && a1 >= 0x1000 && a1 < 0x0000800000000000ULL) {
            /* Legacy wait(status) form: a1 is the status pointer. */
            pid = -1;
            user_status = (void *)(uintptr_t)a1;
        }
        int status = 0;
        int64_t ret = do_waitpid(pid, user_status ? &status : 0, options);
        /* ret: pid (>0), 0 (WNOHANG, none ready), or negative errno. */
        if (ret > 0 && user_status) {
            if (copy_to_user(user_status, &status, sizeof(status)) != 0) {
                return (uint64_t)-EFAULT;
            }
        }
        return (uint64_t)ret;
    }
    case SYS_SPAWN: {
        /* a1 = path, a2 = argv (char* const*) or 0.
         *
         * a2 was ignored before SDK_PLAN phase S3, so a caller passing 0 --
         * every existing one -- behaves exactly as before.  argv is captured
         * inside process_spawn_argv() while the CALLER's address space is
         * still current; the new process gets a fresh one where those
         * pointers would mean nothing. */
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(process_spawn_argv(path, a2), ENOENT);
    }
    case SYS_LISTDIR: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        if (a2 && a3 > 0) {
            int max = (int)a3;
            if (max > VFS_MAX_DIRENTS) max = VFS_MAX_DIRENTS;
            if (max <= 0) return (uint64_t)-EINVAL;
            uint64_t bytes = (uint64_t)max * sizeof(struct vfs_dirent);
            if (!validate_user_range((void *)(uintptr_t)a2, bytes, 1)) {
                return (uint64_t)-EFAULT;
            }
            struct vfs_dirent *kents = kmalloc((size_t)bytes);
            if (!kents) return (uint64_t)-ENOMEM;
            int n = vfs_readdir(path, kents, max);
            if (n > 0) {
                uint64_t used = (uint64_t)n * sizeof(struct vfs_dirent);
                if (copy_to_user((void *)(uintptr_t)a2, kents, used) != 0) {
                    kfree(kents);
                    return (uint64_t)-EFAULT;
                }
            }
            kfree(kents);
            return (uint64_t)vfs_errno(n, ENOENT);
        } else {
            vfs_list(path);
            return 0;
        }
    }
    case SYS_DNS: {
        char host[SYSCALL_PATH_MAX];
        if (copy_string_from_user(host, (const char *)(uintptr_t)a1, sizeof(host)) != 0) {
            return (uint64_t)-EFAULT;
        }
        return net_dns_resolve(host);
    }
    case SYS_DNS_AAAA: {
        char host[SYSCALL_PATH_MAX];
        uint8_t addr[16];
        if (copy_string_from_user(host, (const char *)(uintptr_t)a1,
                                  sizeof(host)) != 0)
            return (uint64_t)-EFAULT;
        if (a2 == 0) return (uint64_t)-EFAULT;
        if (!validate_user_range((void *)(uintptr_t)a2, 16, 1))
            return (uint64_t)-EFAULT;
        if (dns_resolve_aaaa(host, addr) != 0) return (uint64_t)-1;
        if (copy_to_user((void *)(uintptr_t)a2, addr, 16) != 0)
            return (uint64_t)-EFAULT;
        return 0;
    }
    case SYS_SOCKET:
        return (uint64_t)socket_create((int)a1, (int)a2, (int)a3);
    case SYS_SOCKET_CONNECT:
        return (uint64_t)socket_connect((int)a1, (uint32_t)a2, (uint16_t)a3);
    case SYS_SOCKET_CONNECT6: {
        uint8_t addr[16];
        if (a2 == 0) return (uint64_t)-EFAULT;
        if (!validate_user_range((const void *)(uintptr_t)a2, 16, 0))
            return (uint64_t)-EFAULT;
        if (copy_from_user(addr, (const void *)(uintptr_t)a2, 16) != 0)
            return (uint64_t)-EFAULT;
        return (uint64_t)socket_connect6((int)a1, addr, (uint16_t)a3);
    }
    case SYS_SOCKET_SEND: {
        if (a3 == 0) return 0;
        const void *user_buf = (const void *)(uintptr_t)a2;
        if (!validate_user_range(user_buf, a3, 0)) return (uint64_t)-EFAULT;
        char tmp[SYSCALL_IO_CHUNK];
        uint64_t sent = 0;
        while (sent < a3) {
            uint64_t n = a3 - sent;
            if (n > sizeof(tmp)) n = sizeof(tmp);
            if (copy_from_user(tmp, (const uint8_t *)user_buf + sent, n) != 0) return (uint64_t)-EFAULT;
            int64_t r = socket_send((int)a1, tmp, (uint32_t)n);
            if (r < 0) return (uint64_t)r;   /* FIX_R7: keep the specific errno */
            sent += (uint64_t)r;
            if ((uint64_t)r < n) break;
        }
        return sent;
    }
    case SYS_SOCKET_RECV: {
        if (a3 == 0) return 0;
        void *user_buf = (void *)(uintptr_t)a2;
        if (!validate_user_range(user_buf, a3, 1)) return (uint64_t)-EFAULT;
        char tmp[SYSCALL_IO_CHUNK];
        uint32_t n = (a3 > sizeof(tmp)) ? (uint32_t)sizeof(tmp) : (uint32_t)a3;
        int64_t r = socket_recv((int)a1, tmp, n);
        if (r <= 0) return (uint64_t)r;
        if (copy_to_user(user_buf, tmp, (uint64_t)r) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)r;
    }

    case SYS_SENDTO: {
        if (a3 == 0) return 0;
        const void *user_buf = (const void *)(uintptr_t)a2;
        if (!validate_user_range(user_buf, a3, 0)) return (uint64_t)-EFAULT;
        if (a3 > SYSCALL_IO_CHUNK) return (uint64_t)-EINVAL;
        if (a5 == 0 || a6 < 16) return (uint64_t)-EINVAL;
        if (!validate_user_range((const void *)(uintptr_t)a5, 16, 0)) return (uint64_t)-EFAULT;

        char tmp[SYSCALL_IO_CHUNK];
        uint8_t kaddr[16];
        if (copy_from_user(tmp, user_buf, a3) != 0) return (uint64_t)-EFAULT;
        if (copy_from_user(kaddr, (const void *)(uintptr_t)a5, sizeof(kaddr)) != 0) return (uint64_t)-EFAULT;
        uint16_t family = (uint16_t)kaddr[0] | ((uint16_t)kaddr[1] << 8);
        if (family != AURA_AF_INET) return (uint64_t)-EINVAL;
        uint16_t port = ((uint16_t)kaddr[2] << 8) | (uint16_t)kaddr[3];
        uint32_t ip = ((uint32_t)kaddr[4] << 24) | ((uint32_t)kaddr[5] << 16) |
                      ((uint32_t)kaddr[6] << 8) | (uint32_t)kaddr[7];
        int64_t r = socket_sendto((int)a1, tmp, (uint32_t)a3, ip, port);
        return (uint64_t)r;
    }
    case SYS_RECVFROM: {
        if (a3 == 0) return 0;
        void *user_buf = (void *)(uintptr_t)a2;
        if (!validate_user_range(user_buf, a3, 1)) return (uint64_t)-EFAULT;
        if (a3 > SYSCALL_IO_CHUNK) a3 = SYSCALL_IO_CHUNK;

        char tmp[SYSCALL_IO_CHUNK];
        uint32_t src_ip = 0;
        uint16_t src_port = 0;
        int64_t r = socket_recvfrom((int)a1, tmp, (uint32_t)a3, &src_ip, &src_port);
        if (r <= 0) return (uint64_t)r;
        if (copy_to_user(user_buf, tmp, (uint64_t)r) != 0) return (uint64_t)-EFAULT;
        if (a5 != 0) {
            uint8_t kaddr[16];
            memset(kaddr, 0, sizeof(kaddr));
            kaddr[0] = (uint8_t)(AURA_AF_INET & 0xFF);
            kaddr[1] = (uint8_t)((AURA_AF_INET >> 8) & 0xFF);
            kaddr[2] = (uint8_t)((src_port >> 8) & 0xFF);
            kaddr[3] = (uint8_t)(src_port & 0xFF);
            kaddr[4] = (uint8_t)((src_ip >> 24) & 0xFF);
            kaddr[5] = (uint8_t)((src_ip >> 16) & 0xFF);
            kaddr[6] = (uint8_t)((src_ip >> 8) & 0xFF);
            kaddr[7] = (uint8_t)(src_ip & 0xFF);
            if (a6 != 0) {
                uint32_t user_len = 0;
                if (!validate_user_range((void *)(uintptr_t)a6, sizeof(uint32_t), 1)) return (uint64_t)-EFAULT;
                if (copy_from_user(&user_len, (const void *)(uintptr_t)a6, sizeof(uint32_t)) != 0) return (uint64_t)-EFAULT;
                if (user_len < sizeof(kaddr)) return (uint64_t)-EINVAL;
                if (copy_to_user((void *)(uintptr_t)a6, &(uint32_t){ sizeof(kaddr) }, sizeof(uint32_t)) != 0) return (uint64_t)-EFAULT;
            }
            if (!validate_user_range((void *)(uintptr_t)a5, sizeof(kaddr), 1)) return (uint64_t)-EFAULT;
            if (copy_to_user((void *)(uintptr_t)a5, kaddr, sizeof(kaddr)) != 0) return (uint64_t)-EFAULT;
        }
        return (uint64_t)r;
    }
    case SYS_SOCKET_CLOSE:
        return (uint64_t)socket_close((int)a1);
    case SYS_SOCKET_BIND:
        return (uint64_t)socket_bind((int)a1, (uint32_t)a2, (uint16_t)a3);
    case SYS_SOCKET_LISTEN:
        return (uint64_t)socket_listen((int)a1, (int)a2);
    case SYS_SOCKET_ACCEPT: {
        uint32_t peer_ip = 0;
        uint16_t peer_port = 0;
        int64_t r = socket_accept((int)a1, &peer_ip, &peer_port);
        if (r < 0) return (uint64_t)r;   /* FIX_R7: keep the specific errno */
        if (a2 != 0) {
            if (copy_to_user((void *)(uintptr_t)a2, &peer_ip, sizeof(uint32_t)) != 0) return (uint64_t)-EFAULT;
        }
        if (a3 != 0) {
            if (copy_to_user((void *)(uintptr_t)a3, &peer_port, sizeof(uint16_t)) != 0) return (uint64_t)-EFAULT;
        }
        return (uint64_t)r;
    }
    case SYS_NET_CONNECT:
        return (uint64_t)tcp_connect(a1, (uint16_t)a2);
    case SYS_NET_SEND: {
        if (a2 == 0) return 0;
        const void *user_buf = (const void *)(uintptr_t)a1;
        if (!validate_user_range(user_buf, a2, 0)) return (uint64_t)-EFAULT;
        char tmp[SYSCALL_IO_CHUNK];
        uint64_t sent = 0;
        while (sent < a2) {
            uint64_t n = a2 - sent;
            if (n > sizeof(tmp)) n = sizeof(tmp);
            if (copy_from_user(tmp, (const uint8_t *)user_buf + sent, n) != 0) {
                return (uint64_t)-EFAULT;
            }
            int64_t r = tcp_send(tmp, (uint32_t)n);
            if (r < 0) return (uint64_t)r;   /* FIX_R7: keep the specific errno */
            sent += (uint64_t)r;
            if ((uint64_t)r < n) break;
        }
        return sent;
    }
    case SYS_NET_RECV: {
        if (a2 == 0) return 0;
        void *user_buf = (void *)(uintptr_t)a1;
        if (!validate_user_range(user_buf, a2, 1)) return (uint64_t)-EFAULT;
        char tmp[SYSCALL_IO_CHUNK];
        uint32_t n = (a2 > sizeof(tmp)) ? (uint32_t)sizeof(tmp) : (uint32_t)a2;
        int64_t r = tcp_recv(tmp, n);
        if (r <= 0) return (uint64_t)r;
        if (copy_to_user(user_buf, tmp, (uint64_t)r) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    case SYS_NET_CLOSE:
        return (uint64_t)tcp_close();
    case SYS_NET_PING:
        return (uint64_t)net_ping(a1);

    /* X7: ping6.  a1 points to a 16-byte IPv6 address in user space; copy it
     * out (validated/bounded), then ask the kernel's ICMPv6 path to echo it. */
    case SYS_PING6: {
        ipv6_addr_t addr;
        if (!validate_user_range((const void *)(uintptr_t)a1, IPV6_ADDR_LEN, 1))
            return (uint64_t)-EFAULT;
        if (copy_from_user(addr.b, (const void *)(uintptr_t)a1, IPV6_ADDR_LEN) != 0)
            return (uint64_t)-EFAULT;
        return (uint64_t)net_ping6(&addr);
    }

    /* X3: DNS cache inspection/flush and resolver server control for the
     * dnscache/dnsset/dnsflush shell commands.  Buffers are validated and
     * bounded before any copy (D7/HARDENING discipline). */
    case SYS_DNSCTL: {
        switch (a1) {
        case DNSCTL_LIST: {
            if (!validate_user_range((const void *)(uintptr_t)a2, a3, 1)) return (uint64_t)-EFAULT;
            dnsctl_entry_t kbuf[DNS_CACHE_MAX];
            int want = (int)(a3 / sizeof(dnsctl_entry_t));
            int n = dns_cache_snapshot(kbuf, want);
            if (copy_to_user((uint8_t *)(uintptr_t)a2, (const uint8_t *)kbuf,
                             (uint64_t)n * sizeof(dnsctl_entry_t)) != 0)
                return (uint64_t)-EFAULT;
            return (uint64_t)n;
        }
        case DNSCTL_FLUSH:
            dns_cache_flush();
            return 0;
        case DNSCTL_SET_SERVERS: {
            if (!validate_user_range((const void *)(uintptr_t)a2, a3, 0)) return (uint64_t)-EFAULT;
            int n = (int)(a3 / sizeof(uint32_t));
            if (n <= 0 || n > DNS_SERVERS_MAX) return (uint64_t)-EINVAL;
            uint32_t ips[DNS_SERVERS_MAX];
            if (copy_from_user((uint8_t *)ips, (const uint8_t *)(uintptr_t)a2,
                               (uint64_t)n * sizeof(uint32_t)) != 0)
                return (uint64_t)-EFAULT;
            dns_set_servers(ips, n);
            return (uint64_t)n;
        }
        case DNSCTL_FORCE_TC:
            dns_force_tc_once();       /* R9: one-shot TC (see dns.h) */
            return 0;
        case DNSCTL_GET_SERVERS: {
            if (!validate_user_range((const void *)(uintptr_t)a2, a3, 1)) return (uint64_t)-EFAULT;
            uint32_t ips[DNS_SERVERS_MAX];
            int n = dns_get_servers(ips, DNS_SERVERS_MAX);
            if (copy_to_user((uint8_t *)(uintptr_t)a2, (const uint8_t *)ips,
                             (uint64_t)n * sizeof(uint32_t)) != 0)
                return (uint64_t)-EFAULT;
            return (uint64_t)n;
        }
        default:
            return (uint64_t)-EINVAL;
        }
    }

    /* Filesystem extensions. */
    case SYS_MKDIR: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(vfs_mkdir(path, (uint32_t)a2), EACCES);
    }
    case SYS_RMDIR: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(vfs_rmdir(path), ENOENT);
    }
    case SYS_UNLINK: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(vfs_unlink(path), ENOENT);
    }
    case SYS_RENAME: {
        char from[SYSCALL_PATH_MAX], to[SYSCALL_PATH_MAX];
        if (copy_user_path(from, a1) != 0) return (uint64_t)-EFAULT;
        if (copy_user_path(to, a2) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(vfs_rename(from, to), ENOENT);
    }
    case SYS_TRUNCATE: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(vfs_truncate(path, a2), ENOENT);
    }
    case SYS_STAT: {
        char path[SYSCALL_PATH_MAX];
        struct vfs_stat st;
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        if (!validate_user_range((void *)(uintptr_t)a2, sizeof(st), 1)) {
            return (uint64_t)-EFAULT;
        }
        int r = vfs_stat(path, &st);
        if (r != 0) return (uint64_t)vfs_errno(r, ENOENT);
        st.mode = stat_posix_mode(&st);   /* Q12 */
        if (copy_to_user((void *)(uintptr_t)a2, &st, sizeof(st)) != 0) {
            return (uint64_t)-EFAULT;
        }
        return 0;
    }
    case SYS_MKFIFO: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(vfs_mkfifo(path, (uint32_t)a2), EACCES);
    }

    /* GUI syscalls.  gui_syscalls.c performs op-specific user copies. */
    case SYS_GUI_CALL:
        return syscall_gui_call(a1, a2, a3, a4, a5);
    case SYS_GUI_EVENT:
        return syscall_gui_event(a1, a2, a3);
    case SYS_GUI_THEME:
        return syscall_gui_theme(a1, a2, a3, a4, a5);

    /* GPU 3D submission.  gpu_syscalls.c validates every user buffer and
     * translates per-process resource handles to device ids (phase K1). */
    case SYS_GPU_CALL:
        return syscall_gpu_call(a1, a2, a3, a4, a5);

    /* dup / dup2 / pipe / fcntl. */
    case SYS_DUP:
        return (uint64_t)vfs_errno(vfs_dup((int)a1), EBADF);
    case SYS_DUP2:
        return (uint64_t)vfs_errno(vfs_dup2((int)a1, (int)a2), EBADF);
    case SYS_PIPE: {
        int fds[2];
        if (!validate_user_range((void *)(uintptr_t)a1, sizeof(fds), 1)) {
            return (uint64_t)-EFAULT;
        }
        int r = vfs_pipe(fds);
        if (r != 0) return (uint64_t)-EMFILE;
        if (copy_to_user((void *)(uintptr_t)a1, fds, sizeof(fds)) != 0) {
            /* Roll back partially: close both ends. */
            vfs_close(fds[0]);
            vfs_close(fds[1]);
            return (uint64_t)-EFAULT;
        }
        return 0;
    }
    case SYS_FCNTL:
        /* a1 = fd, a2 = cmd, a3 = arg.  vfs_fcntl handles the full subset
         * (F_GETFD/SETFD/GETFL/SETFL/DUPFD/DUPFD_CLOEXEC) and returns errno. */
        return (uint64_t)vfs_fcntl((int)a1, (int)a2, (int)a3);
    case SYS_PIPE2: {
        /* a1 = int fds[2], a2 = flags. */
        int fds[2];
        int flags = (int)a2;
        if (!validate_user_range((void *)(uintptr_t)a1, sizeof(fds), 1)) {
            return (uint64_t)-EFAULT;
        }
        int r = vfs_pipe2(fds, flags);
        if (r != 0) return (uint64_t)r;   /* already a negative errno */
        if (copy_to_user((void *)(uintptr_t)a1, fds, sizeof(fds)) != 0) {
            vfs_close(fds[0]);
            vfs_close(fds[1]);
            return (uint64_t)-EFAULT;
        }
        return 0;
    }
    case SYS_GETUID:
        return cur ? (uint64_t)cur->uid : 0;
    case SYS_GETEUID:
        return cur ? (uint64_t)cur->euid : 0;
    case SYS_GETGID:
        return cur ? (uint64_t)cur->gid : 0;
    case SYS_GETEGID:
        return cur ? (uint64_t)cur->egid : 0;
    case SYS_SETUID: {
        if (!cur) return (uint64_t)-EPERM;
        uint32_t uid = (uint32_t)a1;
        if (cur->euid == 0) {
            cur->uid = uid; cur->euid = uid; cur->suid = uid;
            return 0;
        } else if (uid == cur->uid || uid == cur->suid) {
            cur->euid = uid;
            return 0;
        }
        return (uint64_t)-EPERM;
    }
    case SYS_SETGID: {
        if (!cur) return (uint64_t)-EPERM;
        uint32_t gid = (uint32_t)a1;
        if (cur->euid == 0) {
            cur->gid = gid; cur->egid = gid; cur->sgid = gid;
            return 0;
        } else if (gid == cur->gid || gid == cur->sgid) {
            cur->egid = gid;
            return 0;
        }
        return (uint64_t)-EPERM;
    }
    case SYS_SETREUID: {
        if (!cur) return (uint64_t)-EPERM;
        uint32_t ruid = (uint32_t)a1;
        uint32_t euid = (uint32_t)a2;
        uint32_t new_ruid = (ruid != (uint32_t)-1) ? ruid : cur->uid;
        uint32_t new_euid = (euid != (uint32_t)-1) ? euid : cur->euid;
        if (cur->euid != 0) {
            if (ruid != (uint32_t)-1 && ruid != cur->uid && ruid != cur->euid) return (uint64_t)-EPERM;
            if (euid != (uint32_t)-1 && euid != cur->uid && euid != cur->euid && euid != cur->suid) return (uint64_t)-EPERM;
        }
        if (ruid != (uint32_t)-1 || (euid != (uint32_t)-1 && euid != cur->uid)) {
            cur->suid = new_euid;
        }
        cur->uid = new_ruid;
        cur->euid = new_euid;
        return 0;
    }
    case SYS_SETREGID: {
        if (!cur) return (uint64_t)-EPERM;
        uint32_t rgid = (uint32_t)a1;
        uint32_t egid = (uint32_t)a2;
        uint32_t new_rgid = (rgid != (uint32_t)-1) ? rgid : cur->gid;
        uint32_t new_egid = (egid != (uint32_t)-1) ? egid : cur->egid;
        if (cur->euid != 0) {
            if (rgid != (uint32_t)-1 && rgid != cur->gid && rgid != cur->egid) return (uint64_t)-EPERM;
            if (egid != (uint32_t)-1 && egid != cur->gid && egid != cur->egid && egid != cur->sgid) return (uint64_t)-EPERM;
        }
        if (rgid != (uint32_t)-1 || (egid != (uint32_t)-1 && egid != cur->gid)) {
            cur->sgid = new_egid;
        }
        cur->gid = new_rgid;
        cur->egid = new_egid;
        return 0;
    }
    case SYS_GETGROUPS: {
        if (!cur) return 0;
        int size = (int)a1;
        void *list = (void *)(uintptr_t)a2;
        if (size == 0) return (uint64_t)cur->ngroups;
        if (size < cur->ngroups) return (uint64_t)-EINVAL;
        if (!validate_user_range(list, (uint64_t)cur->ngroups * sizeof(uint32_t), 1)) return (uint64_t)-EFAULT;
        if (copy_to_user(list, cur->supplementary_gids, (uint64_t)cur->ngroups * sizeof(uint32_t)) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)cur->ngroups;
    }
    case SYS_SETGROUPS: {
        if (!cur || cur->euid != 0) return (uint64_t)-EPERM;
        int size = (int)a1;
        const void *list = (const void *)(uintptr_t)a2;
        if (size < 0 || size > 32) return (uint64_t)-EINVAL;
        if (size > 0) {
            if (!validate_user_range(list, (uint64_t)size * sizeof(uint32_t), 0)) return (uint64_t)-EFAULT;
            if (copy_from_user(cur->supplementary_gids, (const uint8_t *)list, (uint64_t)size * sizeof(uint32_t)) != 0) return (uint64_t)-EFAULT;
        }
        cur->ngroups = size;
        return 0;
    }
    case SYS_CHMOD: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_chmod(path, (uint32_t)a2);
    }
    case SYS_FCHMOD:
        return (uint64_t)vfs_fchmod((int)a1, (uint32_t)a2);
    case SYS_CHOWN: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_chown(path, (uint32_t)a2, (uint32_t)a3);
    }
    case SYS_FCHOWN:
        return (uint64_t)vfs_fchown((int)a1, (uint32_t)a2, (uint32_t)a3);
    case SYS_UMASK: {
        if (!cur) return 0022;
        uint16_t old = cur->umask;
        cur->umask = (uint16_t)(a1 & 0777u);
        return (uint64_t)old;
    }
    case SYS_ACCESS: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_access(path, (int)a2);
    }
    case SYS_LSEEK:
        /* a1 = fd, a2 = offset (int64_t), a3 = whence. */
        return (uint64_t)vfs_lseek((int)a1, (int64_t)a2, (int)a3);
    case SYS_IOCTL: {
        /* a1 = fd, a2 = cmd, a3 = arg pointer.  The arg size depends on cmd. */
        int fd = (int)a1;
        unsigned long cmd = (unsigned long)a2;
        void *user_arg = (void *)(uintptr_t)a3;
        uint64_t sz;
        switch (cmd) {
        case TCGETS: case TCSETS: case TCSETSW: case TCSETSF:
            sz = sizeof(struct termios); break;
        case TIOCGWINSZ: case TIOCSWINSZ:
            sz = sizeof(struct winsize); break;
        case TIOCGPGRP: case TIOCSPGRP:
            sz = sizeof(int); break;
        default:
            return (uint64_t)-EINVAL;   /* unsupported ioctl */
        }
        if (sz > 256) return (uint64_t)-EINVAL;
        char kbuf[256];
        /* Copy the user argument in (set ops need the value; get ops are
         * harmless to copy and then overwrite). */
        if (!validate_user_range(user_arg, sz, 1)) return (uint64_t)-EFAULT;
        if (copy_from_user(kbuf, user_arg, sz) != 0) return (uint64_t)-EFAULT;
        int r = vfs_ioctl(fd, cmd, kbuf);
        if (r < 0) return (uint64_t)r;
        if (copy_to_user(user_arg, kbuf, sz) != 0) return (uint64_t)-EFAULT;
        return 0;
    }

    /* ---- P4: signals ---- */
    case SYS_SIGACTION:
        /* a1 = signo, a2 = const struct sigaction *act, a3 = struct sigaction *old */
        return (uint64_t)do_sigaction((int)a1,
                                      (const struct sigaction *)(uintptr_t)a2,
                                      (struct sigaction *)(uintptr_t)a3);
    case SYS_SIGPROCMASK:
        /* a1 = how, a2 = const sigset_t *set, a3 = sigset_t *old */
        return (uint64_t)do_sigprocmask((int)a1,
                                        (const sigset_t *)(uintptr_t)a2,
                                        (sigset_t *)(uintptr_t)a3);
    case SYS_SIGPENDING:
        return (uint64_t)do_sigpending((sigset_t *)(uintptr_t)a1);
    case SYS_ALARM:
        return (uint64_t)do_alarm((unsigned)a1);
    case SYS_PAUSE:
        return (uint64_t)do_pause();
    case SYS_SIGSUSPEND:
        return (uint64_t)do_sigsuspend((const sigset_t *)(uintptr_t)a1);

    /* ---- P6: process groups / sessions ---- */
    case SYS_SETSID:
        return (uint64_t)do_setsid();
    case SYS_SETPGID:
        return (uint64_t)do_setpgid((int64_t)a1, (int64_t)a2);
    case SYS_GETPGID:
        return (uint64_t)do_getpgid((int64_t)a1);
    case SYS_GETSID:
        return (uint64_t)do_getsid((int64_t)a1);
    case SYS_KILL:
        /* a1 = pid, a2 = signo */
        return (uint64_t)signal_kill((int64_t)a1, (int)a2);
    case SYS_SIGRETURN: {
        /* Restore the interrupted context from the user signal frame and return
         * to it via IRETQ (NOT sysret).  Synthesise a frame seeded from the
         * saved syscall-return state; do_sigreturn overwrites it from the
         * user-supplied signal_frame at the current user RSP. */
        struct registers r;
        memset(&r, 0, sizeof(r));
        r.rsp    = syscall_saved_rsp;    /* points at the signal_frame */
        r.cs     = SYSCALL_USER_CS;
        r.ss     = SYSCALL_USER_SS;
        do_sigreturn(&r);                /* fills r from the saved frame */
        syscall_iret_to_user(&r);        /* noreturn */
        return 0;                        /* unreachable */
    }
    case SYS_PREAD64:
        /* a1 = fd, a2 = buf, a3 = count, a4 = offset. */
        return (uint64_t)syscall_vfs_pread((int)a1, (void *)(uintptr_t)a2,
                                           a3, (int64_t)a4);
    case SYS_PWRITE64:
        return (uint64_t)syscall_vfs_pwrite((int)a1, (const void *)(uintptr_t)a2,
                                            a3, (int64_t)a4);
    case SYS_READV:
    case SYS_WRITEV: {
        /* a1 = fd, a2 = const struct iovec *iov, a3 = iovcnt.  The userspace
         * iovec is { void *iov_base; size_t iov_len; } == 16 bytes. */
        int fd = (int)a1;
        int iovcnt = (int)a3;
        if (iovcnt <= 0 || iovcnt > SYSCALL_IOV_MAX) return (uint64_t)-EINVAL;
        struct user_iovec { uint64_t base; uint64_t len; } ;
        uint64_t bytes = (uint64_t)iovcnt * sizeof(struct user_iovec);
        if (!validate_user_range((void *)(uintptr_t)a2, bytes, 0)) {
            return (uint64_t)-EFAULT;
        }
        struct user_iovec *kiov = kmalloc((size_t)bytes);
        if (!kiov) return (uint64_t)-ENOMEM;
        if (copy_from_user(kiov, (const void *)(uintptr_t)a2, bytes) != 0) {
            kfree(kiov);
            return (uint64_t)-EFAULT;
        }
        /* Sum lengths with overflow check before any transfer (POSIX EINVAL). */
        uint64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            if (total + kiov[i].len < total ||
                total + kiov[i].len > 0x7FFFFFFFFFFFFFFFULL) {
                kfree(kiov);
                return (uint64_t)-EINVAL;
            }
            total += kiov[i].len;
        }
        int64_t done = 0;
        for (int i = 0; i < iovcnt; i++) {
            if (kiov[i].len == 0) continue;
            void *base = (void *)(uintptr_t)kiov[i].base;
            int64_t n = (num == SYS_READV)
                ? syscall_vfs_read(fd, base, kiov[i].len)
                : syscall_vfs_write(fd, (const void *)base, kiov[i].len);
            if (n < 0) { done = (done > 0) ? done : n; break; }
            done += n;
            if ((uint64_t)n < kiov[i].len) break;   /* short transfer: stop */
        }
        kfree(kiov);
        return (uint64_t)done;
    }
    case SYS_MMAP:
        return syscall_mmap(a1, a2, a3, a4, a5, a6);
    case SYS_MSYNC: {
        /* A6: msync(addr, len, flags).  Writes a shared file mapping back
         * through the page cache.  MS_INVALIDATE is accepted and ignored;
         * MS_ASYNC and MS_SYNC behave identically because the flush is
         * synchronous either way. */
        uint64_t ms_addr = a1, ms_len = a2;
        uint32_t ms_flags = (uint32_t)a3;
        if (ms_addr & (PAGE_SIZE_BYTES - 1ULL)) return (uint64_t)-EINVAL;
        if (ms_flags & ~(1u | 2u | 4u)) return (uint64_t)-EINVAL;
        if ((ms_flags & 1u) && (ms_flags & 4u)) return (uint64_t)-EINVAL;
        if (ms_len == 0) return 0;
        ms_len = align_up_u64(ms_len, PAGE_SIZE_BYTES);
        if (!user_mmap_range_ok(ms_addr, ms_len)) return (uint64_t)-ENOMEM;

        tcb_t *mc = sched_current();
        if (!mc) return (uint64_t)-EINVAL;

        uint64_t mf = spinlock_acquire_irqsave(&mc->vma_lock);
        vma_t *mv = vma_find(mc->vma_list, ms_addr);
        struct ofd *mo = NULL;
        uint64_t moff = 0;
        if (mv && (mv->flags & VMA_SHARED) && (mv->flags & VMA_FILE) &&
            !(mv->flags & VMA_SHMEM) && mv->file) {
            mo = mv->file;
            moff = mv->file_off + (ms_addr - mv->va_start);
            vfs_ofd_get(mo);
        }
        int unmapped = (mv == NULL);
        spinlock_release_irqrestore(&mc->vma_lock, mf);

        if (unmapped) return (uint64_t)-ENOMEM;
        if (!mo) return 0;      /* private or anonymous: nothing to write */

        int mr = page_cache_flush_range(mo, moff, moff + ms_len);
        vfs_ofd_put(mo);
        return mr == 0 ? 0 : (uint64_t)-EIO;
    }
    case SYS_MUNMAP:
        return syscall_munmap(a1, a2);
    case SYS_MPROTECT:
        return syscall_mprotect(a1, a2, a3);

    /* M4: madvise — advisory hints about memory usage patterns.
     * MADV_NORMAL (0), MADV_RANDOM (1), MADV_SEQUENTIAL (2) are recorded
     * in the VMA but do not change fault behaviour (no readahead yet).
     * MADV_WILLNEED (3) faults pages in eagerly.
     * MADV_DONTNEED (4) discards anonymous pages (frees frames).
     * Unknown advice returns EINVAL (POSIX). */
    case SYS_MADVISE: {
        uint64_t madv_addr = a1, madv_len = a2, advice = a3;
        if (madv_addr & (PAGE_SIZE_BYTES - 1)) return (uint64_t)-EINVAL;
        if (advice > 4) return (uint64_t)-EINVAL;
        madv_len = align_up_u64(madv_len, PAGE_SIZE_BYTES);
        if (madv_len == 0) return 0;
        if (!user_mmap_range_ok(madv_addr, madv_len)) return (uint64_t)-EINVAL;
        if (advice == 3 /* MADV_WILLNEED */) {
            /* Eagerly fault in every page in the range. */
            uint64_t hhdm = boot_get_hhdm_offset();
            for (uint64_t off = 0; off < madv_len; off += PAGE_SIZE_BYTES) {
                if (paging_get_phys(madv_addr + off) != 0) continue;
                uint64_t phys = pmm_alloc_frame();
                if (!phys) break;   /* best effort */
                memset((void *)(uintptr_t)(hhdm + phys), 0, PAGE_SIZE_BYTES);
                paging_map(madv_addr + off, phys,
                           PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
            }
        } else if (advice == 4 /* MADV_DONTNEED */) {
            /* Discard anonymous pages: unmap + free frames. */
            for (uint64_t off = 0; off < madv_len; off += PAGE_SIZE_BYTES) {
                uint64_t phys = paging_get_phys(madv_addr + off);
                if (phys) {
                    paging_unmap(madv_addr + off);
                    pmm_free_frame(phys);
                }
            }
        }
        /* MADV_NORMAL/RANDOM/SEQUENTIAL: advisory only, no action. */
        return 0;
    }

    /* M4: mincore — report which pages are resident in memory.
     * Each byte in the output vector covers one page: bit 0 set = resident.
     * The caller provides a buffer of ceil(len / PAGE_SIZE) bytes. */
    case SYS_MINCORE: {
        uint64_t mc_addr = a1, mc_len = a2;
        void *user_vec = (void *)(uintptr_t)a3;
        if (mc_addr & (PAGE_SIZE_BYTES - 1)) return (uint64_t)-EINVAL;
        mc_len = align_up_u64(mc_len, PAGE_SIZE_BYTES);
        uint64_t npages = mc_len / PAGE_SIZE_BYTES;
        if (npages == 0) return 0;
        if (!validate_user_range(user_vec, npages, 1)) return (uint64_t)-EFAULT;
        if (!user_mmap_range_ok(mc_addr, mc_len)) return (uint64_t)-ENOMEM;
        /* Build the vector in a kernel buffer then copy out. */
        uint8_t kvec[512];   /* covers up to 2 MiB per call */
        if (npages > sizeof(kvec)) npages = sizeof(kvec);
        for (uint64_t i = 0; i < npages; i++) {
            kvec[i] = (paging_get_phys(mc_addr + i * PAGE_SIZE_BYTES) != 0) ? 1 : 0;
        }
        if (copy_to_user(user_vec, kvec, npages) != 0) return (uint64_t)-EFAULT;
        return 0;
    }

    /* M4: mlock/munlock — stub.  The VMA_LOCKED flag exists for future use;
     * today no eviction mechanism exists, so locking is a no-op that succeeds.
     * Returns 0 (success) rather than ENOSYS so programs that call mlock
     * for correctness do not fail. */
    case SYS_MLOCK:
    case SYS_MUNLOCK:
        return 0;
    case SYS_BRK: {
        tcb_t *cur = sched_current();
        if (!cur) return (uint64_t)-ENOMEM;

        if (a1 == 0) {
            return cur->brk; /* Query current break */
        }

        uint64_t req_brk = a1;
        if (req_brk < cur->brk) {
            /* Shrinking is intentionally unsupported for now. */
            return cur->brk;
        }
        if (req_brk >= USER_BRK_MAX) {
            return cur->brk;
        }

        uint64_t new_brk = (req_brk + 4095ULL) & ~4095ULL;
        if (new_brk < cur->brk || new_brk >= USER_BRK_MAX) {
            return cur->brk;
        }

        uint64_t pages_to_alloc = (new_brk - cur->brk) / 4096ULL;
        uint64_t hhdm = boot_get_hhdm_offset();
        for (uint64_t i = 0; i < pages_to_alloc; i++) {
            uint64_t virt = cur->brk + i * 4096ULL;
            if (paging_get_phys(virt) == 0) {
                uint64_t phys = pmm_alloc_frame();
                if (!phys) {
                    return cur->brk;
                }
                memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);
                paging_map(virt, phys,
                           PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE |
                           PAGE_FLAG_USER | PAGE_FLAG_NO_EXEC);
            }
        }

        cur->brk = new_brk;
        return cur->brk;
    }

    /* ---- P8: Clocks, Timers & sleep ---- */
    case SYS_CLOCK_GETTIME: {
        int clockid = (int)a1;
        struct kernel_timespec *user_ts = (struct kernel_timespec *)(uintptr_t)a2;
        if (!validate_user_range(user_ts, sizeof(struct kernel_timespec), 1))
            return (uint64_t)-EFAULT;
        struct kernel_timespec kts;
        extern void kernel_clock_gettime(int, struct kernel_timespec *);
        kernel_clock_gettime(clockid, &kts);
        if (copy_to_user(user_ts, &kts, sizeof(kts)) != 0)
            return (uint64_t)-EFAULT;
        return 0;
    }
    case SYS_CLOCK_GETRES: {
        int clockid = (int)a1;
        struct kernel_timespec *user_ts = (struct kernel_timespec *)(uintptr_t)a2;
        if (!validate_user_range(user_ts, sizeof(struct kernel_timespec), 1))
            return (uint64_t)-EFAULT;
        struct kernel_timespec kres;
        extern void kernel_clock_getres(int, struct kernel_timespec *);
        kernel_clock_getres(clockid, &kres);
        if (copy_to_user(user_ts, &kres, sizeof(kres)) != 0)
            return (uint64_t)-EFAULT;
        return 0;
    }
    case SYS_NANOSLEEP: {
        const struct kernel_timespec *ureq = (const struct kernel_timespec *)(uintptr_t)a1;
        struct kernel_timespec *urem = (struct kernel_timespec *)(uintptr_t)a2;
        struct kernel_timespec kreq, krem = {0};
        if (!validate_user_range((void*)ureq, sizeof(kreq), 0)) return (uint64_t)-EFAULT;
        if (copy_from_user(&kreq, ureq, sizeof(kreq)) != 0) return (uint64_t)-EFAULT;
        extern int kernel_nanosleep(const struct kernel_timespec *, struct kernel_timespec *);
        int r = kernel_nanosleep(&kreq, urem ? &krem : NULL);
        if (r == 0 && urem) {
            if (!validate_user_range(urem, sizeof(krem), 1)) return (uint64_t)-EFAULT;
            if (copy_to_user(urem, &krem, sizeof(krem)) != 0) return (uint64_t)-EFAULT;
        }
        return (uint64_t)r;
    }
    case SYS_GETTIMEOFDAY: {
        struct kernel_timeval *utv = (struct kernel_timeval *)(uintptr_t)a1;
        if (!validate_user_range(utv, sizeof(struct kernel_timeval), 1))
            return (uint64_t)-EFAULT;
        struct kernel_timeval ktv;
        extern int kernel_gettimeofday(struct kernel_timeval *, void *);
        if (kernel_gettimeofday(&ktv, NULL) != 0) return (uint64_t)-EFAULT;
        if (copy_to_user(utv, &ktv, sizeof(ktv)) != 0) return (uint64_t)-EFAULT;
        return 0;
    }
    case SYS_TIME: {
        long *ut = (long *)(uintptr_t)a1;
        extern time_t kernel_time(time_t *);
        time_t now = kernel_time(NULL);
        if (ut) {
            if (!validate_user_range(ut, sizeof(time_t), 1)) return (uint64_t)-EFAULT;
            if (copy_to_user(ut, &now, sizeof(now)) != 0) return (uint64_t)-EFAULT;
        }
        return (uint64_t)now;
    }
    case SYS_GETITIMER: {
        int which = (int)a1;
        struct itimer_state *uval = (struct itimer_state *)(uintptr_t)a2;
        if (!validate_user_range(uval, sizeof(struct itimer_state), 1))
            return (uint64_t)-EFAULT;
        struct itimer_state kval;
        extern int kernel_getitimer(int, struct itimer_state *);
        if (kernel_getitimer(which, &kval) != 0) return (uint64_t)-EINVAL;
        if (copy_to_user(uval, &kval, sizeof(kval)) != 0) return (uint64_t)-EFAULT;
        return 0;
    }
    case SYS_SETITIMER: {
        int which = (int)a1;
        const struct itimer_state *unew = (const struct itimer_state *)(uintptr_t)a2;
        struct itimer_state *uold = (struct itimer_state *)(uintptr_t)a3;
        struct itimer_state knew, kold;
        if (!validate_user_range((void*)unew, sizeof(knew), 0)) return (uint64_t)-EFAULT;
        if (copy_from_user(&knew, unew, sizeof(knew)) != 0) return (uint64_t)-EFAULT;
        extern int kernel_setitimer(int, const struct itimer_state *, struct itimer_state *);
        int r = kernel_setitimer(which, &knew, uold ? &kold : NULL);
        if (r == 0 && uold) {
            if (!validate_user_range(uold, sizeof(kold), 1)) return (uint64_t)-EFAULT;
            if (copy_to_user(uold, &kold, sizeof(kold)) != 0) return (uint64_t)-EFAULT;
        }
        return (uint64_t)r;
    }

    /* ---- P10: select / stat / symlink / cwd ---- */
    case SYS_SELECT: {
        extern int do_select(int, fd_set*, fd_set*, fd_set*, struct kernel_timeval*);
        struct kernel_timeval ktv;
        struct kernel_timeval *ktvp = NULL;
        if (a5) {
            if (copy_from_user(&ktv, (const void *)(uintptr_t)a5, sizeof(ktv)) != 0) {
                return (uint64_t)-EFAULT;
            }
            if (ktv.tv_usec < 0 || ktv.tv_usec >= 1000000) return (uint64_t)-EINVAL;
            if (ktv.tv_sec < 0) return (uint64_t)-EINVAL;
            ktvp = &ktv;
        }
        return (uint64_t)do_select((int)a1, (fd_set*)(uintptr_t)a2,
                                   (fd_set*)(uintptr_t)a3, (fd_set*)(uintptr_t)a4,
                                   ktvp);
    }
    /* Q16: pselect6 — select with a relative timespec and an
     * atomically-installed signal mask (the classic pselect race: the mask
     * is applied for the duration of the block only, so a signal cannot be
     * lost between the check and the wait). */
    case SYS_PSELECT6: {
        extern int do_select(int, fd_set*, fd_set*, fd_set*, struct kernel_timeval*);
        struct kernel_timespec kts;
        struct kernel_timeval ktv, *ktv_p = NULL;
        if (a5) {
            if (copy_from_user(&kts, (const void *)(uintptr_t)a5, sizeof(kts)) != 0)
                return (uint64_t)-EFAULT;
            if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
                return (uint64_t)-EINVAL;
            ktv.tv_sec  = kts.tv_sec;
            ktv.tv_usec = kts.tv_nsec / 1000;
            ktv_p = &ktv;
        }
        tcb_t *cur = sched_current();
        uint32_t old_mask = cur ? cur->sig_mask : 0;
        if (a6) {
            /* a6 = user pointer to { const sigset_t *ss; size_t ss_len; } */
            struct { uint64_t ss; uint64_t ss_len; } um;
            if (copy_from_user(&um, (const void *)(uintptr_t)a6, sizeof(um)) != 0)
                return (uint64_t)-EFAULT;
            if (um.ss_len != sizeof(sigset_t)) return (uint64_t)-EINVAL;
            sigset_t nm;
            if (copy_from_user(&nm, (const void *)(uintptr_t)um.ss, sizeof(nm)) != 0)
                return (uint64_t)-EFAULT;
            if (cur) cur->sig_mask = nm;
        }
        int r = do_select((int)a1, (fd_set*)(uintptr_t)a2,
                          (fd_set*)(uintptr_t)a3, (fd_set*)(uintptr_t)a4, ktv_p);
        if (cur) cur->sig_mask = old_mask;
        return (uint64_t)r;
    }
    /* Q16: ppoll — pollfds + relative timespec + atomic signal mask. */
    case SYS_PPOLL: {
        extern int do_ppoll(struct kernel_pollfd *, uint64_t,
                            struct kernel_timespec *, const sigset_t *);
        if (a5 != 0 && a5 != sizeof(sigset_t)) return (uint64_t)-EINVAL;
        struct kernel_timespec kts;
        struct kernel_timespec *kts_p = NULL;
        if (a3) {
            if (copy_from_user(&kts, (const void *)(uintptr_t)a3, sizeof(kts)) != 0)
                return (uint64_t)-EFAULT;
            if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
                return (uint64_t)-EINVAL;
            kts_p = &kts;
        }
        sigset_t nm;
        const sigset_t *sm = NULL;
        if (a4) {
            if (copy_from_user(&nm, (const void *)(uintptr_t)a4, sizeof(nm)) != 0)
                return (uint64_t)-EFAULT;
            sm = &nm;
        }
        return (uint64_t)do_ppoll((struct kernel_pollfd *)(uintptr_t)a1, a2,
                                  kts_p, sm);
    }
    case SYS_FSTAT: {
        struct vfs_stat st;
        if (!validate_user_range((void *)(uintptr_t)a2, sizeof(st), 1)) return (uint64_t)-EFAULT;
        int r = vfs_fstat((int)a1, &st);
        if (r != 0) return (uint64_t)r;
        st.mode = stat_posix_mode(&st);   /* Q12 */
        if (copy_to_user((void *)(uintptr_t)a2, &st, sizeof(st)) != 0) return (uint64_t)-EFAULT;
        return 0;
    }
    case SYS_LSTAT: {
        char path[SYSCALL_PATH_MAX];
        struct vfs_stat st;
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        if (!validate_user_range((void *)(uintptr_t)a2, sizeof(st), 1)) return (uint64_t)-EFAULT;
        int r = vfs_lstat(path, &st);
        if (r != 0) return (uint64_t)vfs_errno(r, ENOENT);
        st.mode = stat_posix_mode(&st);   /* Q12 */
        if (copy_to_user((void *)(uintptr_t)a2, &st, sizeof(st)) != 0) return (uint64_t)-EFAULT;
        return 0;
    }
    case SYS_SYMLINK: {
        char target[SYSCALL_PATH_MAX], linkp[SYSCALL_PATH_MAX];
        if (copy_user_path(target, a1) != 0 || copy_user_path(linkp, a2) != 0)
            return (uint64_t)-EFAULT;
        return (uint64_t)vfs_symlink(target, linkp);
    }
    case SYS_READLINK: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        size_t bufsiz = (size_t)a3;
        if (bufsiz == 0) return (uint64_t)-EINVAL;
        if (bufsiz > VFS_PATH_MAX) bufsiz = VFS_PATH_MAX;
        if (!validate_user_range((void *)(uintptr_t)a2, bufsiz, 1)) return (uint64_t)-EFAULT;
        char kbuf[VFS_PATH_MAX];
        int r = vfs_readlink(path, kbuf, bufsiz);
        if (r < 0) return (uint64_t)r;
        if (copy_to_user((void *)(uintptr_t)a2, kbuf, (uint64_t)r) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)r;
    }
    case SYS_GETCWD:
        return (uint64_t)do_getcwd((char*)(uintptr_t)a1, (size_t)a2);
    case SYS_CHDIR:
        return (uint64_t)do_chdir((const char*)(uintptr_t)a1);

    /* ---- P9: pthread / clone / futex ---- */
    case SYS_CLONE:
        return (uint64_t)do_clone(a1, a2, a3, a4, a5);
    case SYS_ARCH_PRCTL:
        return (uint64_t)do_arch_prctl((int)a1, a2);
    case SYS_FUTEX:
        return (uint64_t)do_futex(a1, (int)a2, (uint32_t)a3, a4, (uint32_t *)(uintptr_t)a5, (uint32_t)a6);
    case SYS_TKILL:
        return (uint64_t)do_tkill((int64_t)a1, (int)a2);

    /* H2: expose PMM free-frame count for memory-reaping integration tests. */
    case SYS_MEMINFO: {
        extern uint64_t pmm_get_free_frames(void);
        return pmm_get_free_frames();
    }

    /* FIX_R8: keyboard layout get/set/enum.
     *   a1 = 0: copy the active layout's name into (a2, a3 bytes).
     *   a1 = 1: switch to the layout whose name is the string at a2.
     *   a1 = 2: copy keymap_registry[a2]'s name into a3 (KBD_LAYOUT_NAME_MAX
     *           bytes) so shells can enumerate without knowing the list.
     * No vfs_errno() anywhere near this: -ENOENT is -2, but keyboard_set_-
     * layout() may gain -EPERM-class returns later (write protection), and
     * EPERM == 1 == the generic sentinel — return values verbatim. */
    case SYS_KBD_LAYOUT: {
        if (a1 == 0) {
            if (!a2 || a3 == 0) return (uint64_t)-EINVAL;
            if (!validate_user_range((void *)(uintptr_t)a2, a3, 1)) return (uint64_t)-EFAULT;
            const char *nm = keyboard_get_layout();
            uint64_t len = 0;
            while (nm[len] && len + 1 < a3) {
                ((char *)(uintptr_t)a2)[len] = nm[len];
                len++;
            }
            ((char *)(uintptr_t)a2)[len] = '\0';
            return 0;
        }
        if (a1 == 1) {
            char nm[KBD_LAYOUT_NAME_MAX];
            if (copy_string_from_user(nm, (const char *)(uintptr_t)a2, sizeof(nm)) != 0) {
                return (uint64_t)-EFAULT;
            }
            if (nm[0] == '\0') return (uint64_t)-EINVAL;
            return (uint64_t)keyboard_set_layout(nm);   /* 0 or -ENOENT */
        }
        if (a1 == 2) {
            if (!a3) return (uint64_t)-EINVAL;
            if (!validate_user_range((void *)(uintptr_t)a3, KBD_LAYOUT_NAME_MAX, 1)) {
                return (uint64_t)-EFAULT;
            }
            int found = -ENOENT;
            for (int i = 0; keymap_registry[i]; i++) {
                if ((uint64_t)i == a2) {
                    char knm[KBD_LAYOUT_NAME_MAX];
                    uint64_t len = 0;
                    const char *nm = keymap_registry[i]->name;
                    while (nm[len] && len + 1 < sizeof(knm)) { knm[len] = nm[len]; len++; }
                    knm[len] = '\0';
                    if (copy_to_user((void *)(uintptr_t)a3, knm, sizeof(knm)) != 0) {
                        return (uint64_t)-EFAULT;
                    }
                    found = 0;
                    break;
                }
            }
            return (uint64_t)found;
        }
        return (uint64_t)-EINVAL;
    }




    /* ---- Q5: AT-family syscalls ---- */
    case 257: { /* SYS_OPENAT */
        int dirfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int r = copy_at_path(path, a2, dirfd);
        if (r != 0) return (uint64_t)r;
        /* No vfs_errno() — see SYS_OPEN: it would turn EPERM into ENOENT. */
        return (uint64_t)vfs_open(path, (int)a3, (int)a4);
    }
    case 258: { /* SYS_MKDIRAT */
        int dirfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int r = copy_at_path(path, a2, dirfd);
        if (r != 0) return (uint64_t)r;
        return (uint64_t)vfs_errno(vfs_mkdir(path, (uint32_t)a3), EACCES);
    }
    case 260: { /* SYS_FCHOWNAT */
        int dfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int r = copy_at_path(path, a2, dfd);
        if (r != 0) return (uint64_t)r;
        return (uint64_t)vfs_chown(path, (uint32_t)a3, (uint32_t)a4);
    }
    case 262: { /* SYS_FSTATAT */
        int dirfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int flags = (int)a4;
        int r = copy_at_path(path, a2, dirfd);
        if (r != 0) return (uint64_t)r;
        struct vfs_stat st;
        int rs = (flags & AT_SYMLINK_NOFOLLOW)
                 ? vfs_lstat(path, &st) : vfs_stat(path, &st);
        if (rs != 0) return (uint64_t)vfs_errno(rs, ENOENT);
        st.mode = stat_posix_mode(&st);   /* Q12 */
        if (copy_to_user((void*)(uintptr_t)a3, &st, sizeof(st)) != 0)
            return (uint64_t)-EFAULT;
        return 0;
    }
    case 263: { /* SYS_UNLINKAT */
        int dirfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int flags = (int)a3;
        int r = copy_at_path(path, a2, dirfd);
        if (r != 0) return (uint64_t)r;
        if (flags & AT_REMOVEDIR) {
            /* POSIX: AT_REMOVEDIR demands a directory; a symlink or regular
             * file must give ENOTDIR (the fs rmdir hooks only see entries
             * in their own tables, and symlinks live in the kernel's global
             * table, so the type check belongs here in the dispatcher). */
            if (vfs_symlink_vnode(path) != NULL) return (uint64_t)-ENOTDIR;
            struct vfs_stat st;
            if (vfs_stat(path, &st) == 0 && st.type != VFS_TYPE_DIR)
                return (uint64_t)-ENOTDIR;
            return (uint64_t)vfs_errno(vfs_rmdir(path), ENOENT);
        }
        return (uint64_t)vfs_errno(vfs_unlink(path), ENOENT);
    }
    case 264: { /* SYS_RENAMEAT */
        int od = (int)a1;
        int nd = (int)a3;
        char op[SYSCALL_PATH_MAX], np[SYSCALL_PATH_MAX];
        int r = copy_at_path(op, a2, od);
        if (r != 0) return (uint64_t)r;
        r = copy_at_path(np, a4, nd);
        if (r != 0) return (uint64_t)r;
        return (uint64_t)vfs_errno(vfs_rename(op, np), ENOENT);
    }
    case 267: { /* SYS_READLINKAT */
        int dirfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int r = copy_at_path(path, a2, dirfd);
        if (r != 0) return (uint64_t)r;
        size_t bufsiz = (size_t)a4;
        if (bufsiz == 0) return (uint64_t)-EINVAL;
        if (bufsiz > SYSCALL_PATH_MAX) bufsiz = SYSCALL_PATH_MAX;
        if (!validate_user_range((void*)(uintptr_t)a3, bufsiz, 1))
            return (uint64_t)-EFAULT;
        char kbuf[SYSCALL_PATH_MAX];
        int64_t n = vfs_readlink(path, kbuf, bufsiz);
        if (n < 0) return (uint64_t)n;
        if (copy_to_user((void*)(uintptr_t)a3, kbuf, (uint64_t)n) != 0)
            return (uint64_t)-EFAULT;
        return (uint64_t)n;
    }
    case 268: { /* SYS_FCHMODAT */
        int dfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int r = copy_at_path(path, a2, dfd);
        if (r != 0) return (uint64_t)r;
        return (uint64_t)vfs_chmod(path, (uint32_t)a3);
    }
    case 269: { /* SYS_FACCESSAT */
        int dfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int r = copy_at_path(path, a2, dfd);
        if (r != 0) return (uint64_t)r;
        return (uint64_t)vfs_access(path, (int)a3);
    }

    /* ---- Q13: AT-family completion ---- */

    case SYS_LINK: { /* 90: link(2) */
        char old[SYSCALL_PATH_MAX], new_[SYSCALL_PATH_MAX];
        if (copy_user_path(old, a1) != 0) return (uint64_t)-EFAULT;
        if (copy_user_path(new_, a2) != 0) return (uint64_t)-EFAULT;
        /* No vfs_errno() wrapper: vfs_link() returns specific negative
         * errnos, and -EPERM (== -1, the generic sentinel vfs_errno would
         * rewrite to the fallback) is a legitimate link(2) result. */
        return (uint64_t)vfs_link(old, new_);
    }
    case SYS_LINKAT: { /* 265 */
        int od = (int)a1, nd = (int)a3;
        (void)a5;   /* flags: symlink links unsupported -> EPERM regardless */
        char op[SYSCALL_PATH_MAX], np[SYSCALL_PATH_MAX];
        int r = copy_at_path(op, a2, od);
        if (r != 0) return (uint64_t)r;
        r = copy_at_path(np, a4, nd);
        if (r != 0) return (uint64_t)r;
        return (uint64_t)vfs_link(op, np);
    }
    case SYS_SYMLINKAT: { /* 266 */
        int dfd = (int)a1;
        char tgt[SYSCALL_PATH_MAX], lp[SYSCALL_PATH_MAX];
        if (copy_user_path(tgt, a2) != 0) return (uint64_t)-EFAULT;
        int r = copy_at_path(lp, a3, dfd);
        if (r != 0) return (uint64_t)r;
        return (uint64_t)vfs_errno(vfs_symlink(tgt, lp), EEXIST);
    }
    case SYS_MKNODAT: { /* 259 */
        int dfd = (int)a1;
        char path[SYSCALL_PATH_MAX];
        int r = copy_at_path(path, a2, dfd);
        if (r != 0) return (uint64_t)r;
        uint32_t mode = (uint32_t)a3;
        uint32_t typ = mode & Q13_S_IFMT;
        if (typ == Q13_S_IFIFO) {
            return (uint64_t)vfs_errno(vfs_mkfifo(path, mode & 07777u), EACCES);
        }
        if (typ == Q13_S_IFREG) {
            /* mknod(2) semantics: create a regular file, fail if it exists.
             * The syscall's contract is 0-on-success; vfs_open hands back an
             * fd, so close it and return 0.  vfs_open's errnos are specific
             * (never the -1 sentinel), so pass the raw value through. */
            int fd = vfs_open(path, O_CREAT | O_EXCL | O_WRONLY, mode & 07777u);
            if (fd >= 0) {
                vfs_close(fd);
                return 0;
            }
            return (uint64_t)fd;
        }
        if (typ == Q13_S_IFCHR || typ == Q13_S_IFBLK)
            return (uint64_t)-ENOSYS;   /* devfs has no backing for nodes */
        return (uint64_t)-EINVAL;
    }
    case SYS_UTIMENSAT: { /* 280: utimensat(2) and futimens(2) */
        int dfd = (int)a1;
        uint64_t atime, mtime;
        int r = utimens_resolve(a3, &atime, &mtime);
        if (r != 0) return (uint64_t)r;
        if (a2 == 0) {   /* path == NULL: operate on the fd (futimens) */
            return (uint64_t)vfs_errno(vfs_fsettimes((int)dfd, atime, mtime),
                                       EBADF);
        }
        char path[SYSCALL_PATH_MAX];
        r = copy_at_path(path, a2, dfd);
        if (r != 0) return (uint64_t)r;
        int nofollow = ((int)a4 & AT_SYMLINK_NOFOLLOW) ? 1 : 0;
        return (uint64_t)vfs_errno(vfs_settimes(path, atime, mtime, nofollow),
                                   ENOENT);
    }

    case 292: { /* SYS_DUP3 */
        int old = (int)a1, new_ = (int)a2, flags = (int)a3;
        int r = vfs_dup2(old, new_);
        if (r >= 0 && (flags & 0x80000)) {
            tcb_t *cur = sched_current();
            if (cur && r < 64)
                cur->cloexec[r] = 1;
        }
        return (uint64_t)vfs_errno(r, EBADF);
    }
    case 322: { /* SYS_EXECVEAT */
        int dfd = (int)a1;
        int flags = (int)a5;
        char path[256];
        if (flags & 0x1000) { /* AT_EMPTY_PATH */
            /* fd-based exec: use /proc/self/fd/<dfd> as path */
            ksnprintf(path, sizeof(path), "/proc/self/fd/%d", dfd);
        } else {
            if (copy_user_path(path, a2) != 0) return (uint64_t)-EFAULT;
        }
        return (uint64_t)vfs_errno((int64_t)do_execve(path, a3, a4), ENOENT);
    }

    /* Q11: POSIX.1-2024 new functions */
    case 318: { /* SYS_GETENTROPY */
        void *buf = (void*)(uintptr_t)a1;
        uint64_t len = a2;
        if (len > 256) return (uint64_t)-EIO;
        if (!validate_user_range(buf, len, 1)) return (uint64_t)-EFAULT;
        uint8_t *kbuf = kmalloc(len ? len : 1);
        if (!kbuf) return (uint64_t)-ENOMEM;
        /* N0: the ChaCha20 CSPRNG refuses loudly (-ENOSYS) until real
         * entropy exists; never serve guessable bytes. */
        if (rng_try_fill(kbuf, len) != 0) {
            kfree(kbuf);
            return (uint64_t)-ENOSYS;
        }
        if (copy_to_user(buf, kbuf, len) != 0) {
            kfree(kbuf);
            return (uint64_t)-EFAULT;
        }
        kfree(kbuf);
        return 0;
    }
    /* Q16: getrandom(2) — Linux-compatible flags, draws from the same
     * ChaCha20 CSPRNG as getentropy (N0).  Until the generator is ready
     * (hardware RNG or a sufficiently stirred jitter pool), it BLOCKS —
     * like Linux's pre-init /dev/random — unless GRND_NONBLOCK, which
     * returns -EAGAIN.  Unknown flags are rejected with EINVAL. */
    case SYS_GETRANDOM: {
        void *buf = (void*)(uintptr_t)a1;
        uint64_t buflen = a2;
        uint32_t flags = (uint32_t)a3;
        if (flags & ~(1u | 2u)) return (uint64_t)-EINVAL;   /* GRND_NONBLOCK|GRND_RANDOM */
        if (buflen == 0) return 0;
        if (buflen > 65536) return (uint64_t)-EINVAL;       /* sanity bound */
        if (!buf || !validate_user_range(buf, buflen, 1)) return (uint64_t)-EFAULT;
        if (!rng_available()) {
            if (flags & 1u) return (uint64_t)-EAGAIN;       /* GRND_NONBLOCK */
            /* O7: block on the seeded event instead of yield-polling.
             * The 5-tick net bounds the lost-wakeup window (seeding
             * completing between the check and the sleep); the 30 s
             * give-up stays — a machine whose entropy source is
             * genuinely dead should fail the call, not hang it. */
            uint64_t start = timer_get_ticks();
            while (!rng_available()) {
                if (timer_get_ticks() - start > 3000) return (uint64_t)-EAGAIN;
                wq_wait_deadline(&rng_ready_wq, NULL, timer_get_ticks() + 5);
            }
        }
        uint8_t *kbuf = kmalloc(buflen);
        if (!kbuf) return (uint64_t)-ENOMEM;
        if (rng_try_fill(kbuf, buflen) != 0) {
            kfree(kbuf);
            return (uint64_t)-EAGAIN;
        }
        if (copy_to_user(buf, kbuf, buflen) != 0) {
            kfree(kbuf);
            return (uint64_t)-EFAULT;
        }
        kfree(kbuf);
        return (uint64_t)buflen;
    }
    case 436: { /* SYS_CLOSE_RANGE */
        unsigned first=(unsigned)a1, last=(unsigned)a2;
        tcb_t *cur=sched_current(); if(!cur) return (uint64_t)-EBADF;
        unsigned max_fds = 64;
        for(unsigned i=first; i<=last && i<max_fds; i++) vfs_close((int)i);
        return 0;
    }
    case 24: { /* SYS_SCHED_YIELD */
        sched_yield();
        return 0;
    }

    /* ---- Q14: System V IPC ---- */
    case SYS_SEMGET:
        return (uint64_t)sysv_semget((int64_t)a1, (int)a2, (int)a3);
    case SYS_SEMOP:
        return (uint64_t)sysv_semop((int)a1, (const void *)(uintptr_t)a2, a3);
    case SYS_SEMCTL:
        return (uint64_t)sysv_semctl((int)a1, (int)a2, (int)a3, a4);
    case SYS_SHMGET:
        return (uint64_t)sysv_shmget((int64_t)a1, a2, (int)a3);
    case SYS_SHMAT:
        return (uint64_t)sysv_shmat((int)a1, a2, (int)a3);
    case SYS_SHMDT:
        return (uint64_t)sysv_shmdt(a1);
    case SYS_SHMCTL:
        return (uint64_t)sysv_shmctl((int)a1, (int)a2, a3);
    case SYS_MSGGET:
        return (uint64_t)sysv_msgget((int64_t)a1, (int)a2);
    case SYS_MSGSND:
        return (uint64_t)sysv_msgsnd((int)a1, (const void *)(uintptr_t)a2, a3, (int)a4);
    case SYS_MSGRCV:
        return (uint64_t)sysv_msgrcv((int)a1, (void *)(uintptr_t)a2, a3,
                                     (int64_t)a4, (int)a5);
    case SYS_MSGCTL:
        return (uint64_t)sysv_msgctl((int)a1, (int)a2, a3);

    default:
        kprintf("[syscall] unknown syscall %llu\n", (unsigned long long)num);
        return (uint64_t)-ENOSYS;   /* reserved for unimplemented syscall nrs */
    }
}
