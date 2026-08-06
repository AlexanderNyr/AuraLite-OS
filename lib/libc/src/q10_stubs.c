/* libc/src/q10_stubs.c — Phase Q10 stub header implementations
 *
 * Provides minimal but functional implementations for:
 * syslog, nl_langinfo, iconv, search (hsearch/tsearch), wordexp, ftw/nftw,
 * statvfs, times, ftok, and stubs for sysv ipc.
 */

#include <syslog.h>
#include <langinfo.h>
#include <iconv.h>
#include <search.h>
#include <wordexp.h>
#include <ftw.h>
#include <monetary.h>
#include <sys/statvfs.h>
#include <sys/times.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <utmpx.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

/* =============================== syslog =================================== */

static int _log_mask = 0xFF;
static const char *_log_ident = NULL;
static int _log_opts = 0;
static int _log_facility = LOG_USER;

void openlog(const char *ident, int logopt, int facility) {
    _log_ident = ident;
    _log_opts = logopt;
    _log_facility = facility;
}

void closelog(void) {
    _log_ident = NULL;
}

int setlogmask(int maskpri) {
    int old = _log_mask;
    if (maskpri) _log_mask = maskpri;
    return old;
}

void vsyslog(int priority, const char *message, va_list ap) {
    static const char *levels[] = {
        "EMERG", "ALERT", "CRIT", "ERR", "WARNING", "NOTICE", "INFO", "DEBUG"
    };
    int lev = (priority & 7);
    if (lev > 7) lev = 7;
    if (!( _log_mask & (1 << lev))) return;
    dprintf(2, "[SYSLOG] %s: ", levels[lev]);
    if (_log_ident) dprintf(2, "%s: ", _log_ident);
    vdprintf(2, message, ap);
    dprintf(2, "\n");
}

void syslog(int priority, const char *message, ...) {
    va_list ap;
    va_start(ap, message);
    vsyslog(priority, message, ap);
    va_end(ap);
}

/* ============================= nl_langinfo ================================ */

static char _codeset[] = "UTF-8";
static char _d_fmt[] = "%Y-%m-%d";
static char _d_t_fmt[] = "%a %b %e %H:%M:%S %Y";
static char _t_fmt[] = "%H:%M:%S";
static char _am_str[] = "AM";
static char _pm_str[] = "PM";
static char _radixchar[] = ".";
static char _thousep[] = "";
static char _abday[] = "SunMonTueWedThuFriSat";
static char _abmon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

char *nl_langinfo(int item) {
    switch (item) {
    case CODESET: return _codeset;
    case D_T_FMT: return _d_t_fmt;
    case D_FMT:   return _d_fmt;
    case T_FMT:   return _t_fmt;
    case AM_STR:  return _am_str;
    case PM_STR:  return _pm_str;
    case RADIXCHAR: return _radixchar;
    case THOUSEP: return _thousep;
    case ABDAY_1: return _abday;     /* Actually should be individual items */
    case ABMON_1: return _abmon;
    default: return "";
    }
}

char *nl_langinfo_l(int item, locale_t loc) {
    (void)loc;
    return nl_langinfo(item);
}

/* ================================ iconv =================================== */

typedef struct {
    char tocode[8];
    char fromcode[8];
} iconv_cd_t;

iconv_t iconv_open(const char *tocode, const char *fromcode) {
    /* Simplified: only UTF-8 <-> UTF-8 passthrough */
    if (strcasecmp(tocode, "UTF-8") != 0 ||
        strcasecmp(fromcode, "UTF-8") != 0)
        return (iconv_t)-1;
    iconv_cd_t *cd = malloc(sizeof(iconv_cd_t));
    if (!cd) return (iconv_t)-1;
    strncpy(cd->tocode, tocode, sizeof(cd->tocode) - 1);
    strncpy(cd->fromcode, fromcode, sizeof(cd->fromcode) - 1);
    return (iconv_t)cd;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft) {
    if (cd == (iconv_t)-1) { errno = EBADF; return (size_t)-1; }
    (void)cd;
    if (!inbuf || !*inbuf || !outbuf || !*outbuf) return 0;
    /* Passthrough: copy input to output byte-for-byte */
    size_t n = *inbytesleft < *outbytesleft ? *inbytesleft : *outbytesleft;
    memcpy(*outbuf, *inbuf, n);
    *inbuf += n;
    *inbytesleft -= n;
    *outbuf += n;
    *outbytesleft -= n;
    return n;
}

