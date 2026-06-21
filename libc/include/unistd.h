#ifndef AURALITE_LIBC_UNISTD_H
#define AURALITE_LIBC_UNISTD_H

/*
 * Minimal libc unistd declarations for AuraLite OS user programs.
 * These are thin wrappers around SYSCALL/SYSRET (see libc/src/syscall.asm).
 */

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers (Linux-compatible subset). */
#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_EXIT  60

/* ssize_t / pid_t stand-ins (kept simple for the freestanding libc). */
typedef int64_t ssize_t;
typedef int64_t pid_t;

/*
 * Generic syscall: num in the first argument, up to 6 more arguments.
 * Implemented in libc/src/syscall.asm (remaps C ABI -> SYSCALL ABI).
 */
int64_t syscall(int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6);

/* POSIX-style wrappers. */
ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);
void    _exit(int code);
pid_t   getpid(void);

#endif /* AURALITE_LIBC_UNISTD_H */
