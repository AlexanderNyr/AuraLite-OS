#ifndef AURALITE_LIBC_GRP_H
#define AURALITE_LIBC_GRP_H

#include "libc/include/sys/types.h"

struct group {
    char   *gr_name;    /* group name              */
    char   *gr_passwd;  /* password (unused)       */
    gid_t   gr_gid;     /* group ID                */
    char  **gr_mem;     /* NULL-terminated members */
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);

#endif /* AURALITE_LIBC_GRP_H */
