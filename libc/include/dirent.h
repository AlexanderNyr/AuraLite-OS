#ifndef AURALITE_LIBC_DIRENT_H
#define AURALITE_LIBC_DIRENT_H

#include "sys/types.h"

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12

struct dirent {
    ino_t          d_ino;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct __dirstream DIR;

DIR           *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int            closedir(DIR *dirp);
void           rewinddir(DIR *dirp);

int  dirfd(DIR *dirp);
int  scandir(const char *dir, struct dirent ***namelist,
             int (*sel)(const struct dirent *),
             int (*compar)(const struct dirent **, const struct dirent **));
int  alphasort(const struct dirent **a, const struct dirent **b);
int  versionsort(const struct dirent **a, const struct dirent **b);

#endif /* AURALITE_LIBC_DIRENT_H */
