#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/types.h>

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9

#define RLIM_INFINITY (~0UL)

struct rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

#endif /* _SYS_RESOURCE_H */