int iconv_close(iconv_t cd) {
    if (cd == (iconv_t)-1) return -1;
    free(cd);
    return 0;
}

/* ================================ search ================================== */

/* Simple hash table for hsearch */
#define HTAB_SIZE 128
static struct { char *key; void *data; int used; } _htab[HTAB_SIZE];
static int _htab_inited = 0;

int hcreate(size_t nel) {
    (void)nel;
    if (_htab_inited) return 0;
    memset(_htab, 0, sizeof(_htab));
    _htab_inited = 1;
    return 1;
}

void hdestroy(void) {
    for (int i = 0; i < HTAB_SIZE; i++) {
        if (_htab[i].used) {
            free(_htab[i].key);
            _htab[i].used = 0;
        }
    }
    _htab_inited = 0;
}

static unsigned _hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = (h * 33) ^ (unsigned char)*s++;
    return h;
}

ENTRY *hsearch(ENTRY item, ACTION action) {
    if (!_htab_inited) return NULL;
    static ENTRY _hsearch_result;  /* static to avoid returning local address */
    unsigned idx = _hash(item.key) % HTAB_SIZE;
    unsigned start = idx;
    do {
        if (!_htab[idx].used) {
            if (action == ENTER) {
                _htab[idx].key = strdup(item.key);
                if (!_htab[idx].key) return NULL;
                _htab[idx].data = item.data;
                _htab[idx].used = 1;
                _hsearch_result.key = _htab[idx].key;
                _hsearch_result.data = _htab[idx].data;
                return &_hsearch_result;
            }
            return NULL;
        }
        if (strcmp(_htab[idx].key, item.key) == 0) {
            _hsearch_result.key = _htab[idx].key;
            _hsearch_result.data = _htab[idx].data;
            return &_hsearch_result;
        }
        idx = (idx + 1) % HTAB_SIZE;
    } while (idx != start);
    return NULL;
}

/* Binary tree (tsearch) — simple unbalanced BST */

struct _tree_node {
    const void *key;
    struct _tree_node *left, *right;
};

void *tsearch(const void *key, void **rootp,
              int (*compar)(const void *, const void *)) {
    if (!rootp) return NULL;
    struct _tree_node **pp = (struct _tree_node **)rootp;
    while (*pp) {
        int c = compar(key, (*pp)->key);
        if (c == 0) return *pp;
        pp = (c < 0) ? &(*pp)->left : &(*pp)->right;
    }
    struct _tree_node *n = malloc(sizeof(struct _tree_node));
    if (!n) return NULL;
    n->key = key;
    n->left = n->right = NULL;
    *pp = n;
    return n;
}

void *tfind(const void *key, void *const *rootp,
            int (*compar)(const void *, const void *)) {
    if (!rootp) return NULL;
    struct _tree_node *p = *(struct _tree_node **)rootp;
    while (p) {
        int c = compar(key, p->key);
        if (c == 0) return p;
        p = (c < 0) ? p->left : p->right;
    }
    return NULL;
}

