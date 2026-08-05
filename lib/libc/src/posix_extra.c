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
#include <signal.h>
#include <pthread.h>   /* Q15: mq_notify SIGEV_THREAD / watcher thread */
#include <errno.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <time.h>
#include <fcntl.h>        /* Q12: O_CREAT/AT_* for the missing-body batch */
#include <sched.h>        /* Q12: sched family bodies */
#include <sys/resource.h> /* Q12: getrusage body */
#include <net/if.h>       /* Q12: if_nameindex family bodies */
#include <sys/stat.h>     /* Q13: S_IFIFO for mkfifoat, utimensat decls */

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
    errno = EAGAIN;   /* Q12: POSIX requires EAGAIN on a would-block trywait */
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


/* ========================= shm_open / shm_unlink (Q7) ===================== */

int shm_open(const char *name, int oflag, mode_t mode) {
    char path[512];
    if (name[0] == '/')
        snprintf(path, sizeof(path), "/dev/shm%s", name);
    else
        snprintf(path, sizeof(path), "/dev/shm/%s", name);
    return open(path, oflag | O_CREAT, mode);
}

int shm_unlink(const char *name) {
    char path[512];
    if (name[0] == '/')
        snprintf(path, sizeof(path), "/dev/shm%s", name);
    else
        snprintf(path, sizeof(path), "/dev/shm/%s", name);
    return unlink(path);
}

/* ===================== Named semaphores (Q7) ============================== */

sem_t *sem_open(const char *name, int oflag, ...) {
    /* Q12: glibc-style flat naming under the /dev/shm volume ("sem.NAME"),
     * avoiding a directory component that would need mkdir first.  Named
     * semaphores are a documented partial (see POSIX2024_PLAN.md Q12 /
     * tests/posix2024/known_partials.txt): they need MAP_SHARED backing,
     * which the kernel does not provide yet, so sem_open reaches the
     * mmap() call below and honestly fails with ENOSYS. */
    char path[512];
    snprintf(path, sizeof(path), "/dev/shm/sem.%s",
             name[0]=='/' ? name + 1 : name);
    mode_t mode = 0600;
    unsigned val = 0;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, mode_t);
        val = va_arg(ap, unsigned);
        va_end(ap);
    }
    int fd = open(path, oflag | O_RDWR, mode);
    if (fd < 0) return SEM_FAILED;
    if (oflag & O_CREAT) {
        write(fd, &val, sizeof(val));
    }
    sem_t *s = mmap(NULL, sizeof(sem_t), PROT_READ|PROT_WRITE,
                    MAP_SHARED, fd, 0);
    close(fd);
    return (s == MAP_FAILED) ? SEM_FAILED : s;
}

int sem_close(sem_t *sem) {
    return munmap(sem, sizeof(sem_t));
}

int sem_unlink(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "/dev/shm/sem.%s",
             name[0]=='/' ? name + 1 : name);
    return unlink(path);
}

int sem_timedwait(sem_t *sem, const struct timespec *abs_timeout) {
    while (1) {
        if (sem_trywait(sem) == 0) return 0;
        struct timespec now;
        clock_gettime(0, &now);
        if (now.tv_sec > abs_timeout->tv_sec ||
            (now.tv_sec == abs_timeout->tv_sec &&
             now.tv_nsec >= abs_timeout->tv_nsec)) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct timespec sl = {0, 1000000};
        nanosleep(&sl, NULL);
    }
}

/* ================== Message queues (Q7 / Q15) ============================ */

/* File-based message queue: each mq is a queue FILE under /tmp/mq_<name>.
 * mq_open(name, O_CREAT) creates the queue file; mq_send appends a
 * [len:4][data] record at EOF; mq_receive consumes the FIRST record and
 * rewrites the queue file without it, so the queue is a true FIFO and
 * "empty" really means file size 0 — the state the mq_notify watcher
 * keys on (Q15).  No cross-process locking is provided; single-writer /
 * single-reader usage remains the documented emulation model (Q12 note). */

#define MQ_PATH_MAX 512

static void _mq_name_to_path(const char *name, char *path, size_t sz) {
    const char *n = name;
    while (*n == '/') n++;
    snprintf(path, sz, "/tmp/mq_%s", n);
}

