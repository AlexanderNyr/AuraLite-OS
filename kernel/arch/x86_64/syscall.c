/* syscall.c — system call dispatch.
 *
 * Phase 11+ syscall surface for the shell, user programs, networking, VFS and
 * GUI.  User pointers are validated/copied through usercopy helpers before the
 * kernel dereferences them.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/lib/kprintf.h"
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
#include "kernel/gui/gui_syscalls.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"

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

/* Saved user-mode RIP/RFLAGS from the syscall_entry asm stub.  Defined in
 * syscall_entry.asm; we read them once at the top of every dispatch and copy
 * them into the current TCB so that a nested syscall from a context-switch
 * partner can safely overwrite the globals.  On the way out we copy them
 * back so the asm sysret prologue lands at the right user RIP. */
extern uint64_t syscall_saved_rcx;
extern uint64_t syscall_saved_r11;

/* Called from syscall_entry.asm just before sysret.  Refreshes the
 * syscall_saved_* globals from the current TCB's per-thread copies.  Uses the
 * default SysV C ABI: no args, no return. */
void syscall_restore_user_frame(void) {
    tcb_t *cur = sched_current();
    if (!cur) return;
    if (cur->saved_user_rip) {
        syscall_saved_rcx = cur->saved_user_rip;
        syscall_saved_r11 = cur->saved_user_rflags;
    }
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;

    /* Capture the user-mode return frame into the current TCB so subsequent
     * context switches in this syscall cannot corrupt it via the globals. */
    tcb_t *cur = sched_current();
    if (cur) {
        cur->saved_user_rip    = syscall_saved_rcx;
        cur->saved_user_rflags = syscall_saved_r11;
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
                    __asm__ volatile ("pause");
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
            /* If a2 is non-zero, treat it as a buffer for readdir instead of legacy print */
            int n = vfs_readdir(path, (struct vfs_dirent *)(uintptr_t)a2, (int)a3);
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
    case SYS_BRK: {
        tcb_t *cur = sched_current();
        if (!cur) return (uint64_t)-1;
        
        if (a1 == 0) {
            return cur->brk; /* Just return current break */
        }
        
        uint64_t new_brk = (a1 + 4095) & ~4095ULL; /* Page align up */
        if (new_brk < cur->brk) {
            /* Shrinking not supported yet, just return current */
            return cur->brk; 
        }
        
        uint64_t pages_to_alloc = (new_brk - cur->brk) / 4096;
        for (uint64_t i = 0; i < pages_to_alloc; i++) {
            uint64_t virt = cur->brk + i * 4096;
            if (paging_get_phys(virt) == 0) {
                uint64_t phys = pmm_alloc_frame();
                if (!phys) {
                    /* OOM, return old break */
                    return cur->brk;
                }
                paging_map(virt, phys, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER);
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
