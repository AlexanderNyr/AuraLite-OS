/* libc/src/resource.c — getrlimit / setrlimit (P10) */

#include <sys/resource.h>
#include <errno.h>

int getrlimit(int resource, struct rlimit *rlim) {
    if (!rlim) {
        errno = EFAULT;
        return -1;
    }

    switch (resource) {
    case RLIMIT_NOFILE:
        rlim->rlim_cur = 64;
        rlim->rlim_max = 1024;
        break;
    case RLIMIT_STACK:
        rlim->rlim_cur = 8 * 1024 * 1024;
        rlim->rlim_max = RLIM_INFINITY;
        break;
    case RLIMIT_AS:
        rlim->rlim_cur = 512 * 1024 * 1024;
        rlim->rlim_max = RLIM_INFINITY;
        break;
    default:
        rlim->rlim_cur = RLIM_INFINITY;
        rlim->rlim_max = RLIM_INFINITY;
        break;
    }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim) {
    (void)resource; (void)rlim;
    return 0;   /* stub: always succeed */
}