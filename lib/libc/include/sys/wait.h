#ifndef AURALITE_LIBC_SYS_WAIT_H
#define AURALITE_LIBC_SYS_WAIT_H

/*
 * sys/wait.h — process wait (POSIX.1-2017).
 *
 * Status word encoding (matches the kernel do_waitpid):
 *   normal exit : (code & 0xff) << 8     -> WIFEXITED, WEXITSTATUS = high byte
 *   signal death: signo (low 7 bits)     -> WIFSIGNALED, WTERMSIG = low 7 bits
 */

#include <stddef.h>

#ifndef AURALITE_TYPE_PID_T
#define AURALITE_TYPE_PID_T
typedef long pid_t;
#endif

/* waitpid options. */
#define WNOHANG   1
#define WUNTRACED 2

/* Status inspection macros. */
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSIGNALED(s)  (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WCOREDUMP(s)    (0)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* AURALITE_LIBC_SYS_WAIT_H */
