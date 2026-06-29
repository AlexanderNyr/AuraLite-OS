/* libc/src/posix_extra.c — assorted POSIX library functions (P10)
 *
 * poll (via select), setlocale stub, wide-char (ASCII), semaphore (futex),
 * fnmatch + glob, inet_pton/ntop/aton/ntoa, getaddrinfo, getgr*.
 */

#include <poll.h>
#include <locale.h>
#include <wchar.h>
#include <semaphore.h>
#include <fnmatch.h>
#include <glob.h>
#include <grp.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ============================ poll (via select) ============================ */

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = -1;

    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (fds[i].events & POLLIN)  FD_SET(fds[i].fd, &rfds);
        if (fds[i].events & POLLOUT) FD_SET(fds[i].fd, &wfds);
        if (fds[i].fd > maxfd) maxfd = fds[i].fd;
    }

    struct timeval tv, *ptv = NULL;
    if (timeout >= 0) {
        tv.tv_sec  = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }

    int r = select(maxfd + 1, &rfds, &wfds, NULL, ptv);
    if (r < 0) return -1;

    int ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd < 0) continue;
        if ((fds[i].events & POLLIN)  && FD_ISSET(fds[i].fd, &rfds))
            fds[i].revents |= POLLIN;
        if ((fds[i].events & POLLOUT) && FD_ISSET(fds[i].fd, &wfds))
            fds[i].revents |= POLLOUT;
        if (fds[i].revents) ready++;
    }
    return ready;
}

/* ============================ locale (C only) ============================= */

char *setlocale(int category, const char *locale) {
    (void)category; (void)locale;
    return (char *)"C";
}

static struct lconv c_lconv = {
    .decimal_point = (char *)".",
    .thousands_sep = (char *)"",
    .currency_symbol = (char *)"",
};

struct lconv *localeconv(void) { return &c_lconv; }

/* ===================== wide characters (ASCII codepoints) ================= */

size_t wcslen(const wchar_t *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src) {
    wchar_t *d = dst; while ((*d++ = *src++)) {} return dst;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src) {
    wchar_t *d = dst + wcslen(dst);
    while ((*d++ = *src++)) {}
    return dst;
}

int wcscmp(const wchar_t *s1, const wchar_t *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (int)(*s1 - *s2);
}

int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return (int)(*s1 - *s2);
}

size_t mbstowcs(wchar_t *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && (dest == NULL || i < n); i++)
        if (dest) dest[i] = (unsigned char)src[i];
    if (dest && i < n) dest[i] = 0;
    return i;
}

size_t wcstombs(char *dest, const wchar_t *src, size_t n) {
    size_t i = 0;
    for (; src[i] && (dest == NULL || i < n); i++)
        if (dest) dest[i] = (char)(src[i] & 0xFF);
    if (dest && i < n) dest[i] = 0;
    return i;
}

wint_t btowc(int c) { return (c < 0 || c > 0x7F) ? WEOF : (wint_t)c; }
int    wctob(wint_t c) { return (c > 0x7F) ? EOF : (int)c; }

/* ========================= semaphores (futex) ============================= */

#define SYS_FUTEX 530

int sem_init(sem_t *sem, int pshared, unsigned int value) {
    (void)pshared;
    if (!sem) return -1;
    sem->value = (int)value;
    return 0;
}

int sem_destroy(sem_t *sem) { (void)sem; return 0; }

