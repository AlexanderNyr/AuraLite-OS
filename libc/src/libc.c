/*
 * libc.c — C wrappers around the generic syscall() for AuraLite OS user programs.
 * Compiled with the host compiler and linked into user binaries.
 */

#include "unistd.h"

ssize_t write(int fd, const void *buf, size_t count) {
    return syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count,
                   0, 0, 0);
}

ssize_t read(int fd, void *buf, size_t count) {
    return syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count,
                   0, 0, 0);
}

void _exit(int code) {
    syscall(SYS_EXIT, (uint64_t)code, 0, 0, 0, 0, 0);
}

pid_t getpid(void) {
    return (pid_t)syscall(39, 0, 0, 0, 0, 0, 0);
}