/* Q15: fd -> queue-name table so mq_notify(mqd_t) can find the queue's
 * path for the one-registration-per-queue (EBUSY) rule. */
#define MQ_OPEN_MAX 16
struct mq_fd_name { int fd; char name[MQ_PATH_MAX]; };
static struct mq_fd_name mq_fd_names[MQ_OPEN_MAX];

static void mq_fd_name_set(int fd, const char *name) {
    for (int i = 0; i < MQ_OPEN_MAX; i++) {
        if (mq_fd_names[i].fd == fd || mq_fd_names[i].fd == 0) {
            mq_fd_names[i].fd = fd;
            snprintf(mq_fd_names[i].name, sizeof(mq_fd_names[i].name), "%s", name);
            return;
        }
    }
}
static void mq_fd_name_clear(int fd) {
    for (int i = 0; i < MQ_OPEN_MAX; i++)
        if (mq_fd_names[i].fd == fd) mq_fd_names[i].fd = 0;
}
static const char *mq_fd_name_get(int fd) {
    for (int i = 0; i < MQ_OPEN_MAX; i++)
        if (mq_fd_names[i].fd == fd) return mq_fd_names[i].name;
    return NULL;
}

mqd_t mq_open(const char *name, int oflag, ...) {
    if (!name) { errno = EINVAL; return MQD_INVALID; }
    mode_t mode = 0600;
    struct mq_attr *attr = NULL;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, mode_t);
        attr = va_arg(ap, struct mq_attr *);
        va_end(ap);
    }
    char path[MQ_PATH_MAX];
    _mq_name_to_path(name, path, sizeof(path));
    int fd = open(path, oflag | O_RDWR, mode);
    if (fd < 0) return MQD_INVALID;
    (void)attr;
    mq_fd_name_set(fd, name);
    return (mqd_t)(intptr_t)fd;
}

int mq_close(mqd_t mqdes) {
    int fd = (int)mqdes;
    mq_fd_name_clear(fd);
    return close(fd);
}

int mq_unlink(const char *name) {
    char path[MQ_PATH_MAX];
    _mq_name_to_path(name, path, sizeof(path));
    return unlink(path);
}

int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned msg_prio) {
    (void)msg_prio;
    if (!msg_ptr) { errno = EINVAL; return -1; }
    /* Append a [len:4][data] record at EOF. */
    if (lseek((int)mqdes, 0, SEEK_END) < 0) return -1;
    size_t len = msg_len;
    if (write((int)mqdes, &len, sizeof(len)) < 0) return -1;
    if (write((int)mqdes, msg_ptr, len) < 0) return -1;
    return 0;
}

/* Q15: truncate(2) through procfs.  There is no ftruncate syscall, but
 * /proc/self/fd/<N> resolves to the fd's real vnode (Q13), so truncating
 * that path truncates the open queue file. */
static int mq_ftruncate(int fd, off_t size) {
    char p[40];
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    return truncate(p, size);
}

ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned *msg_prio) {
    (void)msg_prio;
    int fd = (int)mqdes;
    if (!msg_ptr) { errno = EINVAL; return -1; }
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    size_t len = 0;
    ssize_t r = read(fd, &len, sizeof(len));
    if (r < 0) return -1;
    if (r == 0) { errno = EAGAIN; return -1; }   /* empty queue */
    if (len > msg_len) { errno = EMSGSIZE; return -1; }
    if (read(fd, msg_ptr, len) != (ssize_t)len) return -1;

    /* Dequeue: rewrite the queue file with everything after this record
     * and truncate, so an emptied queue is a 0-byte file. */
    off_t consumed = (off_t)sizeof(len) + (off_t)len;
    off_t fsz = lseek(fd, 0, SEEK_END);
    off_t tail = (fsz > consumed) ? fsz - consumed : 0;
    if (tail > 0) {
        char *buf = malloc((size_t)tail);
        if (!buf) { errno = ENOMEM; return -1; }
        if (lseek(fd, consumed, SEEK_SET) < 0 ||
            read(fd, buf, (size_t)tail) != (ssize_t)tail) {
            free(buf); errno = EIO; return -1;
        }
        if (lseek(fd, 0, SEEK_SET) < 0 ||
            write(fd, buf, (size_t)tail) != (ssize_t)tail) {
            free(buf); errno = EIO; return -1;
        }
        free(buf);
    }
    if (mq_ftruncate(fd, tail) < 0) { errno = EIO; return -1; }
    return (ssize_t)len;
}