void *tdelete(const void *key, void **rootp,
              int (*compar)(const void *, const void *)) {
    if (!rootp || !*rootp) return NULL;
    /* Find parent and node */
    struct _tree_node *parent = NULL, **pp = (struct _tree_node **)rootp;
    while (*pp) {
        int c = compar(key, (*pp)->key);
        if (c == 0) break;
        parent = *pp;
        pp = (c < 0) ? &(*pp)->left : &(*pp)->right;
    }
    if (!*pp) return NULL;
    struct _tree_node *node = *pp;
    /* Simple deletion: replace with left child's rightmost or right child */
    if (!node->left) {
        *pp = node->right;
    } else if (!node->right) {
        *pp = node->left;
    } else {
        struct _tree_node **rp = &node->right;
        while ((*rp)->left) rp = &(*rp)->left;
        node->key = (*rp)->key;
        struct _tree_node *tmp = *rp;
        *rp = (*rp)->right;
        free(tmp);
        return parent ? (void *)parent : (void *)rootp;
    }
    free(node);
    return parent ? (void *)parent : (void *)rootp;
}

static void _twalk_inner(const struct _tree_node *n,
                          void (*action)(const void *, VISIT, int), int depth) {
    if (!n) return;
    if (!n->left && !n->right) {
        action(n, leaf, depth);
    } else {
        action(n, preorder, depth);
        _twalk_inner(n->left, action, depth + 1);
        action(n, postorder, depth);
        _twalk_inner(n->right, action, depth + 1);
        action(n, endorder, depth);
    }
}

void twalk(const void *root,
           void (*action)(const void *, VISIT, int)) {
    _twalk_inner((const struct _tree_node *)root, action, 0);
}

/* Linear search */

void *lsearch(const void *key, void *base, size_t *nelp, size_t width,
              int (*compar)(const void *, const void *)) {
    char *b = (char *)base;
    for (size_t i = 0; i < *nelp; i++) {
        if (compar(key, b + i * width) == 0)
            return b + i * width;
    }
    /* Not found: append */
    memcpy(b + (*nelp) * width, key, width);
    (*nelp)++;
    return b + (*nelp - 1) * width;
}

void *lfind(const void *key, const void *base, size_t *nelp, size_t width,
            int (*compar)(const void *, const void *)) {
    const char *b = (const char *)base;
    for (size_t i = 0; i < *nelp; i++) {
        if (compar(key, b + i * width) == 0)
            return (void *)(b + i * width);
    }
    return NULL;
}

/* =============================== wordexp ================================== */

int wordexp(const char *words, wordexp_t *pwordexp, int flags) {
    if (!words || !pwordexp) return WRDE_BADVAL;
    if (!(flags & WRDE_REUSE)) {
        pwordexp->we_wordc = 0;
        pwordexp->we_wordv = NULL;
        pwordexp->we_offs = 0;
    }
    if (flags & WRDE_DOOFFS) {
        size_t offs = pwordexp->we_offs;
        pwordexp->we_wordv = calloc(offs + 64, sizeof(char *));
        if (!pwordexp->we_wordv) return WRDE_NOSPACE;
        pwordexp->we_offs = offs;
    } else {
        pwordexp->we_wordv = calloc(64, sizeof(char *));
        if (!pwordexp->we_wordv) return WRDE_NOSPACE;
    }

    /* Simple word splitting on whitespace, expand ~ */
    char tmp[1024];
    strncpy(tmp, words, sizeof(tmp) - 1);
    char *save = NULL;
    char *tok = strtok_r(tmp, " \t\n", &save);
    int count = 0;
    while (tok && count < 63) {
        if (tok[0] == '~' && (tok[1] == '\0' || tok[1] == '/')) {
            const char *home = getenv("HOME");
            if (home) {
                char expanded[1024];
                if (tok[1] == '/')
                    snprintf(expanded, sizeof(expanded), "%s%s", home, tok + 1);
                else
                    snprintf(expanded, sizeof(expanded), "%s", home);
                pwordexp->we_wordv[pwordexp->we_offs + count] = strdup(expanded);
            } else {
                pwordexp->we_wordv[pwordexp->we_offs + count] = strdup(tok);
            }
        } else {
            pwordexp->we_wordv[pwordexp->we_offs + count] = strdup(tok);
        }
        if (!pwordexp->we_wordv[pwordexp->we_offs + count]) {
            wordfree(pwordexp);
            return WRDE_NOSPACE;
        }
        count++;
        tok = strtok_r(NULL, " \t\n", &save);
    }
    pwordexp->we_wordc = (size_t)count;
    return 0;
}

