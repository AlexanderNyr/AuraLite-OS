/* syscall.c — system call dispatch.
 *
 * Phase 11+ syscall surface for the shell, user programs, networking, VFS and
 * GUI.  User pointers are validated/copied through usercopy helpers before the
 * kernel dereferences them.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/process.h"
#include "kernel/proc/usercopy.h"
#include "kernel/fs/vfs.h"
#include "kernel/net/net.h"
#include "kernel/net/tcp.h"
#include "kernel/net/socket.h"
#include "drivers/uart/uart.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/timer/pit.h"
#include "kernel/gui/gui_syscalls.h"
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
#define SYS_MMAP     9
#define SYS_MUNMAP   11
#define SYS_BRK      12
/* Socket-style networking API. */
#define SYS_SOCKET        300
#define SYS_SOCKET_CONNECT 301
#define SYS_SOCKET_SEND    302
#define SYS_SOCKET_RECV    303
#define SYS_SOCKET_CLOSE   304

/* File-descriptor extensions. */
#define SYS_DUP    32
#define SYS_DUP2   33
#define SYS_PIPE   22
#define SYS_FCNTL  72

/* fcntl commands.  Small subset matching Linux numbers. */
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1

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
#define MAP_PRIVATE           0x02
#define MAP_FIXED             0x10
#define MAP_ANONYMOUS         0x20

static int copy_user_path(char *dst, uint64_t user_path) {
    return copy_string_from_user(dst, (const char *)(uintptr_t)user_path,
                                 SYSCALL_PATH_MAX);
}

static uint64_t syscall_vfs_write(int fd, const void *user_buf, uint64_t len) {
    if (len == 0) return 0;
    if (!validate_user_range(user_buf, len, 0)) return (uint64_t)-1;

    char tmp[SYSCALL_IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        if (copy_from_user(tmp, (const uint8_t *)user_buf + done, n) != 0) {
            return (uint64_t)-1;
        }
        int64_t wr = vfs_write(fd, tmp, n);
        if (wr < 0) return (uint64_t)-1;
        done += (uint64_t)wr;
        if ((uint64_t)wr < n) break;
    }
    return done;
}

static uint64_t syscall_vfs_read(int fd, void *user_buf, uint64_t len) {
    if (len == 0) return 0;
    if (!validate_user_range(user_buf, len, 1)) return (uint64_t)-1;

    char tmp[SYSCALL_IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        int64_t rd = vfs_read(fd, tmp, n);
        if (rd < 0) return (uint64_t)-1;
        if (rd == 0) break;
        if (copy_to_user((uint8_t *)user_buf + done, tmp, (uint64_t)rd) != 0) {
            return (uint64_t)-1;
        }
        done += (uint64_t)rd;
        if ((uint64_t)rd < n) break;
    }
    return done;
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
    if (!cur || len == 0) return (uint64_t)-1;

    /* First implementation: eager MAP_PRIVATE mappings.  Anonymous mappings
     * are zero-filled; file-backed mappings read the file contents now (not a
     * shared page-cache VMA yet).  PROT_NONE would need VMA fault bookkeeping,
     * so reject it for now. */
    int anonymous = (flags & MAP_ANONYMOUS) ? 1 : 0;
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0 || prot == 0) {
        return (uint64_t)-1;
    }
    if (!(flags & MAP_PRIVATE)) return (uint64_t)-1;
    if (anonymous) {
        if (fd != (uint64_t)-1) return (uint64_t)-1;
    } else {
        if (fd == (uint64_t)-1 || (off & (PAGE_SIZE_BYTES - 1ULL)) ||
            off > 0x7FFFFFFFFFFFFFFFULL) {
            return (uint64_t)-1;
        }
    }

    len = align_up_u64(len, PAGE_SIZE_BYTES);
    if (len == 0 || len > (USER_MMAP_MAX - USER_MMAP_BASE)) return (uint64_t)-1;

    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE_BYTES - 1ULL)) return (uint64_t)-1;
        if (!user_mmap_range_ok(addr, len) || !user_range_is_free(addr, len)) {
            return (uint64_t)-1;
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
        if (addr == 0) return (uint64_t)-1;
        cur->mmap_next = addr + len;
    }

    uint64_t pte_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (prot & PROT_WRITE) pte_flags |= PAGE_FLAG_WRITABLE;
    if (!(prot & PROT_EXEC)) pte_flags |= PAGE_FLAG_NO_EXEC;

    int64_t old_pos = -1;
    if (!anonymous) {
        old_pos = vfs_lseek((int)fd, 0, 1);
        if (old_pos < 0) return (uint64_t)-1;
        if (vfs_lseek((int)fd, (int64_t)off, 0) < 0) {
            (void)vfs_lseek((int)fd, old_pos, 0);
            return (uint64_t)-1;
        }
    }

    uint64_t hhdm = limine_get_hhdm_offset();
    uint64_t mapped = 0;
    for (; mapped < len; mapped += PAGE_SIZE_BYTES) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) goto fail;
        memset((void *)(uintptr_t)(hhdm + phys), 0, PAGE_SIZE_BYTES);
        if (!anonymous) {
            int64_t rd = vfs_read((int)fd, (void *)(uintptr_t)(hhdm + phys),
                                  PAGE_SIZE_BYTES);
            if (rd < 0) {
                pmm_free_frame(phys);
                goto fail;
            }
        }
        paging_map(addr + mapped, phys, pte_flags);
    }
    if (!anonymous) (void)vfs_lseek((int)fd, old_pos, 0);
    return addr;