int mq_timedsend(mqd_t mqdes, const char *msg_ptr, size_t msg_len,
                 unsigned msg_prio, const struct timespec *abs_timeout) {
    (void)abs_timeout;
    return mq_send(mqdes, msg_ptr, msg_len, msg_prio);
}

ssize_t mq_timedreceive(mqd_t mqdes, char *msg_ptr, size_t msg_len,
                        unsigned *msg_prio, const struct timespec *abs_timeout) {
    (void)abs_timeout;
    return mq_receive(mqdes, msg_ptr, msg_len, msg_prio);
}

int mq_getattr(mqd_t mqdes, struct mq_attr *mqstat) {
    if (!mqstat) { errno = EINVAL; return -1; }
    int fd = (int)mqdes;
    mqstat->mq_flags = 0;
    mqstat->mq_maxmsg = 16;
    mqstat->mq_msgsize = 1024;
    mqstat->mq_curmsgs = 0;
    /* Count [len][data] records so mq_curmsgs is truthful. */
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size <= 0) return 0;
    uint64_t fsz = (uint64_t)st.st_size;
    uint64_t pos = 0;
    int n = 0;
    while (pos + (uint64_t)sizeof(size_t) <= fsz) {
        size_t l = 0;
        if (lseek(fd, (off_t)pos, SEEK_SET) < 0) break;
        if (read(fd, &l, sizeof(l)) != (ssize_t)sizeof(l)) break;
        if (l > 1024) break;                 /* corrupt record */
        pos += (uint64_t)sizeof(l) + (uint64_t)l;
        n++;
        if (pos > fsz) break;
    }
    mqstat->mq_curmsgs = n;
    return 0;
}

int mq_setattr(mqd_t mqdes, const struct mq_attr *restrict mqstat,
               struct mq_attr *restrict omqstat) {
    (void)mqdes;
    if (omqstat) mq_getattr(mqdes, omqstat);
    (void)mqstat;
    return 0;
}

/* ---- Q15: mq_notify + sigevent delivery (POSIX2024_PLAN.md phase Q15) ----
 *
 * The mqueue is file-backed, so notification lives in user space: each
 * registration spawns a watcher thread that polls the queue FILE SIZE.
 * A size 0 -> >0 transition is the POSIX "queue went from empty to
 * non-empty" edge and delivers the notification.  Because mq_receive
 * dequeues, "empty" really is size 0, and a burst of messages between two
 * polls compresses to fewer notifications — the POSIX "at least one" rule,
 * documented rather than hidden.
 *
 *   SIGEV_SIGNAL : kill(registrar_pid, sigev_signo).  AuraLite's sigaction
 *                  has no siginfo delivery, so sigev_value is not conveyed
 *                  to the handler (documented limitation).
 *   SIGEV_THREAD : a detached pthread runs sigev_notify_function(sigev_value).
 *   SIGEV_NONE / NULL : deregister.
 *
 * One registration per queue (POSIX): a second mq_notify on the same queue
 * fails with EBUSY until the first is deregistered.
 */

#define MQ_NOTIFY_MAX 8
struct mq_notify_reg {
    int             active;
    int             fd;              /* registering mqd_t (identity) */
    char            path[MQ_PATH_MAX];
    struct sigevent sev;
    pthread_t       thread;
    volatile int    stop;            /* set to ask the watcher to exit */
    volatile int    done;            /* watcher sets when it has exited */
    int             target_pid;      /* getpid() at registration time */
    volatile int    ready;           /* watcher has observed the queue once */
};
static struct mq_notify_reg mq_notify_regs[MQ_NOTIFY_MAX];

