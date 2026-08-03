#ifndef _WORDEXP_H
#define _WORDEXP_H

#include <stddef.h>

typedef struct {
    size_t we_wordc;
    char **we_wordv;
    size_t we_offs;
} wordexp_t;

#define WRDE_DOOFFS  1
#define WRDE_APPEND  2
#define WRDE_NOCMD   4
#define WRDE_REUSE   8
#define WRDE_SHOWERR 16
#define WRDE_UNDEF   32

#define WRDE_SUCCESS   0
#define WRDE_NOSPACE   1
#define WRDE_BADCHAR   2
#define WRDE_BADVAL    3
#define WRDE_CMDSUB    4
#define WRDE_NOSYS     5

int wordexp(const char *words, wordexp_t *pwordexp, int flags);
void wordfree(wordexp_t *pwordexp);

#endif /* _WORDEXP_H */
