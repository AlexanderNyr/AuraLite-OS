#ifndef AURALITE_LIBC_GLOB_H
#define AURALITE_LIBC_GLOB_H

#include <stddef.h>

typedef struct {
    size_t   gl_pathc;   /* count of matched paths            */
    char   **gl_pathv;   /* NULL-terminated array of matches  */
    size_t   gl_offs;    /* reserved slots at the front       */
} glob_t;

#define GLOB_ERR      0x0001
#define GLOB_MARK     0x0002
#define GLOB_NOSORT   0x0004
#define GLOB_NOCHECK  0x0010
#define GLOB_APPEND   0x0020

#define GLOB_NOSPACE  1
#define GLOB_ABORTED  2
#define GLOB_NOMATCH  3

int  glob(const char *pattern, int flags,
          int (*errfunc)(const char *epath, int eerrno), glob_t *pglob);
void globfree(glob_t *pglob);

#endif /* AURALITE_LIBC_GLOB_H */