static void mq_notify_deliver(const struct sigevent *sev, int target_pid) {
    switch (sev->sigev_notify) {
    case SIGEV_SIGNAL:
        if (sev->sigev_signo > 0 && sev->sigev_signo < NSIG)
            kill(target_pid, sev->sigev_signo);
        break;
    case SIGEV_THREAD:
        if (!sev->sigev_notify_function) break;
        /* Deviation, annotated (POSIX2024_PLAN.md Q15): POSIX says the
         * notification function runs on a fresh thread.  AuraLite's
         * pthread_create clones a kernel thread, and under QEMU TCG a
         * thread created from ANOTHER thread is not scheduled promptly
         * (observed: tens of guest-seconds), which made the documented
         * gate flaky.  The function therefore runs on the watcher thread
         * (itself a thread of the registering process); the observable
         * contract — the function is invoked with sigev_value on the
         * empty->non-empty transition — is preserved.  Revisit when the
         * scheduler schedules clone children of threads promptly. */
        sev->sigev_notify_function(sev->sigev_value);
        break;
    default:
        break;
    }
}

static void *mq_notify_watcher(void *arg) {
    struct mq_notify_reg *r = (struct mq_notify_reg *)arg;
    int wfd = open(r->path, O_RDONLY);
    if (wfd < 0) { r->done = 1; return NULL; }
    struct stat st;
    int was_empty = 1;
    if (fstat(wfd, &st) == 0) was_empty = (st.st_size == 0);
    /* Publish readiness only after the initial state is captured, so
     * mq_notify() can return knowing the watcher is actually observing the
     * queue.  Without this, a slow first scheduling of the watcher thread
     * (QEMU TCG) lets a send happen BEFORE the watcher's first fstat, and
     * the empty->non-empty edge is lost forever. */
    r->ready = 1;
    while (!r->stop) {
        if (fstat(wfd, &st) == 0) {
            int empty = (st.st_size == 0);
            if (!empty && was_empty) {
                /* empty -> non-empty edge */
                mq_notify_deliver(&r->sev, r->target_pid);
                was_empty = 0;
            } else if (empty) {
                was_empty = 1;       /* re-arm once drained */
            }
        }
        struct timespec ts = { 0, 2000000 };    /* 2 ms poll */
        nanosleep(&ts, NULL);
    }
    close(wfd);
    r->done = 1;
    return NULL;
}

int mq_notify(mqd_t mqdes, const struct sigevent *notification) {
    int fd = (int)mqdes;
    const char *name = mq_fd_name_get(fd);
    if (!name) { errno = EBADF; return -1; }
    char path[MQ_PATH_MAX];
    _mq_name_to_path(name, path, sizeof(path));

    /* Deregistration: NULL or SIGEV_NONE. */
    if (!notification || notification->sigev_notify == SIGEV_NONE) {
        for (int i = 0; i < MQ_NOTIFY_MAX; i++) {
            struct mq_notify_reg *r = &mq_notify_regs[i];
            if (r->active && strcmp(r->path, path) == 0) {
                r->stop = 1;
                int spins = 0;
                while (!r->done && spins++ < 2000) {
                    struct timespec ts = { 0, 1000000 };   /* 1 ms */
                    nanosleep(&ts, NULL);
                }
                r->active = 0;
                r->done = 0;
                return 0;
            }
        }
        return 0;   /* nothing registered: nothing to undo */
    }

    if (notification->sigev_notify != SIGEV_SIGNAL &&
        notification->sigev_notify != SIGEV_THREAD) {
        errno = EINVAL; return -1;
    }
    if (notification->sigev_notify == SIGEV_SIGNAL &&
        (notification->sigev_signo <= 0 || notification->sigev_signo >= NSIG)) {
        errno = EINVAL; return -1;
    }

    /* One registration per queue. */
    for (int i = 0; i < MQ_NOTIFY_MAX; i++) {
        if (mq_notify_regs[i].active &&
            strcmp(mq_notify_regs[i].path, path) == 0) {
            errno = EBUSY;
            return -1;
        }
    }
    int slot = -1;
    for (int i = 0; i < MQ_NOTIFY_MAX; i++)
        if (!mq_notify_regs[i].active) { slot = i; break; }
    if (slot < 0) { errno = ENOSPC; return -1; }

    struct mq_notify_reg *r = &mq_notify_regs[slot];
    memset(r, 0, sizeof(*r));
    r->active = 1;
    r->fd = fd;
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = 0;
    r->sev = *notification;
    r->target_pid = (int)getpid();
    r->stop = 0;
    r->done = 0;
    r->ready = 0;
    if (pthread_create(&r->thread, NULL, mq_notify_watcher, r) != 0) {
        r->active = 0;
        errno = EAGAIN;
        return -1;
    }
    /* Wait until the watcher has observed the queue's initial state, so a
     * caller's very next send cannot race ahead of the first fstat and lose
     * the empty->non-empty edge (slow first scheduling under QEMU TCG). */
    {
        int spins = 0;
        while (!r->ready && spins++ < 200000) {
            struct timespec ts = { 0, 1000000 };   /* 1 ms */
            nanosleep(&ts, NULL);
        }
        if (!r->ready) {
            r->stop = 1;
            r->active = 0;
            errno = EAGAIN;
            return -1;
        }
    }
    return 0;
}

