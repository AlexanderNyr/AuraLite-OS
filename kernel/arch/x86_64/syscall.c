/* syscall.c — system call dispatch.
 *
 * Phase 11: expanded set for the shell.
 *   SYS_READ (0):  fd 0 = serial input (line-buffered); fd 3+ = VFS read
 *   SYS_WRITE(1):  fd 1/2 = console output
 *   SYS_OPEN (2):  open a file from the VFS
 *   SYS_CLOSE(3):  close a file descriptor
 *   SYS_GETPID(39): current thread ID
 *   SYS_EXIT (60): terminate the calling thread
 *   SYS_LISTDIR(80): list files in a directory (non-standard, for `ls`)
 */

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/lib/kprintf.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/process.h"
#include "kernel/fs/vfs.h"
#include "drivers/uart/uart.h"

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
#define SYS_LISTDIR 80

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    switch (num) {
    case SYS_WRITE: {
        /* a1 = fd, a2 = buffer, a3 = length. fd 1 (stdout) and 2 (stderr). */
        if (a1 != 1 && a1 != 2) {
            return (uint64_t)-1;
        }
        const char *buf = (const char *)a2;
        for (uint64_t i = 0; i < a3; i++) {
            kputchar(buf[i]);
        }
        return a3;
    }
    case SYS_READ: {
        /* a1 = fd, a2 = buffer, a3 = count. */
        int fd = (int)a1;
        if (fd == 0) {
            /* stdin: serial line input. Block until a line is available. */
            char *buf = (char *)a2;
            uint64_t count = a3;
            uint64_t got = 0;
            while (got < count) {
                /* Poll the UART, yielding to other threads while idle. */
                while (!uart_has_data()) {
                    sched_yield();
                }
                char c = uart_getchar();
                /* Translate CR to LF (terminals send \r on Enter). */
                if (c == '\r') {
                    c = '\n';
                }
                buf[got++] = c;
                kputchar(c);   /* echo the character */
                if (c == '\n') {
                    break;      /* line complete */
                }
            }
            return got;
        }
        /* fd 3+: read from a VFS file. */
        return (uint64_t)vfs_read(fd, (void *)a2, a3);
    }
    case SYS_OPEN:
        return (uint64_t)vfs_open((const char *)a1);
    case SYS_CLOSE:
        return (uint64_t)vfs_close((int)a1);
    case SYS_EXIT:
        thread_exit();
        return 0;   /* unreachable */
    case SYS_GETPID: {
        tcb_t *cur = sched_current();
        return cur ? cur->id : 0;
    }
    case SYS_FORK:
        return do_fork();
    case SYS_EXECVE:
        return do_execve((const char *)a1);
    case SYS_WAIT4:
        return do_wait4((int64_t *)a1);
    case SYS_SPAWN:
        return process_spawn((const char *)a1);
    case SYS_LISTDIR:
        vfs_list((const char *)a1);
        return 0;
    default:
        kprintf("[syscall] unknown syscall %llu\n", (unsigned long long)num);
        return (uint64_t)-1;
    }
}
