/* libc/src/env.c — environment variables (P10) */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char **environ = NULL;   /* set by crt0 or execve */

char *getenv(const char *name) {
    if (!environ || !name) return NULL;
    size_t len = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=') {
            return *e + len + 1;
        }
    }
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value) return -1;
    /* Simplified: we don't realloc environ here */
    (void)overwrite;
    return 0;
}

int unsetenv(const char *name) {
    (void)name;
    return 0;
}