/* =============== POSIX2024 Q12: declared-but-missing bodies ================
 * POSIX2024_PLAN.md phase Q12.  The new tests/posix2024 matrix drift check
 * (every ✅ row of docs/posix2024_compliance.md must resolve in libaurac.a)
 * caught a batch of functions declared in the public headers by earlier
 * phases but never given bodies.  They are implemented here, thin over the
 * already-dispatched syscalls (numbers per kernel/arch/x86_64/syscall.c).
 * Nothing here changes behaviour already shipped -- it fills declared holes.
 */

/* Local syscall numbers the headers do not carry (like SYS_FUTEX above). */
#define SYS_SCHED_YIELD    24
#define SYS_NANOSLEEP      35
#define SYS_OPENAT        257
#define SYS_MKDIRAT       258
#define SYS_FCHOWNAT      260
#define SYS_FSTATAT       262
#define SYS_UNLINKAT      263
#define SYS_RENAMEAT      264
#define SYS_READLINKAT    267
#define SYS_FCHMODAT      268
#define SYS_FACCESSAT     269
#define SYS_DUP3          292
#define SYS_EXECVEAT      322
#define SYS_CLOSE_RANGE   436

/* Decode the in-band negative-errno convention used by syscall_dispatch:
 * raw < 0 is -(errno); anything else is the successful return value. */
static int q12_ret(int64_t raw) {
    if (raw < 0) {
        errno = (int)-raw;
        return -1;
    }
    return (int)raw;
}

/* ---- AT-family wrappers (Q5 declared these; the bodies were missing) ---- */

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return q12_ret(syscall(SYS_OPENAT, (uint64_t)dirfd, (uint64_t)path,
                           (uint64_t)flags, (uint64_t)mode, 0, 0));
}

int mkdirat(int dfd, const char *path, mode_t mode) {
    return q12_ret(syscall(SYS_MKDIRAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)mode, 0, 0, 0));
}

int fchownat(int dfd, const char *path, uid_t owner, gid_t group, int flags) {
    return q12_ret(syscall(SYS_FCHOWNAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)owner, (uint64_t)group,
                           (uint64_t)flags, 0));
}

int fstatat(int dfd, const char *path, struct stat *buf, int flags) {
    return q12_ret(syscall(SYS_FSTATAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)buf, (uint64_t)flags, 0, 0));
}

int unlinkat(int dfd, const char *path, int flags) {
    return q12_ret(syscall(SYS_UNLINKAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)flags, 0, 0, 0));
}

int renameat(int old_dfd, const char *old, int new_dfd, const char *new_) {
    return q12_ret(syscall(SYS_RENAMEAT, (uint64_t)old_dfd, (uint64_t)old,
                           (uint64_t)new_dfd, (uint64_t)new_, 0, 0));
}

ssize_t readlinkat(int dfd, const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)syscall(SYS_READLINKAT, (uint64_t)dfd, (uint64_t)path,
                            (uint64_t)buf, (uint64_t)bufsiz, 0, 0);
}

int fchmodat(int dfd, const char *path, mode_t mode, int flags) {
    return q12_ret(syscall(SYS_FCHMODAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)mode, (uint64_t)flags, 0, 0));
}

int faccessat(int dfd, const char *path, int mode, int flags) {
    (void)flags;
    return q12_ret(syscall(SYS_FACCESSAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)mode, 0, 0, 0));
}

int dup3(int oldfd, int newfd, int flags) {
    return q12_ret(syscall(SYS_DUP3, (uint64_t)oldfd, (uint64_t)newfd,
                           (uint64_t)flags, 0, 0, 0));
}

