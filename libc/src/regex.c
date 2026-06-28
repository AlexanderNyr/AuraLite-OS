/* libc/src/regex.c — минимальная реализация POSIX regex (P10) */

#include <regex.h>
#include <string.h>
#include <stdlib.h>

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    (void)cflags;
    preg->re_nsub = 0;
    preg->re_comp = strdup(pattern);
    return preg->re_comp ? 0 : REG_ESPACE;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
    (void)eflags; (void)nmatch; (void)pmatch;

    if (!preg || !preg->re_comp || !string) return REG_NOMATCH;

    /* Очень простая реализация: ищем подстроку */
    if (strstr(string, preg->re_comp))
        return 0;
    return REG_NOMATCH;
}

void regfree(regex_t *preg) {
    if (preg && preg->re_comp) {
        free(preg->re_comp);
        preg->re_comp = NULL;
    }
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    (void)preg;
    const char *msg = "Regex error";
    if (errbuf && errbuf_size > 0) {
        strncpy(errbuf, msg, errbuf_size - 1);
        errbuf[errbuf_size - 1] = 0;
    }
    return strlen(msg);
}