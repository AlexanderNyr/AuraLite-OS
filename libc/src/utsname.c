/* libc/src/utsname.c — uname (P10) */

#include <sys/utsname.h>
#include <string.h>

int uname(struct utsname *buf) {
    if (!buf) return -1;

    strcpy(buf->sysname, "AuraLite");
    strcpy(buf->nodename, "auralite");
    strcpy(buf->release, "1.0.0");
    strcpy(buf->version, "POSIX P10");
    strcpy(buf->machine, "x86_64");
    return 0;
}