int sem_trywait(sem_t *sem) {
    if (!sem) return -1;
    int old = __atomic_load_n(&sem->value, __ATOMIC_SEQ_CST);
    while (old > 0) {
        if (__atomic_compare_exchange_n(&sem->value, &old, old - 1, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return 0;
    }
    return -1;   /* would block */
}

int sem_wait(sem_t *sem) {
    if (!sem) return -1;
    for (;;) {
        if (sem_trywait(sem) == 0) return 0;
        /* value == 0: sleep until a post bumps it. */
        syscall(SYS_FUTEX, (uint64_t)&sem->value, 0 /*FUTEX_WAIT*/, 0, 0, 0, 0);
    }
}

int sem_post(sem_t *sem) {
    if (!sem) return -1;
    __atomic_add_fetch(&sem->value, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_FUTEX, (uint64_t)&sem->value, 1 /*FUTEX_WAKE*/, 1, 0, 0, 0);
    return 0;
}

int sem_getvalue(sem_t *sem, int *sval) {
    if (!sem || !sval) return -1;
    *sval = __atomic_load_n(&sem->value, __ATOMIC_SEQ_CST);
    return 0;
}

/* ============================== fnmatch =================================== */

int fnmatch(const char *pattern, const char *string, int flags) {
    const char *p = pattern, *s = string;

    while (*p) {
        if (*p == '*') {
            p++;
            if (*p == '\0') {
                /* trailing '*' matches the rest, but not '/' under PATHNAME */
                if (flags & FNM_PATHNAME)
                    return strchr(s, '/') ? FNM_NOMATCH : 0;
                return 0;
            }
            for (; *s; s++) {
                if (fnmatch(p, s, flags) == 0) return 0;
                if ((flags & FNM_PATHNAME) && *s == '/') break;
            }
            return fnmatch(p, s, flags);
        } else if (*p == '?') {
            if (*s == '\0') return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
            p++; s++;
        } else if (*p == '[') {
            if (*s == '\0') return FNM_NOMATCH;
            const char *cls = p + 1;
            int negate = 0;
            if (*cls == '!' || *cls == '^') { negate = 1; cls++; }
            int matched = 0;
            while (*cls && *cls != ']') {
                if (cls[1] == '-' && cls[2] && cls[2] != ']') {
                    if ((unsigned char)*s >= (unsigned char)cls[0] &&
                        (unsigned char)*s <= (unsigned char)cls[2])
                        matched = 1;
                    cls += 3;
                } else {
                    if (*s == *cls) matched = 1;
                    cls++;
                }
            }
            if (*cls != ']') return FNM_NOMATCH;   /* malformed */
            if (matched == negate) return FNM_NOMATCH;
            p = cls + 1; s++;
        } else if (*p == '\\' && !(flags & FNM_NOESCAPE)) {
            p++;
            if (*p != *s) return FNM_NOMATCH;
            p++; s++;
        } else {
            if (*p != *s) return FNM_NOMATCH;
            p++; s++;
        }
    }
    return (*s == '\0') ? 0 : FNM_NOMATCH;
}

/* =============================== glob ===================================== */

static int glob_add(glob_t *g, const char *path) {
    size_t newc = g->gl_pathc + 1;
    char **v = realloc(g->gl_pathv, (newc + 1) * sizeof(char *));
    if (!v) return GLOB_NOSPACE;
    g->gl_pathv = v;
    g->gl_pathv[g->gl_pathc] = strdup(path);
    if (!g->gl_pathv[g->gl_pathc]) return GLOB_NOSPACE;
    g->gl_pathc = newc;
    g->gl_pathv[newc] = NULL;
    return 0;
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int), glob_t *pglob) {
    (void)errfunc;
    if (!pattern || !pglob) return GLOB_NOSPACE;

    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
        pglob->gl_offs  = 0;
    }

    /* Split into directory part and a basename pattern. */
    const char *slash = strrchr(pattern, '/');
    char dir[256];
    const char *pat;
    if (slash) {
        size_t dlen = (size_t)(slash - pattern);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, pattern, dlen);
        dir[dlen] = '\0';
        if (dir[0] == '\0') { dir[0] = '/'; dir[1] = '\0'; }
        pat = slash + 1;
    } else {
        dir[0] = '.'; dir[1] = '\0';
        pat = pattern;
    }

    DIR *d = opendir(dir);
    if (!d) {
        if (flags & GLOB_NOCHECK) { glob_add(pglob, pattern); return 0; }
        return GLOB_NOMATCH;
    }

    int found = 0;
    struct dirent *de;
    char full[512];
    while ((de = readdir(d)) != NULL) {
        if (fnmatch(pat, de->d_name, 0) == 0) {
            if (slash)
                snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
            else
                snprintf(full, sizeof(full), "%s", de->d_name);
            if (glob_add(pglob, full) != 0) { closedir(d); return GLOB_NOSPACE; }
            found++;
        }
    }
    closedir(d);

    if (!found) {
        if (flags & GLOB_NOCHECK) { glob_add(pglob, pattern); return 0; }
        return GLOB_NOMATCH;
    }
    return 0;
}