void wordfree(wordexp_t *pwordexp) {
    if (!pwordexp || !pwordexp->we_wordv) return;
    for (size_t i = 0; i < pwordexp->we_wordc + pwordexp->we_offs; i++)
        free(pwordexp->we_wordv[i]);
    free(pwordexp->we_wordv);
    pwordexp->we_wordv = NULL;
    pwordexp->we_wordc = 0;
}

/* ============================= ftw / nftw ================================= */

int ftw(const char *dir, int (*fn)(const char *, const struct stat *, int),
        int depth) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    return nftw(dir, (int (*)(const char *, const struct stat *, int, struct FTW *))fn,
                depth, 0);
#pragma GCC diagnostic pop
}

int nftw(const char *dir, int (*fn)(const char *, const struct stat *, int, struct FTW *),
         int depth, int flags) {
    (void)depth;
    if (!dir || !fn) { errno = EINVAL; return -1; }
    struct stat st;
    struct FTW ftw;
    /* Call fn on the directory itself first */
    if (stat(dir, &st) == 0) {
        ftw.base = 0;
        ftw.level = 0;
        int r = fn(dir, &st, FTW_D, &ftw);
        if (r != 0) return r;
    }

    if ((flags & FTW_PHYS) || S_ISDIR(st.st_mode)) {
        DIR *d = opendir(dir);
        if (!d) return -1;
        struct dirent *de;
        char path[4096];
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            size_t dl = strlen(dir);
            snprintf(path, sizeof(path), "%s%s%s",
                     dir, dl > 0 && dir[dl-1] == '/' ? "" : "/", de->d_name);
            if (stat(path, &st) != 0) continue;
            int type = S_ISDIR(st.st_mode) ? FTW_D : (S_ISLNK(st.st_mode) ? FTW_SL : FTW_F);
            ftw.base = (int)(strrchr(path, '/') ? (strrchr(path, '/') - path + 1) : 0);
            ftw.level = 1;
            int r = fn(path, &st, type, &ftw);
            if (r != 0) { closedir(d); return r; }
        }
        closedir(d);
    }
    return 0;
}

/* ============================= monetary =================================== */

ssize_t strfmon(char *s, size_t max, const char *format, ...) {
    /* Minimal stub: copies format to output, replacing %n with "0.00" and %i with "USD 0.00" */
    va_list ap;
    va_start(ap, format);
    char buf[256];
    int pos = 0;
    for (const char *f = format; *f && pos < (int)sizeof(buf) - 1; f++) {
        if (*f == '%') {
            f++;
            if (*f == 'n') {
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "0.00");
            } else if (*f == 'i') {
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "USD 0.00");
            } else if (*f == '%') {
                buf[pos++] = '%';
            }
        } else {
            buf[pos++] = *f;
        }
    }
    buf[pos] = '\0';
    va_end(ap);
    size_t n = (size_t)pos;
    if (s && max > 0) {
        size_t cp = n < max ? n : max - 1;
        memcpy(s, buf, cp);
        s[cp] = '\0';
    }
    return n;
}

/* ============================= statvfs ==================================== */

int statvfs(const char *path, struct statvfs *buf) {
    (void)path;
    if (!buf) { errno = EFAULT; return -1; }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 1048576;
    buf->f_bfree = 524288;
    buf->f_bavail = 524288;
    buf->f_files = 65536;
    buf->f_ffree = 32768;
    buf->f_favail = 32768;
    buf->f_namemax = 255;
    return 0;
}

int fstatvfs(int fd, struct statvfs *buf) {
    (void)fd;
    return statvfs("/", buf);
}

/* =============================== times ==================================== */