fail:
    if (!anonymous && old_pos >= 0) (void)vfs_lseek((int)fd, old_pos, 0);
    for (uint64_t off2 = 0; off2 < mapped; off2 += PAGE_SIZE_BYTES) {
        uint64_t phys = paging_get_phys(addr + off2);
        if (phys) {
            paging_unmap(addr + off2);
            pmm_free_frame(phys);
        }
    }
    return (uint64_t)-1;
}

static uint64_t syscall_munmap(uint64_t addr, uint64_t len) {
    if (len == 0) return (uint64_t)-1;
    if (addr & (PAGE_SIZE_BYTES - 1ULL)) return (uint64_t)-1;
    len = align_up_u64(len, PAGE_SIZE_BYTES);
    if (!user_mmap_range_ok(addr, len)) return (uint64_t)-1;

    for (uint64_t off = 0; off < len; off += PAGE_SIZE_BYTES) {
        uint64_t virt = addr + off;
        uint64_t phys = paging_get_phys(virt);
        if (phys) {
            paging_unmap(virt);
            pmm_free_frame(phys);
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
    }

    switch (num) {
    case SYS_WRITE: {
        /* a1 = fd, a2 = buffer, a3 = length. fd 1/2 go to console; fd >= 3
         * writes through the VFS (tmpfs/devfs/etc.). */
        const void *user_buf = (const void *)(uintptr_t)a2;
        if (a3 != 0 && !validate_user_range(user_buf, a3, 0)) {
            return (uint64_t)-1;
        }
        if (a1 == 1 || a1 == 2) {
            char tmp[SYSCALL_IO_CHUNK];
            uint64_t done = 0;
            while (done < a3) {
                uint64_t n = a3 - done;
                if (n > sizeof(tmp)) n = sizeof(tmp);
                if (copy_from_user(tmp, (const uint8_t *)user_buf + done, n) != 0) {
                    return (uint64_t)-1;
                }
                for (uint64_t i = 0; i < n; i++) kputchar(tmp[i]);
                done += n;
            }
            return a3;
        }
        return syscall_vfs_write((int)a1, user_buf, a3);
    }
    case SYS_READ: {
        /* a1 = fd, a2 = buffer, a3 = count. */
        int fd = (int)a1;
        void *user_buf = (void *)(uintptr_t)a2;
        if (a3 != 0 && !validate_user_range(user_buf, a3, 1)) {
            return (uint64_t)-1;
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
                    return (uint64_t)-1;
                }
                got++;
                kputchar(c);   /* echo */
                if (c == '\n') break;
            }
            return got;
        }
        return syscall_vfs_read(fd, user_buf, a3);
    }
    case SYS_OPEN: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        return (uint64_t)vfs_open(path);
    }
    case SYS_CLOSE:
        return (uint64_t)vfs_close((int)a1);
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
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        return do_execve(path);
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
        if (a2 == 0 && a1 >= 0x1000 && a1 < 0x0000800000000000ULL) {
            pid = -1;
            user_status = (void *)(uintptr_t)a1;
        }
        int64_t status = 0;
        int64_t ret = do_wait4_pid(pid, user_status ? &status : 0);
        if (ret >= 0 && user_status) {
            if (copy_to_user(user_status, &status, sizeof(status)) != 0) {
                return (uint64_t)-1;
            }
        }
        return (uint64_t)ret;
    }
    case SYS_SPAWN: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        return process_spawn(path);
    }
    case SYS_LISTDIR: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        if (a2 && a3 > 0) {
            int max = (int)a3;
            if (max > VFS_MAX_DIRENTS) max = VFS_MAX_DIRENTS;
            if (max <= 0) return (uint64_t)-1;
            uint64_t bytes = (uint64_t)max * sizeof(struct vfs_dirent);
            if (!validate_user_range((void *)(uintptr_t)a2, bytes, 1)) {
                return (uint64_t)-1;
            }
            struct vfs_dirent *kents = kmalloc((size_t)bytes);
            if (!kents) return (uint64_t)-1;
            int n = vfs_readdir(path, kents, max);
            if (n > 0) {
                uint64_t used = (uint64_t)n * sizeof(struct vfs_dirent);
                if (copy_to_user((void *)(uintptr_t)a2, kents, used) != 0) {
                    kfree(kents);
                    return (uint64_t)-1;
                }
            }
            kfree(kents);
            return (uint64_t)n;
        } else {
            vfs_list(path);
            return 0;
        }
    }
    case SYS_DNS: {
        char host[SYSCALL_PATH_MAX];
        if (copy_string_from_user(host, (const char *)(uintptr_t)a1, sizeof(host)) != 0) {
            return (uint64_t)-1;
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
        if (!validate_user_range(user_buf, a3, 0)) return (uint64_t)-1;
        char tmp[SYSCALL_IO_CHUNK];
        uint64_t sent = 0;
        while (sent < a3) {
            uint64_t n = a3 - sent;
            if (n > sizeof(tmp)) n = sizeof(tmp);
            if (copy_from_user(tmp, (const uint8_t *)user_buf + sent, n) != 0) return (uint64_t)-1;
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
        if (!validate_user_range(user_buf, a3, 1)) return (uint64_t)-1;
        char tmp[SYSCALL_IO_CHUNK];
        uint32_t n = (a3 > sizeof(tmp)) ? (uint32_t)sizeof(tmp) : (uint32_t)a3;
        int64_t r = socket_recv((int)a1, tmp, n);
        if (r <= 0) return (uint64_t)r;
        if (copy_to_user(user_buf, tmp, (uint64_t)r) != 0) return (uint64_t)-1;
        return (uint64_t)r;
    }
    case SYS_SOCKET_CLOSE:
        return (uint64_t)socket_close((int)a1);
    case SYS_NET_CONNECT:
        return (uint64_t)tcp_connect(a1, (uint16_t)a2);
    case SYS_NET_SEND: {
        if (a2 == 0) return 0;
        const void *user_buf = (const void *)(uintptr_t)a1;
        if (!validate_user_range(user_buf, a2, 0)) return (uint64_t)-1;
        char tmp[SYSCALL_IO_CHUNK];
        uint64_t sent = 0;
        while (sent < a2) {
            uint64_t n = a2 - sent;
            if (n > sizeof(tmp)) n = sizeof(tmp);
            if (copy_from_user(tmp, (const uint8_t *)user_buf + sent, n) != 0) {
                return (uint64_t)-1;
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
        if (!validate_user_range(user_buf, a2, 1)) return (uint64_t)-1;
        char tmp[SYSCALL_IO_CHUNK];
        uint32_t n = (a2 > sizeof(tmp)) ? (uint32_t)sizeof(tmp) : (uint32_t)a2;
        int64_t r = tcp_recv(tmp, n);
        if (r <= 0) return (uint64_t)r;
        if (copy_to_user(user_buf, tmp, (uint64_t)r) != 0) return (uint64_t)-1;
        return (uint64_t)r;
    }
    case SYS_NET_CLOSE:
        return (uint64_t)tcp_close();
    case SYS_NET_PING:
        return (uint64_t)net_ping(a1);

    /* Filesystem extensions. */
    case SYS_MKDIR: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        return (uint64_t)vfs_mkdir(path);
    }
    case SYS_RMDIR: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        return (uint64_t)vfs_rmdir(path);
    }
    case SYS_UNLINK: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        return (uint64_t)vfs_unlink(path);
    }
    case SYS_RENAME: {
        char from[SYSCALL_PATH_MAX], to[SYSCALL_PATH_MAX];
        if (copy_user_path(from, a1) != 0) return (uint64_t)-1;
        if (copy_user_path(to, a2) != 0) return (uint64_t)-1;
        return (uint64_t)vfs_rename(from, to);
    }
    case SYS_TRUNCATE: {
        char path[SYSCALL_PATH_MAX];
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        return (uint64_t)vfs_truncate(path, a2);
    }
    case SYS_STAT: {
        char path[SYSCALL_PATH_MAX];
        struct vfs_stat st;
        if (copy_user_path(path, a1) != 0) return (uint64_t)-1;
        if (!validate_user_range((void *)(uintptr_t)a2, sizeof(st), 1)) return (uint64_t)-1;
        int r = vfs_stat(path, &st);
        if (r != 0) return (uint64_t)r;
        if (copy_to_user((void *)(uintptr_t)a2, &st, sizeof(st)) != 0) return (uint64_t)-1;
        return 0;
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
        return (uint64_t)vfs_dup((int)a1);
    case SYS_DUP2:
        return (uint64_t)vfs_dup2((int)a1, (int)a2);
    case SYS_PIPE: {
        int fds[2];
        if (!validate_user_range((void *)(uintptr_t)a1, sizeof(fds), 1)) {
            return (uint64_t)-1;
        }
        int r = vfs_pipe(fds);
        if (r != 0) return (uint64_t)-1;
        if (copy_to_user((void *)(uintptr_t)a1, fds, sizeof(fds)) != 0) {
            /* Roll back partially: close both ends. */
            vfs_close(fds[0]);
            vfs_close(fds[1]);
            return (uint64_t)-1;
        }
        return 0;
    }
    case SYS_FCNTL: {
        int fd = (int)a1;
        int cmd = (int)a2;
        switch (cmd) {
        case F_GETFD: return (uint64_t)vfs_get_cloexec(fd);
        case F_SETFD: return (uint64_t)vfs_set_cloexec(fd, (a3 & FD_CLOEXEC) ? 1 : 0);
        default:      return (uint64_t)-1;
        }
    }
    case SYS_MMAP:
        return syscall_mmap(a1, a2, a3, a4, a5, a6);
    case SYS_MUNMAP:
        return syscall_munmap(a1, a2);
    case SYS_BRK: {
        tcb_t *cur = sched_current();
        if (!cur) return (uint64_t)-1;

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
    default:
        kprintf("[syscall] unknown syscall %llu\n", (unsigned long long)num);
        return (uint64_t)-1;
    }
}
