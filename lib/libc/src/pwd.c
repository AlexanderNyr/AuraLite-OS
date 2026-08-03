/* libc/src/pwd.c — getpwuid / getpwnam (stub) */

#include <pwd.h>
#include <string.h>

static struct passwd root_pw = {
    .pw_name = "root",
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_dir = "/root",
    .pw_shell = "/bin/sh"
};

struct passwd *getpwuid(uid_t uid) {
    if (uid == 0) return &root_pw;
    return NULL;
}

struct passwd *getpwnam(const char *name) {
    if (strcmp(name, "root") == 0) return &root_pw;
    return NULL;
}