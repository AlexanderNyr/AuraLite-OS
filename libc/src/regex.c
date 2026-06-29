/* libc/src/regex.c — minimal POSIX regex (P10)
 *
 * This is a substring-matching stub: regcomp() stores the pattern verbatim and
 * regexec() succeeds when the pattern occurs anywhere in the subject string.
 * It is enough for the common "does this contain X" usage; full ERE support is
 * a future enhancement (tracked in TODO.md).
 */

#include "libc/include/regex.h"
#include "libc/include/string.h"
#include "libc/include/stdlib.h"

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    (void)cflags;
    if (!preg || !pattern) return REG_BADPAT;
    preg->re_nsub = 0;
    preg->re_comp = strdup(pattern);
    return preg->re_comp ? 0 : REG_ESPACE;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
    (void)eflags;

    if (!preg || !preg->re_comp || !string) return REG_NOMATCH;

    char *hit = strstr(string, preg->re_comp);
    if (!hit) return REG_NOMATCH;

    if (nmatch > 0 && pmatch) {
        pmatch[0].rm_so = (regoff_t)(hit - string);
        pmatch[0].rm_eo = (regoff_t)(hit - string + (regoff_t)strlen(preg->re_comp));
        for (size_t i = 1; i < nmatch; i++) {
            pmatch[i].rm_so = -1;
            pmatch[i].rm_eo = -1;
        }
    }
    return 0;
}

void regfree(regex_t *preg) {
    if (preg && preg->re_comp) {
        free(preg->re_comp);
        preg->re_comp = NULL;
    }
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    (void)preg;
    const char *msg;
    switch (errcode) {
    case REG_NOMATCH: msg = "No match";              break;
    case REG_BADPAT:  msg = "Invalid regex pattern"; break;
    case REG_ESPACE:  msg = "Out of memory";         break;
    default:          msg = "Regex error";           break;
    }
    if (errbuf && errbuf_size > 0) {
        strncpy(errbuf, msg, errbuf_size - 1);
        errbuf[errbuf_size - 1] = '\0';
    }
    return strlen(msg) + 1;
}
