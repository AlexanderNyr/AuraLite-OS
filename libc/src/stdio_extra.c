/* libc/src/stdio_extra.c — sprintf, sscanf, tmpfile (P10) */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(str, 4096, fmt, ap);   /* safe upper bound */
    va_end(ap);
    return n;
}

int vsprintf(char *str, const char *fmt, va_list ap) {
    return vsnprintf(str, 4096, fmt, ap);
}

FILE *tmpfile(void) {
    static int tmp_cnt = 0;
    char name[64];
    snprintf(name, sizeof(name), "/tmp/tmpfile_%d", tmp_cnt++);
    return fopen(name, "w+");
}

int remove(const char *path) {
    return unlink(path);
}