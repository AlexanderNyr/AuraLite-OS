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
#include "drivers/uart/uart.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/timer/pit.h"
#include "kernel/gui/gui_syscalls.h"
#include "kernel/time.h"
#include "kernel/sync/futex.h"
// clone.c compiled separately
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/limine_requests.h"

/* P10 types */
typedef struct {
    uint64_t fds_bits[64 / 64];
} fd_set;

/* struct kernel_timeval is provided by kernel/time.h (included above). */
// clone.c compiled separately   /* inline for simplicity in this build */
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/limine_requests.h"

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
/* Socket-style networking API. */
#define SYS_SOCKET        300
#define SYS_SOCKET_CONNECT 301
#define SYS_SOCKET_SEND    302
#define SYS_SOCKET_RECV    303
#define SYS_SOCKET_CLOSE   304
#define SYS_SOCKET_BIND    305
#define SYS_SOCKET_LISTEN  306
#define SYS_SOCKET_ACCEPT  307

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
#define SYS_MEMINFO        600   /* non-standard: returns pmm_get_free_frames() to userspace */

/* fcntl command numbers and the open-flag / FD_CLOEXEC values come from
 * kernel/fs/vfs.h (Linux/asm-generic ABI). */

#define SYSCALL_PATH_MAX 256
#define SYSCALL_IO_CHUNK 256

/* Keep the heap well below the user stack so brk growth cannot collide with
 * the fixed high-address stack mapping. */
#define USER_STACK_TOP        0x7FFFF0000000ULL
#define USER_STACK_SIZE       0x10000ULL
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
    if ((flags & MAP_SHARED) && !anonymous) return (uint64_t)-ENOSYS;
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
        file_ofd = cur->fd_table[fd];
    }

    /* Create the VMA descriptor. No physical allocation here. */
    if (vma_insert(&cur->vma_list, addr, addr + len,
                   vflags, file_ofd, off) != 0) {
        return (uint64_t)-ENOMEM;
    }

    /* MAP_POPULATE: eager allocation (optional). */
    if (flags & 0x4000) { /* MAP_POPULATE */
        uint64_t hhdm = limine_get_hhdm_offset();
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

    for (uint64_t off = 0; off < len; off += PAGE_SIZE_BYTES) {
        uint64_t virt = addr + off;
        uint64_t phys = paging_get_phys(virt);
        if (phys) {
            paging_unmap(virt);
            pmm_free_frame(phys);
        }
    }

    vma_remove_range(&cur->vma_list, addr, addr + len);
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

    vma_t *vma = vma_find(cur->vma_list, addr);
    if (!vma || vma->va_start != addr || vma->va_end < addr + len) {
        return (uint64_t)-ENOMEM;
    }

    uint32_t vflags = vma->flags & ~(VMA_READ | VMA_WRITE | VMA_EXEC);
    if (prot & PROT_READ)  vflags |= VMA_READ;
    if (prot & PROT_WRITE) vflags |= VMA_WRITE;
    if (prot & PROT_EXEC)  vflags |= VMA_EXEC;
    vma->flags = vflags;

    uint64_t pte_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (prot & PROT_WRITE) pte_flags |= PAGE_FLAG_WRITABLE;
    if (!(prot & PROT_EXEC)) pte_flags |= PAGE_FLAG_NO_EXEC;

    for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE_BYTES) {
        uint64_t phys = paging_get_phys(va);
        if (phys) {
            paging_map(va, phys, pte_flags);
        }
    }

    return 0;
}

/* Saved user-mode RIP/RFLAGS from the syscall_entry asm stub.  Defined in
 * syscall_entry.asm; we read them once at the top of every dispatch and copy
 * them into the current TCB so that a nested syscall from a context-switch
 * partner can safely overwrite the globals.  On the way out we copy them
 * back so the asm sysret prologue lands at the right user RIP. */
extern uint64_t syscall_saved_rcx;
extern uint64_t syscall_saved_r11;
extern uint64_t syscall_saved_rsp;
/* Live user callee-saved registers captured at the SYSCALL boundary
 * (defined in syscall_entry.asm). */
extern uint64_t syscall_saved_rbx;
extern uint64_t syscall_saved_rbp;
extern uint64_t syscall_saved_r12;
extern uint64_t syscall_saved_r13;
extern uint64_t syscall_saved_r14;
extern uint64_t syscall_saved_r15;

/* Called from syscall_entry.asm just before sysret.  Refreshes the
 * syscall_saved_* globals from the current TCB's per-thread copies.  Uses the
 * default SysV C ABI: no args, no return. */
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
     * the normal SYSRET path. */
}

int is_restartable(uint64_t num) {
    switch (num) {
        case SYS_READ:
        case SYS_WRITE:
        case SYS_WAIT4:
        case SYS_NANOSLEEP:
        case SYS_SELECT:
        case SYS_POLL:
        case SYS_FUTEX:
        case SYS_SOCKET_RECV:
        case SYS_SOCKET_SEND:
        case SYS_SOCKET_CONNECT:
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
            char tmp[SYSCALL_IO_CHUNK];
            uint64_t done = 0;
            while (done < a3) {
                uint64_t n = a3 - done;
                if (n > sizeof(tmp)) n = sizeof(tmp);
                if (copy_from_user(tmp, (const uint8_t *)user_buf + done, n) != 0) {
                    return (uint64_t)-EFAULT;
                }
                for (uint64_t i = 0; i < n; i++) kputchar(tmp[i]);
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
                     * "kicked" the guest.  Restore interrupts while waiting and
                     * yield so the compositor and input pollers keep running. */
                    __asm__ volatile ("sti" ::: "memory");
                    sched_yield();
                    if (timer_get_ticks() & 1ULL) {
                        __asm__ volatile ("hlt" ::: "memory");
                    } else {
                        __asm__ volatile ("pause");
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
        /* a1 = path, a2 = flags, a3 = mode.  vfs_open already returns specific
         * errno values; vfs_errno() is an idempotent safety net. */
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(vfs_open(path, (int)a2, (int)a3), ENOENT);
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
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-EFAULT;
        return (uint64_t)vfs_errno(process_spawn(path), ENOENT);
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
    case SYS_SOCKET:
        return (uint64_t)socket_create((int)a1, (int)a2, (int)a3);
    case SYS_SOCKET_CONNECT:
        return (uint64_t)socket_connect((int)a1, (uint32_t)a2, (uint16_t)a3);
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
            if (r < 0) return (uint64_t)-1;
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
        if (r < 0) return (uint64_t)-1;
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
            if (r < 0) return (uint64_t)-1;
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
    case SYS_MUNMAP:
        return syscall_munmap(a1, a2);
    case SYS_MPROTECT:
        return syscall_mprotect(a1, a2, a3);
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
        uint64_t hhdm = limine_get_hhdm_offset();
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
        return (uint64_t)do_select((int)a1, (fd_set*)(uintptr_t)a2,
                                   (fd_set*)(uintptr_t)a3, (fd_set*)(uintptr_t)a4,
                                   (struct kernel_timeval*)(uintptr_t)a5);
    }
    case SYS_FSTAT: {
        struct vfs_stat st;
        if (!validate_user_range((void *)(uintptr_t)a2, sizeof(st), 1)) return (uint64_t)-EFAULT;
        int r = vfs_fstat((int)a1, &st);
        if (r != 0) return (uint64_t)r;
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

    default:
        kprintf("[syscall] unknown syscall %llu\n", (unsigned long long)num);
        return (uint64_t)-ENOSYS;   /* reserved for unimplemented syscall nrs */
    }
}
