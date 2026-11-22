#ifndef AURALITE_LIBC_PWD_H
#define AURALITE_LIBC_PWD_H

#include "sys/types.h"
#include <stddef.h>

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

/* HOME_PLAN H0: enumeration and the thread-safe spelling.  The sequential
 * set is what `ls ~` and a future `adduser` diagnostics tool need; before
 * H0 these names did not exist at all. */
struct passwd *getpwent(void);
void           setpwent(void);
void           endpwent(void);
int            getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
                          size_t buflen, struct passwd **result);

#endif /* AURALITE_LIBC_PWD_H */
