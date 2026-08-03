#ifndef AURALITE_LIBC_REGEX_H
#define AURALITE_LIBC_REGEX_H

#include <stddef.h>

/* cflags for regcomp() */
#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8

/* eflags for regexec() */
#define REG_NOTBOL   1
#define REG_NOTEOL   2

/* error codes */
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ESPACE  12

typedef struct {
    char  *re_comp;   /* compiled pattern (internal)   */
    size_t re_nsub;   /* number of parenthesised subexpressions */
} regex_t;

typedef long regoff_t;

typedef struct {
    regoff_t rm_so;   /* byte offset of match start    */
    regoff_t rm_eo;   /* byte offset just past the end */
} regmatch_t;

int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string, size_t nmatch,
               regmatch_t pmatch[], int eflags);
void   regfree(regex_t *preg);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);

#endif /* AURALITE_LIBC_REGEX_H */
