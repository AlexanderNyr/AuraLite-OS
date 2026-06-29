#ifndef AURALITE_LIBC_SYS_UTSNAME_H
#define AURALITE_LIBC_SYS_UTSNAME_H

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

int uname(struct utsname *buf);

#endif /* AURALITE_LIBC_SYS_UTSNAME_H */
