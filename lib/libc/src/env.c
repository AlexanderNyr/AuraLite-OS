/* libc/src/env.c — environment variables (P10)
 *
 * `environ` is set by crt0/__libc_start_main from the process's initial
 * environment.  setenv()/unsetenv()/putenv() maintain a mutable copy in a
 * static table; once we take ownership we never write back into the original
 * argv/envp area on the stack.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ENV_MAX 128

extern char **environ;
char **environ = NULL;

/* Mutable backing store.  env_owned == 1 once we've migrated `environ` here. */
static char  *env_store[ENV_MAX + 1];
static int    env_owned = 0;

static int env_len(void) {
    int n = 0;
    if (environ) while (environ[n]) n++;
    return n;
}

/* Migrate the inherited environ[] into our own writable table so we can add,
 * replace and remove entries without touching the initial stack image. */
static void env_take_ownership(void) {
    if (env_owned) return;
    int n = 0;
    if (environ) {
        for (; environ[n] && n < ENV_MAX; n++) {
            env_store[n] = strdup(environ[n]);
            if (!env_store[n]) break;
        }
    }
    env_store[n] = NULL;
    environ = env_store;
    env_owned = 1;
}

char *getenv(const char *name) {
    if (!environ || !name) return NULL;
    size_t len = strlen(name);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=')
            return *e + len + 1;
    }
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || strchr(name, '=')) return -1;
    env_take_ownership();

    size_t nlen = strlen(name);

    /* Replace existing? */
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            if (!overwrite) return 0;
            char *entry = malloc(nlen + 1 + strlen(value) + 1);
            if (!entry) return -1;
            strcpy(entry, name);
            entry[nlen] = '=';
            strcpy(entry + nlen + 1, value);
            free(environ[i]);
            environ[i] = entry;
            return 0;
        }
    }

    /* Append a new entry. */
    int n = env_len();
    if (n >= ENV_MAX) return -1;
    char *entry = malloc(nlen + 1 + strlen(value) + 1);
    if (!entry) return -1;
    strcpy(entry, name);
    entry[nlen] = '=';
    strcpy(entry + nlen + 1, value);
    env_store[n]     = entry;
    env_store[n + 1] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || strchr(name, '=')) return -1;
    if (!environ) return 0;
    env_take_ownership();

    size_t nlen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            free(environ[i]);
            /* shift the rest down (including the NULL terminator). */
            int j = i;
            do { environ[j] = environ[j + 1]; j++; } while (environ[j - 1]);
            i--;
        }
    }
    return 0;
}

int putenv(char *string) {
    if (!string) return -1;
    char *eq = strchr(string, '=');
    if (!eq) return unsetenv(string);

    env_take_ownership();
    size_t nlen = (size_t)(eq - string);

    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], string, nlen) == 0 && environ[i][nlen] == '=') {
            environ[i] = string;   /* putenv: caller owns the storage */
            return 0;
        }
    }
    int n = env_len();
    if (n >= ENV_MAX) return -1;
    env_store[n]     = string;
    env_store[n + 1] = NULL;
    return 0;
}
