#ifndef AURALITE_LIBC_PWD_H
#define AURALITE_LIBC_PWD_H

#include "libc/include/sys/types.h"

struct passwd {
    char   *pw_name;    /* user name      */
    char   *pw_passwd;  /* password (unused; always "x")  */
    uid_t   pw_uid;     /* user ID        */
    gid_t   pw_gid;     /* group ID       */
    char   *pw_gecos;   /* real name      */
    char   *pw_dir;     /* home directory */
    char   *pw_shell;   /* login shell    */
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);

#endif /* AURALITE_LIBC_PWD_H */
