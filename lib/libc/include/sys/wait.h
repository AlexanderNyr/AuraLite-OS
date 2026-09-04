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
#include <signal.h>   /* siginfo_t for waitid */

#ifndef AURALITE_TYPE_PID_T
#define AURALITE_TYPE_PID_T
typedef long pid_t;
#endif

/* waitpid options. */
#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   WUNTRACED
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000

/* Status inspection macros. */
#define WIFCONTINUED(s) ((s) == 0xffff)

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

/* RESIDUE2 T1: wait4 with rusage, and waitid.  idtype_t values match the
 * kernel's WAITID_P_* (and Linux's): P_ALL=0, P_PID=1, P_PGID=2. */
typedef int idtype_t;
#ifndef AURALITE_TYPE_ID_T
#define AURALITE_TYPE_ID_T
typedef unsigned int id_t;
#endif

struct rusage;   /* <sys/resource.h> */

pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage);
/* waitid takes siginfo_t*.  The freestanding libc's signal.h always
 * defines siginfo_t (__AURALITE__ builds, no feature macros).  A hosted
 * build (host unit tests compile this header standalone) under
 * -std=c11/__STRICT_ANSI__ hides glibc's siginfo_t behind
 * _POSIX_C_SOURCE -- hide the prototype in exactly those cases instead
 * of forcing feature macros onto every consumer. */
#if defined(__AURALITE__) || !defined(__STRICT_ANSI__) || \
    defined(_POSIX_C_SOURCE) || defined(_DEFAULT_SOURCE) || \
    defined(_GNU_SOURCE)
int   waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);
#endif

#endif /* AURALITE_LIBC_SYS_WAIT_H */
