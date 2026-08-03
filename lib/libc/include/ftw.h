#ifndef _FTW_H
#define _FTW_H

#include <sys/stat.h>

#define FTW_F   0
#define FTW_D   1
#define FTW_DNR 2
#define FTW_SL  3
#define FTW_NS  4

#define FTW_PHYS  1
#define FTW_MOUNT 2
#define FTW_DEPTH 4

struct FTW {
    int base;
    int level;
};

int nftw(const char *dir, int (*fn)(const char *, const struct stat *, int, struct FTW *),
         int depth, int flags);
int ftw(const char *dir, int (*fn)(const char *, const struct stat *, int), int depth);

#endif /* _FTW_H */