clock_t times(struct tms *buf) {
    if (!buf) return (clock_t)-1;
    memset(buf, 0, sizeof(*buf));
    return 0;
}

/* =============================== ftok ===================================== */

key_t ftok(const char *path, int id) {
    struct stat st;
    if (stat(path, &st) < 0) return (key_t)-1;
    /* Standard ftok: combine dev+ino with id.  Our struct stat lacks st_dev,
       so we use 0 for the device component. */
    return (key_t)((id & 0xFF) | ((st.st_inode & 0xFF) << 8) | ((st.st_inode >> 8 & 0xFFFF) << 16));
}

/* ====================== SysV IPC (Q14) ==================================== */

/* Wrappers for the Q14 kernel objects (SYS_* numbers in <unistd.h>).
 * All return -1 and set errno on failure (in-band negative errno). */

static long ipc_ret(long raw) {
    if (raw < 0) { errno = (int)-raw; return -1; }
    return raw;
}

int semget(key_t key, int nsems, int semflg) {
    return (int)ipc_ret(syscall(SYS_SEMGET, (uint64_t)key,
                                (uint64_t)nsems, (uint64_t)semflg, 0, 0, 0));
}

int semop(int semid, struct sembuf *sops, size_t nsops) {
    return (int)ipc_ret(syscall(SYS_SEMOP, (uint64_t)semid,
                                (uint64_t)sops, (uint64_t)nsops, 0, 0, 0));
}

/* The 4th argument is the traditional union semun { int val; struct
 * semid_ds *buf; unsigned short *array; }.  It is passed by value as one
 * register-sized argument. */
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

int semctl(int semid, int semnum, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    union semun arg = va_arg(ap, union semun);
    va_end(ap);
    return (int)ipc_ret(syscall(SYS_SEMCTL, (uint64_t)semid, (uint64_t)semnum,
                                (uint64_t)cmd, (uint64_t)arg.val, 0, 0));
}

int shmget(key_t key, size_t size, int shmflg) {
    return (int)ipc_ret(syscall(SYS_SHMGET, (uint64_t)key,
                                (uint64_t)size, (uint64_t)shmflg, 0, 0, 0));
}

void *shmat(int shmid, const void *shmaddr, int shmflg) {
    long r = syscall(SYS_SHMAT, (uint64_t)shmid, (uint64_t)shmaddr,
                     (uint64_t)shmflg, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return (void *)-1; }
    return (void *)(uintptr_t)r;
}

int shmdt(const void *shmaddr) {
    return (int)ipc_ret(syscall(SYS_SHMDT, (uint64_t)shmaddr, 0, 0, 0, 0, 0));
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
    return (int)ipc_ret(syscall(SYS_SHMCTL, (uint64_t)shmid, (uint64_t)cmd,
                                (uint64_t)buf, 0, 0, 0));
}

int msgget(key_t key, int msgflg) {
    return (int)ipc_ret(syscall(SYS_MSGGET, (uint64_t)key,
                                (uint64_t)msgflg, 0, 0, 0, 0));
}

int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg) {
    return (int)ipc_ret(syscall(SYS_MSGSND, (uint64_t)msqid, (uint64_t)msgp,
                                (uint64_t)msgsz, (uint64_t)msgflg, 0, 0));
}

ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg) {
    return (ssize_t)ipc_ret(syscall(SYS_MSGRCV, (uint64_t)msqid, (uint64_t)msgp,
                                    (uint64_t)msgsz, (uint64_t)msgtyp,
                                    (uint64_t)msgflg, 0));
}

int msgctl(int msqid, int cmd, struct msqid_ds *buf) {
    return (int)ipc_ret(syscall(SYS_MSGCTL, (uint64_t)msqid, (uint64_t)cmd,
                                (uint64_t)buf, 0, 0, 0));
}

/* =============================== utmpx ==================================== */

void setutxent(void) {}
void endutxent(void) {}
struct utmpx *getutxent(void) { return NULL; }