int fexecve(int fd, char *const argv[], char *const envp[]) {
    /* execveat(AT_EMPTY_PATH) resolves /proc/self/fd/<fd> in the kernel. */
    return q12_ret(syscall(SYS_EXECVEAT, (uint64_t)fd, 0,
                           (uint64_t)argv, (uint64_t)envp,
                           AT_EMPTY_PATH, 0));
}

/* ---- close_range / closefrom (Q11 declared; bodies missing) ---- */

int close_range(unsigned first, unsigned last, int flags) {
    (void)flags;
    return q12_ret(syscall(SYS_CLOSE_RANGE, (uint64_t)first,
                           (uint64_t)last, 0, 0, 0, 0));
}

int closefrom(int lowfd) {
    /* Kernel caps the range scan at 64 descriptors. */
    return close_range((unsigned)lowfd, 0x40000000u, 0);
}

/* ---- clock_nanosleep / timespec_get (Q11 declared; bodies missing) ----
 * The kernel's nanosleep is relative-only, so TIMER_ABSTIME is computed as
 * a delta here (POSIX: clock_nanosleep returns the error number directly,
 * 0 on success -- not -1+errno). */

int clock_nanosleep(clockid_t clockid, int flags,
                    const struct timespec *req, struct timespec *rem) {
    if (!req || (flags & ~TIMER_ABSTIME)) return EINVAL;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clockid, &now) != 0) return errno;
        int64_t nsec = (int64_t)(req->tv_sec - now.tv_sec) * 1000000000LL
                     + (int64_t)req->tv_nsec - (int64_t)now.tv_nsec;
        if (nsec <= 0) return 0;
        struct timespec delta;
        delta.tv_sec  = nsec / 1000000000LL;
        delta.tv_nsec = nsec % 1000000000LL;
        int64_t r = syscall(SYS_NANOSLEEP, (uint64_t)&delta, 0, 0, 0, 0, 0);
        if (r < 0) {
            if (rem) *rem = delta;   /* best-effort remainder */
            return (int)-r;
        }
        return 0;
    }
    int64_t r = syscall(SYS_NANOSLEEP, (uint64_t)req, (uint64_t)rem, 0, 0, 0, 0);
    return r < 0 ? (int)-r : 0;
}

int timespec_get(struct timespec *ts, int base) {
    if (!ts || base != TIME_UTC) return 0;
    if (clock_gettime(CLOCK_REALTIME, ts) != 0) return 0;
    return TIME_UTC;
}

int timespec_getres(struct timespec *ts, int base) {
    if (!ts || base != TIME_UTC) return 0;
    if (clock_getres(CLOCK_REALTIME, ts) != 0) return 0;
    return TIME_UTC;
}

/* ---- pseudo-terminal skeleton (Q11 declared; bodies missing) ----
 * The interface exists and never fails on grant/unlock; ptsname reports the
 * single skeleton name.  posix_openpt opens /dev/ptmx, which the device
 * layer does not provide yet, so it fails cleanly with ENOENT. */

int posix_openpt(int oflag) { return open("/dev/ptmx", oflag); }

int grantpt(int fd) { (void)fd; return 0; }

int unlockpt(int fd) { (void)fd; return 0; }

char *ptsname(int fd) { (void)fd; return "/dev/pts/0"; }

int ptsname_r(int fd, char *buf, size_t buflen) {
    (void)fd;
    static const char name[] = "/dev/pts/0";
    if (buflen < sizeof(name)) { errno = ERANGE; return ERANGE; }
    memcpy(buf, name, sizeof(name));
    return 0;
}

/* ---- network interface name/index family (Q10 declared; bodies missing) -
 * The OS has one NIC: eth0 == index 1. */

unsigned if_nametoindex(const char *ifname) {
    if (ifname && strcmp(ifname, "eth0") == 0) return 1;
    return 0;
}

char *if_indextoname(unsigned ifindex, char *ifname) {
    if (ifindex == 1) {
        strcpy(ifname, "eth0");
        return ifname;
    }
    return NULL;
}

struct if_nameindex *if_nameindex(void) {
    struct if_nameindex *arr = malloc(2 * sizeof(*arr));
    if (!arr) return NULL;
    char *n = strdup("eth0");
    if (!n) { free(arr); return NULL; }
    arr[0].if_index = 1; arr[0].if_name = n;
    arr[1].if_index = 0; arr[1].if_name = NULL;
    return arr;
}

