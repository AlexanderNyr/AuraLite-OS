/* libc/src/utsname.c — uname (P10) */

#include <sys/utsname.h>
#include <string.h>

/* OTA_PLAN O2: the release field is the build's version identity; the
 * Makefile passes -DAURALITE_VERSION for normal builds (see the Makefile's
 * AURALITE_VERSION block), this fallback covers compiles without it. */
#ifndef AURALITE_VERSION
#define AURALITE_VERSION "0.0.1"
#endif

int uname(struct utsname *buf) {
    if (!buf) return -1;

    strcpy(buf->sysname, "AuraLite");
    strcpy(buf->nodename, "auralite");
    strcpy(buf->release, AURALITE_VERSION);
    strcpy(buf->version, "POSIX P10");
    strcpy(buf->machine, "x86_64");
    return 0;
}