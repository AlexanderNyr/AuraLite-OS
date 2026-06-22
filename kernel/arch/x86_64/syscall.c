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
#include "kernel/net/net.h"
#include "kernel/net/tcp.h"
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
#define SYS_DNS    82   /* non-standard: resolve a hostname */
#define SYS_NET_CONNECT 83  /* non-standard: TCP connect */
#define SYS_NET_SEND    84  /* non-standard: TCP send */
#define SYS_NET_RECV    85  /* non-standard: TCP recv */
#define SYS_NET_CLOSE   86  /* non-standard: TCP close */
#define SYS_NET_PING    87  /* non-standard: ICMP ping */
#define SYS_LISTDIR 80

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    switch (num) {
    case SYS_WRITE: {
        /* a1 = fd, a2 = buffer, a3 = length. fd 1/2 go to console; fd >= 3
         * writes through the VFS (tmpfs/devfs/etc.). */
        if (a1 == 1 || a1 == 2) {
            const char *buf = (const char *)a2;
            for (uint64_t i = 0; i < a3; i++) {
                kputchar(buf[i]);
            }
            return a3;
        }
        return (uint64_t)vfs_write((int)a1, (const void *)a2, a3);
    }
    case SYS_READ: {
        /* a1 = fd, a2 = buffer, a3 = count. */
        int fd = (int)a1;
        if (fd == 0) {
            /* stdin: serial line input. Poll the UART directly without
             * yielding (sched_yield from a SYSCALL handler that runs on the
             * user stack causes context-switch corruption). */
            char *cbuf = (char *)a2;
            uint64_t count = a3;
            uint64_t got = 0;
            while (got < count) {
                /* Spin-wait for UART data. This is acceptable for the serial
                 * console; the alternative (interrupt-driven) is a TODO. */
                while (!uart_has_data()) {
                    __asm__ volatile ("pause");
                }
                char c = uart_getchar();
                if (c == '\r') c = '\n';
                cbuf[got++] = c;
                kputchar(c);   /* echo */
                if (c == '\n') break;
            }
            return got;
        }
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
    case SYS_DNS:
        return net_dns_resolve((const char *)a1);
    case SYS_NET_CONNECT:
        /* a1 = IP (host order), a2 = port */
        return (uint64_t)tcp_connect(a1, (uint16_t)a2);
    case SYS_NET_SEND:
        /* a1 = data ptr, a2 = len */
        return (uint64_t)tcp_send((const void *)a1, (uint32_t)a2);
    case SYS_NET_RECV:
        /* a1 = buf ptr, a2 = bufsize */
        return (uint64_t)tcp_recv((void *)a1, (uint32_t)a2);
    case SYS_NET_CLOSE:
        return (uint64_t)tcp_close();
    case SYS_NET_PING:
        return (uint64_t)net_ping(a1);
    default:
        kprintf("[syscall] unknown syscall %llu\n", (unsigned long long)num);
        return (uint64_t)-1;
    }
}