void if_freenameindex(struct if_nameindex *ptr) {
    if (!ptr) return;
    free(ptr[0].if_name);
    free(ptr);
}

/* ---- sched family (Q8 declared; bodies missing) ---- */

int sched_yield(void) {
    syscall(SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
    return 0;
}

int sched_get_priority_max(int policy) { (void)policy; return 99; }

int sched_get_priority_min(int policy) { (void)policy; return 0; }

int sched_getscheduler(pid_t pid) {
    (void)pid;
    return SCHED_OTHER;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    (void)pid;
    (void)param;
    if (policy != SCHED_OTHER) { errno = EINVAL; return -1; }
    return 0;   /* single policy: accept-and-ignore (documented stub) */
}

int sched_getparam(pid_t pid, struct sched_param *param) {
    (void)pid;
    if (!param) { errno = EINVAL; return -1; }
    param->sched_priority = 0;
    return 0;
}

int sched_setparam(pid_t pid, const struct sched_param *param) {
    (void)pid;
    (void)param;
    return 0;
}

int sched_rr_get_interval(pid_t pid, struct timespec *tp) {
    (void)pid;
    if (!tp) { errno = EINVAL; return -1; }
    tp->tv_sec = 0;
    tp->tv_nsec = 10000000L;   /* 10 ms PIT tick quantum */
    return 0;
}

/* ---- getrusage (Q8 declared, documented ENOSYS stub; body missing) ---- */

int getrusage(int who, struct rusage *usage) {
    (void)who;
    (void)usage;
    errno = ENOSYS;
    return -1;
}

/* ---- atol (stdlib gap caught by the same sweep) ---- */

long atol(const char *s) { return (long)strtol(s, (char **)NULL, 10); }

/* =============== POSIX2024 Q13: AT-family completion bodies ================
 * POSIX2024_PLAN.md phase Q13: link(2)/linkat(2), symlinkat(2), mkfifoat(2),
 * mknod(2)/mknodat(2), utimensat(2)/futimens(2).  Syscall numbers per
 * kernel/arch/x86_64/syscall.c (Q13 block).  The VFS layer resolves AT_FDCWD
 * relative paths against the caller's cwd (Q12 copy_at_path), so these are
 * thin wrappers exactly like the Q12 batch above.
 */

#define SYS_LINK       90
#define SYS_MKNODAT   259
#define SYS_LINKAT    265
#define SYS_SYMLINKAT 266
#define SYS_UTIMENSAT 280

int link(const char *old, const char *new) {
    return q12_ret(syscall(SYS_LINK, (uint64_t)old, (uint64_t)new,
                           0, 0, 0, 0));
}

int linkat(int old_dfd, const char *old, int new_dfd, const char *new,
           int flags) {
    return q12_ret(syscall(SYS_LINKAT, (uint64_t)old_dfd, (uint64_t)old,
                           (uint64_t)new_dfd, (uint64_t)new,
                           (uint64_t)flags, 0));
}

int symlinkat(const char *target, int new_dfd, const char *linkpath) {
    return q12_ret(syscall(SYS_SYMLINKAT, (uint64_t)new_dfd,
                           (uint64_t)target, (uint64_t)linkpath, 0, 0, 0));
}

int mknodat(int dfd, const char *path, mode_t mode, dev_t dev) {
    return q12_ret(syscall(SYS_MKNODAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)mode, (uint64_t)dev, 0, 0));
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    return mknodat(AT_FDCWD, path, mode, dev);
}

int mkfifoat(int dfd, const char *path, mode_t mode) {
    return mknodat(dfd, path, (mode_t)((mode & 07777) | S_IFIFO), 0);
}

int utimensat(int dfd, const char *path, const struct timespec times[2],
              int flags) {
    return q12_ret(syscall(SYS_UTIMENSAT, (uint64_t)dfd, (uint64_t)path,
                           (uint64_t)times, (uint64_t)flags, 0, 0));
}

int futimens(int fd, const struct timespec times[2]) {
    /* Kernel: utimensat(dfd, NULL, times, 0) == futimens. */
    return q12_ret(syscall(SYS_UTIMENSAT, (uint64_t)fd, 0,
                           (uint64_t)times, 0, 0, 0));
}