void globfree(glob_t *pglob) {
    if (!pglob || !pglob->gl_pathv) return;
    for (size_t i = 0; i < pglob->gl_pathc; i++)
        free(pglob->gl_pathv[i]);
    free(pglob->gl_pathv);
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}

/* =============================== inet ===================================== */

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET || !src || !dst) return -1;
    unsigned int a, b, c, d;
    if (sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    unsigned char *p = (unsigned char *)dst;
    p[0] = (unsigned char)a; p[1] = (unsigned char)b;
    p[2] = (unsigned char)c; p[3] = (unsigned char)d;
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET || !src || !dst) return NULL;
    const unsigned char *p = (const unsigned char *)src;
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    if (n < 0 || (socklen_t)n >= size) return NULL;
    memcpy(dst, tmp, (size_t)n + 1);
    return dst;
}

int inet_aton(const char *cp, struct in_addr *inp) {
    unsigned char buf[4];
    if (inet_pton(AF_INET, cp, buf) != 1) return 0;
    if (inp) inp->s_addr = ((uint32_t)buf[0]) | ((uint32_t)buf[1] << 8) |
                           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return 1;
}

in_addr_t inet_addr(const char *cp) {
    struct in_addr a;
    if (inet_aton(cp, &a)) return a.s_addr;
    return (in_addr_t)-1;
}

char *inet_ntoa(struct in_addr in) {
    static char buf[16];
    const unsigned char *p = (const unsigned char *)&in.s_addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

/* ============================ getaddrinfo ================================= */

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!node || !res) return EAI_NONAME;

    uint32_t ip = 0;
    unsigned char b[4];
    if (inet_pton(AF_INET, node, b) == 1) {
        ip = ((uint32_t)b[0]) | ((uint32_t)b[1] << 8) |
             ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    } else {
        ip = dns_resolve(node);   /* network-byte-order IPv4, 0 on failure */
        if (ip == 0) return EAI_NONAME;
    }

    struct addrinfo    *ai = malloc(sizeof(*ai));
    struct sockaddr_in *sa = malloc(sizeof(*sa));
    if (!ai || !sa) { free(ai); free(sa); return EAI_MEMORY; }

    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = ip;
    sa->sin_port = service ? htons((uint16_t)atoi(service)) : 0;

    memset(ai, 0, sizeof(*ai));
    ai->ai_family   = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen  = sizeof(*sa);
    ai->ai_addr     = (struct sockaddr *)sa;
    ai->ai_next     = NULL;

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *n = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = n;
    }
}

const char *gai_strerror(int errcode) {
    switch (errcode) {
    case 0:          return "Success";
    case EAI_NONAME: return "Name or service not known";
    case EAI_FAIL:   return "Non-recoverable failure in name resolution";
    case EAI_MEMORY: return "Memory allocation failure";
    default:         return "Unknown error";
    }
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent he;
    static uint32_t addr;
    static char    *addr_list[2];
    static char    *aliases[1] = { NULL };

    unsigned char b[4];
    if (inet_pton(AF_INET, name, b) == 1)
        addr = ((uint32_t)b[0]) | ((uint32_t)b[1] << 8) |
               ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    else {
        addr = dns_resolve(name);
        if (addr == 0) return NULL;
    }

    addr_list[0]   = (char *)&addr;
    addr_list[1]   = NULL;
    he.h_name      = (char *)name;
    he.h_aliases   = aliases;
    he.h_addrtype  = AF_INET;
    he.h_length    = 4;
    he.h_addr_list = addr_list;
    return &he;
}

/* =============================== grp ===================================== */

static struct group root_group = {
    .gr_name   = (char *)"root",
    .gr_passwd = (char *)"x",
    .gr_gid    = 0,
    .gr_mem    = NULL,
};

struct group *getgrgid(gid_t gid) {
    return gid == 0 ? &root_group : NULL;
}

struct group *getgrnam(const char *name) {
    return (name && strcmp(name, "root") == 0) ? &root_group : NULL;
}
