/* w32/src/kernel32_ps.c — W32A-2 processes.
 *
 * Four models a reader needs before touching this file:
 *
 * BIRTH.  CreateProcess is fork+execve.  A target starting with "MZ" is a PE
 * and is respawned under /apps/w32run (the kernel never runs a PE directly
 * — binding lives in w32run); an ELF runs directly; anything else is
 * BAD_EXE_FORMAT.  Without bInheritHandles the child closes every fd but
 * 0/1/2 (found by scanning /proc/self/fd); with it, only fds marked by
 * SetHandleInformation survive.  CREATE_SUSPENDED is refused (no threads
 * yet); the priority class is accepted and ignored (no scheduler API).
 *
 * DEATH.  No SIGCHLD handler exists; instead every status query sweeps
 * waitpid(-1, WNOHANG) so exited children land in a ring of cached exit
 * codes instead of lingering as zombies.  A foreign pid's times are zeros
 * (no interface reads another process's clocks); our own children's
 * creation times are recorded at spawn and their exit times at reap.
 *
 * IDENTITY.  The version APIs report Windows 10 build 19045.  That is the
 * one sanctioned impersonation in the personality: it lives in one place
 * (below), it is greppable, and it is documented here rather than
 * scattered.  Everything else that can be measured — CPU features via
 * CPUID, page size and CPU count via sysconf, the process list via /proc —
 * is measured, not claimed.
 *
 * MODULES.  One module exists: this process.  GetModuleHandle(NULL) and
 * FROM_ADDRESS both resolve to it; LoadLibrary records names in a table
 * and hands back cookies, and GetModuleFileName reads the table back.
 * There is no loader beneath — the cookies are honest because the table
 * backs them, and FreeLibrary drops them.
 */

#include "w32/kernel32.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/w32_module.h"

#ifndef AURALITE_W32_HOST_TEST
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <cpuid.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#endif

/* The process environment, consulted live (a second declaration next to the
 * libc's own is harmless in C). */
extern char **environ;

/* The one impersonation.  Windows 10, build 19045. */
#define PS_VER_MAJOR 10u
#define PS_VER_MINOR 0u
#define PS_VER_BUILD 19045u

/* ---- startup snapshot ---------------------------------------------------------
 *
 * w32_ps_init runs once at CRT startup (argc/argv from w32run, environ from
 * the real one).  It snapshots the command line (canonically re-quoted —
 * argv has already lost the original quoting, and the rebuild is the
 * documented equivalent), the startup directory (so a relative argv[0]
 * still resolves after SetCurrentDirectory), and the birth timestamp.
 */

static char **ps_argv_saved;
static int ps_argc_saved;
static char ps_startup_cwd[4096];
static W32_WCHAR *ps_cmdline_w;
static uint64_t ps_birth_ft;
static W32_DWORD ps_encode_cookie;

void w32_ps_init(int argc, char **argv, char **envp) {
    struct timespec ts;
    size_t total = 0;
    size_t pos = 0;
    int i;

    (void)envp;                 /* environ is consulted live, not snapped */
    ps_argc_saved = argc;
    ps_argv_saved = argv;
    if (!getcwd(ps_startup_cwd, sizeof(ps_startup_cwd))) {
        ps_startup_cwd[0] = '/';
        ps_startup_cwd[1] = '\0';
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        ps_birth_ft = ((uint64_t)(ts.tv_sec + 11644473600LL)) * 10000000ULL +
            (uint64_t)(ts.tv_nsec / 100);
    }
    /* Re-quote argv into one command line.  An argument needs quotes when
     * it is empty or holds space, tab or a quote; quotes and trailing
     * backslashes escape the CommandLineToArgvW way so the child that
     * splits this line gets argv back exactly. */
    for (i = 0; i < argc; i++) {
        const char *a = argv[i] ? argv[i] : "";
        size_t n = strlen(a);
        size_t extra = 2;       /* a space and the NUL/terminator */
        size_t bs = 0;
        size_t j;
        int needq = (n == 0);
        for (j = 0; j < n; j++) {
            if (a[j] == ' ' || a[j] == '\t' || a[j] == '"')
                needq = 1;
            if (a[j] == '"')
                extra++;        /* backslash-escaped */
        }
        if (needq) {
            extra += 2;
            for (j = n; j > 0 && a[j - 1] == '\\'; j--)
                extra++;        /* trailing runs double under quotes */
        }
        (void)bs;
        total += n + extra;
    }
    ps_cmdline_w = (W32_WCHAR *)malloc((total + 1) * sizeof(W32_WCHAR));
    if (!ps_cmdline_w)
        return;
    for (i = 0; i < argc; i++) {
        const char *a = argv[i] ? argv[i] : "";
        size_t n = strlen(a);
        size_t j;
        int needq = (n == 0);
        for (j = 0; j < n; j++) {
            if (a[j] == ' ' || a[j] == '\t' || a[j] == '"')
                needq = 1;
        }
        if (i > 0)
            ps_cmdline_w[pos++] = (W32_WCHAR)' ';
        if (needq)
            ps_cmdline_w[pos++] = (W32_WCHAR)'"';
        for (j = 0; j < n; j++) {
            size_t bs = 0;
            while (j < n && a[j] == '\\') {
                bs++;
                j++;
            }
            if (j == n) {
                size_t k;
                for (k = 0; k < bs * (needq ? 2 : 1); k++)
                    ps_cmdline_w[pos++] = (W32_WCHAR)'\\';
                break;
            }
            if (a[j] == '"') {
                size_t k;
                for (k = 0; k < bs * 2 + 1; k++)
                    ps_cmdline_w[pos++] = (W32_WCHAR)'\\';
                ps_cmdline_w[pos++] = (W32_WCHAR)'"';
            } else {
                size_t k;
                for (k = 0; k < bs; k++)
                    ps_cmdline_w[pos++] = (W32_WCHAR)'\\';
                /* argv is UTF-8 bytes; the command line is UTF-16 units.
                 * Non-ASCII bytes convert below, one code point at a
                 * time — ASCII fast path first. */
                if ((unsigned char)a[j] < 0x80) {
                    ps_cmdline_w[pos++] = (W32_WCHAR)(unsigned char)a[j];
                } else {
                    size_t used = 0;
                    size_t need = 0;
                    /* Decode one UTF-8 sequence starting at j. */
                    unsigned char c0 = (unsigned char)a[j];
                    size_t seqlen = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : 2;
                    uint16_t units[2];
                    int rc;
                    if (j + seqlen > n)
                        seqlen = n - j;
                    rc = w32_utf8_to_utf16(a + j, seqlen, units, 2, &need);
                    if (rc != W32_UTF_OK || need == 0 || need > 2) {
                        ps_cmdline_w[pos++] = (W32_WCHAR)'?';
                        used = 1;
                    } else {
                        size_t k2;
                        for (k2 = 0; k2 < need; k2++)
                            ps_cmdline_w[pos++] = units[k2];
                        used = seqlen;
                    }
                    j += used - 1;
                }
            }
        }
        if (needq)
            ps_cmdline_w[pos++] = (W32_WCHAR)'"';
    }
    ps_cmdline_w[pos] = 0;
    /* The pointer cookie: pid, time and stack address folded together.  Not
     * cryptographic — it only has to differ per process and be opaque. */
    {
        uintptr_t sp = (uintptr_t)&total;
        ps_encode_cookie = (W32_DWORD)(getpid() * 0x9E3779B1u +
            (W32_DWORD)sp + (W32_DWORD)(sp >> 32) * 0x85EBCA6Bu +
            (W32_DWORD)ps_birth_ft);
        if (ps_encode_cookie == 0)
            ps_encode_cookie = 0xA5A5A5A5u;
    }
}

static W32_BOOL ps_fail(W32_DWORD code) {
    w32_set_last_error(code);
    return 0;
}

static char *ps_w16_dup(const W32_WCHAR *w) {
    size_t n;
    size_t need = 0;
    size_t need2 = 0;
    char *out;
    int rc;

    if (!w) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    n = w32_utf16_len(w, 32768);
    if (n == 32768) {
        w32_set_last_error(W32_ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    rc = w32_utf16_to_utf8(w, n, NULL, 0, &need);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return NULL;
    }
    out = (char *)malloc(need + 1);
    if (!out) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    rc = w32_utf16_to_utf8(w, n, out, need, &need2);
    if (rc != W32_UTF_OK || need2 != need) {
        free(out);
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return NULL;
    }
    out[need] = '\0';
    return out;
}

static W32_DWORD ps_copy_out(const char *s, W32_WCHAR *buf, W32_DWORD cch) {
    size_t need = 0;
    size_t need2 = 0;
    int rc;

    if (!s) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    rc = w32_utf8_to_utf16(s, strlen(s), NULL, 0, &need);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (buf && cch >= (W32_DWORD)(need + 1) && cch > 0) {
        rc = w32_utf8_to_utf16(s, strlen(s), buf, need, &need2);
        if (rc != W32_UTF_OK || need2 != need) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        buf[need] = 0;
    }
    return (W32_DWORD)(need + 1);
}

/* ---- process objects ----------------------------------------------------------
 *
 * One kind (PROC), three shapes: a process, a thread-ref (the main thread,
 * whose id on this kernel is the pid), and a toolhelp snapshot.  Every
 * entry point checks the tag it needs; CloseHandle frees any of them.
 */

#define PS_TAG_PROCESS 1
#define PS_TAG_THREAD  2
#define PS_TAG_SNAP    3

struct ps_snap_entry {
    W32_DWORD pid;
    W32_DWORD ppid;
    char exe[260];
};

struct ps_obj {
    int tag;
    int pid;
    uint64_t birth_ft;          /* spawn time (creation), 0 if unknown */
    uint64_t exit_ft;           /* reap time, 0 while running/unknown */
    int exit_code;
    int exited;
    struct ps_snap_entry *snap;
    size_t snap_n;
    size_t snap_i;
};

static void ps_obj_free(void *p) {
    struct ps_obj *o = (struct ps_obj *)p;
    if (!o)
        return;
    free(o->snap);
    free(o);
}

/* ---- reaping ----------------------------------------------------------------------
 *
 * waitpid(-1, WNOHANG) sweeps every exited child — queried or not — into a
 * ring, so zombies never accumulate past the next status call.  The ring is
 * small and documented; past 32 uncollected exits the oldest entry drops,
 * which only costs a cached code, never a leak (the child is reaped).
 */

#define PS_RING_MAX 32

static struct {
    int in_use;
    int pid;
    int code;
    uint64_t exit_ft;
} ps_ring[PS_RING_MAX];

static uint64_t ps_now_ft(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return ((uint64_t)(ts.tv_sec + 11644473600LL)) * 10000000ULL +
        (uint64_t)(ts.tv_nsec / 100);
}

static void ps_ring_add(int pid, int code) {
    size_t i;
    for (i = 0; i < PS_RING_MAX; i++) {
        if (!ps_ring[i].in_use) {
            ps_ring[i].in_use = 1;
            ps_ring[i].pid = pid;
            ps_ring[i].code = code;
            ps_ring[i].exit_ft = ps_now_ft();
            return;
        }
    }
    /* Full: the oldest slot (0) is recycled.  A debug build could log it;
     * a silent drop only loses a cached code. */
    ps_ring[0].pid = pid;
    ps_ring[0].code = code;
    ps_ring[0].exit_ft = ps_now_ft();
}

static int ps_ring_find(int pid, int *code, uint64_t *exit_ft) {
    size_t i;
    for (i = 0; i < PS_RING_MAX; i++) {
        if (ps_ring[i].in_use && ps_ring[i].pid == pid) {
            if (code)
                *code = ps_ring[i].code;
            if (exit_ft)
                *exit_ft = ps_ring[i].exit_ft;
            return 1;
        }
    }
    return 0;
}

static void ps_sweep(void) {
    int st;
    int pid;
    for (;;) {
        pid = waitpid(-1, &st, WNOHANG);
        if (pid <= 0)
            return;
        if (WIFEXITED(st))
            ps_ring_add(pid, WEXITSTATUS(st));
        else if (WIFSIGNALED(st))
            ps_ring_add(pid, 128 + WTERMSIG(st));
        else
            ps_ring_add(pid, WSTOPSIG(st));
    }
}

/* ---- command lines ---------------------------------------------------------------
 *
 * CommandLineToArgvW, re-derived: backslashes escape only before a quote
 * (2n before a quote become n literal, 2n+1 become n literal plus a literal
 * quote), quotes toggle in-word mode, spaces split outside quotes.  The
 * argv[0] question — quoted program name or bare token — falls out of the
 * same rule.
 */

static char **ps_split_cmdline(const char *cmd, int *argcOut) {
    int cap = 8;
    int argc = 0;
    char **argv = NULL;
    const char *p = cmd;

    argv = (char **)malloc((size_t)(cap + 1) * sizeof(char *));
    if (!argv)
        return NULL;
    while (*p) {
        size_t bcap = 64;
        size_t blen = 0;
        char *word;
        int inquote = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        word = (char *)malloc(bcap);
        if (!word)
            goto fail;
        while (*p) {
            size_t bs = 0;
            while (*p == '\\') {
                bs++;
                p++;
            }
            if (*p == '"') {
                size_t k;
                for (k = 0; k < bs / 2; k++) {
                    if (blen + 1 >= bcap) {
                        bcap *= 2;
                        word = (char *)realloc(word, bcap);
                        if (!word)
                            goto fail;
                    }
                    word[blen++] = '\\';
                }
                if (bs % 2 == 1) {
                    if (blen + 1 >= bcap) {
                        bcap *= 2;
                        word = (char *)realloc(word, bcap);
                        if (!word)
                            goto fail;
                    }
                    word[blen++] = '"';
                    p++;
                } else {
                    inquote = !inquote;
                    p++;
                }
            } else if ((*p == ' ' || *p == '\t') && !inquote) {
                size_t k;
                for (k = 0; k < bs; k++) {
                    if (blen + 1 >= bcap) {
                        bcap *= 2;
                        word = (char *)realloc(word, bcap);
                        if (!word)
                            goto fail;
                    }
                    word[blen++] = '\\';
                }
                break;
            } else if (*p == '\0') {
                size_t k;
                for (k = 0; k < bs; k++) {
                    if (blen + 1 >= bcap) {
                        bcap *= 2;
                        word = (char *)realloc(word, bcap);
                        if (!word)
                            goto fail;
                    }
                    word[blen++] = '\\';
                }
                break;
            } else {
                size_t k;
                for (k = 0; k < bs; k++) {
                    if (blen + 1 >= bcap) {
                        bcap *= 2;
                        word = (char *)realloc(word, bcap);
                        if (!word)
                            goto fail;
                    }
                    word[blen++] = '\\';
                }
                if (blen + 1 >= bcap) {
                    bcap *= 2;
                    word = (char *)realloc(word, bcap);
                    if (!word)
                        goto fail;
                }
                word[blen++] = *p++;
            }
        }
        word[blen] = '\0';
        if (argc >= cap) {
            cap *= 2;
            argv = (char **)realloc(argv, (size_t)(cap + 1) * sizeof(char *));
            if (!argv)
                goto fail;
        }
        argv[argc++] = word;
    }
    argv[argc] = NULL;
    if (argcOut)
        *argcOut = argc;
    return argv;
fail:
    if (argv) {
        int i;
        for (i = 0; i < argc; i++)
            free(argv[i]);
        free(argv);
    }
    return NULL;
}

static void ps_free_argv(char **argv, int argc) {
    int i;
    if (!argv)
        return;
    for (i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

/* Resolve the application the CreateProcess way: as-given first (with and
 * without .exe), then the PATH.  Returns a malloc'd host path, or NULL
 * with last error set. */
static char *ps_resolve_app(const char *app) {
    char *host = NULL;
    struct stat st;
    const char *path;
    char *copy = NULL;
    char *dir;
    size_t i;

    /* Bare "C:\..." style names arrive with backslashes; fold them. */
    {
        size_t n = strlen(app);
        host = (char *)malloc(n + 5);
        if (!host) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        for (i = 0; i < n; i++)
            host[i] = (app[i] == '\\') ? '/' : app[i];
        host[n] = '\0';
        /* A drive prefix ("C:/x") roots at "/x". */
        if (((host[0] >= 'A' && host[0] <= 'Z') ||
             (host[0] >= 'a' && host[0] <= 'z')) && host[1] == ':') {
            if (host[0] != 'C' && host[0] != 'c') {
                free(host);
                w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
                return NULL;
            }
            memmove(host, host + 2, n - 1);
            n -= 2;
        }
        if (stat(host, &st) == 0 && S_ISREG(st.st_mode))
            return host;
        /* Try with .exe appended, the way the loader does. */
        memcpy(host + n, ".exe", 5);
        if (stat(host, &st) == 0 && S_ISREG(st.st_mode))
            return host;
        /* A name containing a slash is a path: no PATH search. */
        if (strchr(app, '/') || strchr(app, '\\')) {
            free(host);
            w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
            return NULL;
        }
        free(host);
        host = NULL;
    }
    path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin:/apps";
    copy = (char *)malloc(strlen(path) + 1);
    if (!copy) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    strcpy(copy, path);
    dir = copy;
    for (;;) {
        char *sep = strchr(dir, ':');
        size_t dl;
        char *cand;
        if (sep)
            *sep = '\0';
        dl = strlen(dir);
        cand = (char *)malloc(dl + 1 + strlen(app) + 5);
        if (!cand) {
            free(copy);
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        memcpy(cand, dir, dl);
        cand[dl] = '/';
        strcpy(cand + dl + 1, app);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
            free(copy);
            return cand;
        }
        strcpy(cand + dl + 1 + strlen(app), ".exe");
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
            free(copy);
            return cand;
        }
        free(cand);
        if (!sep)
            break;
        dir = sep + 1;
    }
    free(copy);
    w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
    return NULL;
}

/* What is this file?  1 = PE ("MZ"), 2 = ELF, 0 = neither, -1 = unreadable. */
static int ps_sniff(const char *path) {
    int fd;
    unsigned char magic[4];
    ssize_t n;
    size_t got = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while (got < sizeof(magic)) {
        n = read(fd, magic + got, sizeof(magic) - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (got < 2)
        return 0;
    if (magic[0] == 'M' && magic[1] == 'Z')
        return 1;
    if (got >= 4 && magic[0] == 0x7F && magic[1] == 'E' &&
        magic[2] == 'L' && magic[3] == 'F')
        return 2;
    return 0;
}

/* Convert a double-NUL W16 environment block to a UTF-8 envp.  NULL in
 * gives NULL out (inherit).  Malformed entries (no '=') are skipped the
 * way the loader skips them. */
static char **ps_env_block(const W32_WCHAR *block, int *countOut) {
    int cap = 16;
    int n = 0;
    char **envp;
    const W32_WCHAR *p;

    if (!block) {
        if (countOut)
            *countOut = 0;
        return NULL;
    }
    envp = (char **)malloc((size_t)(cap + 1) * sizeof(char *));
    if (!envp) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    p = block;
    while (*p) {
        size_t len = w32_utf16_len(p, 32768);
        size_t need = 0;
        size_t need2 = 0;
        char *one;
        int rc;
        if (len == 32768) {
            ps_free_argv(envp, n);
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return NULL;
        }
        rc = w32_utf16_to_utf8(p, len, NULL, 0, &need);
        if (rc == W32_UTF_OK) {
            one = (char *)malloc(need + 1);
            if (!one) {
                ps_free_argv(envp, n);
                w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                return NULL;
            }
            rc = w32_utf16_to_utf8(p, len, one, need, &need2);
            if (rc == W32_UTF_OK && need2 == need && strchr(one, '=')) {
                one[need] = '\0';
                if (n >= cap) {
                    cap *= 2;
                    envp = (char **)realloc(envp,
                        (size_t)(cap + 1) * sizeof(char *));
                    if (!envp) {
                        free(one);
                        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                        return NULL;
                    }
                }
                envp[n++] = one;
            } else {
                free(one);
            }
        }
        p += len + 1;
    }
    envp[n] = NULL;
    if (countOut)
        *countOut = n;
    return envp;
}

/* Close every fd but 0/1/2 that must not survive into the child.  With
 * inherit, the SetHandleInformation table decides; without, everything
 * goes.  The fd list comes from /proc/self/fd, read with raw syscalls —
 * opendir after fork is legal here (no threads), but the scan must skip
 * the scan's own fd. */
static void ps_scrub_fds(int inherit) {
    DIR *d;
    struct dirent *de;
    int self;

    d = opendir("/proc/self/fd");
    if (!d) {
        /* No /proc: fall back to closing a plausible range.  The three
         * standard fds are never touched. */
        int fd;
        for (fd = 3; fd < 256; fd++) {
            if (inherit && w32_fs_is_inheritable(fd))
                continue;
            close(fd);
        }
        return;
    }
    self = dirfd(d);
    while ((de = readdir(d)) != NULL) {
        char *end;
        long fd;
        if (de->d_name[0] == '.')
            continue;
        fd = strtol(de->d_name, &end, 10);
        if (*end != '\0' || fd < 0)
            continue;
        if (fd == self || fd < 3)
            continue;
        if (inherit && w32_fs_is_inheritable((int)fd))
            continue;
        close((int)fd);
    }
    closedir(d);
}

static W32_BOOL ps_spawn(const char *app_utf8, const char *cmd_utf8,
                         void *envBlock, const char *cwd_utf8,
                         W32_BOOL inherit, W32_DWORD flags,
                         int siStdIn, int siStdOut, int siStdErr, int useStd,
                         W32_PROCESS_INFORMATION *pi) {
    char *resolved = NULL;
    char **child_argv = NULL;
    int child_argc = 0;
    char **child_envp = NULL;
    int child_envc = 0;
    char *cwd_host = NULL;
    char *first = NULL;
    const char *cmdfile = NULL;
    int kind;
    int pid;
    struct ps_obj *proc = NULL;
    struct ps_obj *thr = NULL;
    W32_HANDLE hProc = NULL;
    W32_HANDLE hThr = NULL;

    if (!pi)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (flags & W32_CREATE_SUSPENDED)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (!app_utf8 && !cmd_utf8)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);

    /* The module name: explicit, or the first token of the command line. */
    if (app_utf8) {
        first = (char *)malloc(strlen(app_utf8) + 1);
        if (!first)
            return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        strcpy(first, app_utf8);
    } else {
        int n = 0;
        char **toks = ps_split_cmdline(cmd_utf8, &n);
        if (!toks)
            return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        if (n == 0) {
            ps_free_argv(toks, n);
            return ps_fail(W32_ERROR_INVALID_PARAMETER);
        }
        first = toks[0];
        toks[0] = NULL;
        ps_free_argv(toks, n);
    }
    resolved = ps_resolve_app(first);
    free(first);
    if (!resolved)
        return 0;

    /* The child's argv: the command line re-split, with argv[0] forced to
     * the resolved module (Windows builds argv from the command line the
     * same way — the module path leads). */
    if (cmd_utf8) {
        child_argv = ps_split_cmdline(cmd_utf8, &child_argc);
        if (!child_argv) {
            free(resolved);
            return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        }
        cmdfile = child_argv[0];
        (void)cmdfile;
        free(child_argv[0]);
        child_argv[0] = resolved;
        resolved = NULL;        /* owned by argv now */
    } else {
        child_argv = (char **)malloc(2 * sizeof(char *));
        if (!child_argv) {
            free(resolved);
            return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        }
        child_argv[0] = resolved;
        resolved = NULL;
        child_argv[1] = NULL;
        child_argc = 1;
    }

    if (cwd_utf8) {
        /* Reuse the fs translator's rules via a local fold: backslashes
         * to slashes, C: rooted at /. */
        size_t n = strlen(cwd_utf8);
        size_t i;
        struct stat st;
        cwd_host = (char *)malloc(n + 1);
        if (!cwd_host) {
            ps_free_argv(child_argv, child_argc);
            return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        }
        for (i = 0; i < n; i++)
            cwd_host[i] = (cwd_utf8[i] == '\\') ? '/' : cwd_utf8[i];
        cwd_host[n] = '\0';
        if (((cwd_host[0] >= 'A' && cwd_host[0] <= 'Z') ||
             (cwd_host[0] >= 'a' && cwd_host[0] <= 'z')) &&
            cwd_host[1] == ':') {
            if (cwd_host[0] != 'C' && cwd_host[0] != 'c') {
                free(cwd_host);
                ps_free_argv(child_argv, child_argc);
                return ps_fail(W32_ERROR_PATH_NOT_FOUND);
            }
            memmove(cwd_host, cwd_host + 2, n - 1);
        }
        if (stat(cwd_host, &st) != 0) {
            W32_DWORD code = w32_error_from_c(-1);
            free(cwd_host);
            ps_free_argv(child_argv, child_argc);
            if (code == W32_ERROR_FILE_NOT_FOUND)
                code = W32_ERROR_PATH_NOT_FOUND;
            w32_set_last_error(code);
            return 0;
        }
        if (!S_ISDIR(st.st_mode)) {
            free(cwd_host);
            ps_free_argv(child_argv, child_argc);
            return ps_fail(W32_ERROR_DIRECTORY);
        }
    }

    if (envBlock) {
        child_envp = ps_env_block((const W32_WCHAR *)envBlock, &child_envc);
        if (!child_envp && child_envc == 0) {
            /* NULL envp with count 0 is ambiguous (inherit vs. convert a
             * trivially empty block); ps_env_block sets last error only on
             * real failure, so check it. */
            if (w32_get_last_error_raw() != W32_ERROR_SUCCESS) {
                free(cwd_host);
                ps_free_argv(child_argv, child_argc);
                return 0;
            }
        }
    }

    kind = ps_sniff(child_argv[0]);
    if (kind <= 0) {
        free(cwd_host);
        ps_free_argv(child_argv, child_argc);
        ps_free_argv(child_envp, child_envc);
        if (kind < 0)
            return ps_fail(w32_error_from_c(-1));
        return ps_fail(W32_ERROR_BAD_EXE_FORMAT);
    }

    ps_sweep();                 /* reap strays before minting handles */
    pid = fork();
    if (pid < 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(cwd_host);
        ps_free_argv(child_argv, child_argc);
        ps_free_argv(child_envp, child_envc);
        w32_set_last_error(code);
        return 0;
    }
    if (pid == 0) {
        /* The child.  No w32 calls from here to execve: only raw syscalls
         * and async-safe memory the parent already allocated. */
        if (cwd_host && chdir(cwd_host) != 0)
            _exit(127);
        if (useStd) {
            if (siStdIn >= 0 && siStdIn != 0)
                dup2(siStdIn, 0);
            if (siStdOut >= 0 && siStdOut != 1)
                dup2(siStdOut, 1);
            if (siStdErr >= 0 && siStdErr != 2)
                dup2(siStdErr, 2);
        }
        ps_scrub_fds(inherit != 0);
        if (kind == 1) {
            /* PE: respawn under w32run.  argv[0] stays the module (w32run
             * shifts it into place), so the child's own GetModuleFileName
             * and GetCommandLine come out right. */
            char **wargv;
            int i;
            wargv = (char **)malloc((size_t)(child_argc + 2) *
                sizeof(char *));
            if (!wargv)
                _exit(127);
            wargv[0] = (char *)"/apps/w32run";
            for (i = 0; i < child_argc; i++)
                wargv[i + 1] = child_argv[i];
            wargv[child_argc + 1] = NULL;
            if (child_envp)
                execve("/apps/w32run", wargv, child_envp);
            else
                execve("/apps/w32run", wargv, environ);
            _exit(127);
        }
        if (child_envp)
            execve(child_argv[0], child_argv, child_envp);
        else
            execve(child_argv[0], child_argv, environ);
        _exit(127);
    }

    free(cwd_host);
    ps_free_argv(child_argv, child_argc);
    ps_free_argv(child_envp, child_envc);

    proc = (struct ps_obj *)calloc(1, sizeof(*proc));
    thr = (struct ps_obj *)calloc(1, sizeof(*thr));
    if (!proc || !thr) {
        free(proc);
        free(thr);
        return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
    }
    proc->tag = PS_TAG_PROCESS;
    proc->pid = pid;
    proc->birth_ft = ps_now_ft();
    thr->tag = PS_TAG_THREAD;
    thr->pid = pid;
    thr->birth_ft = proc->birth_ft;
    hProc = w32_handle_alloc_obj(W32_HANDLE_KIND_PROC, proc, ps_obj_free);
    if (!hProc) {
        ps_obj_free(proc);
        ps_obj_free(thr);
        return ps_fail(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    hThr = w32_handle_alloc_obj(W32_HANDLE_KIND_PROC, thr, ps_obj_free);
    if (!hThr) {
        ps_obj_free(w32_handle_release_obj(hProc, W32_HANDLE_KIND_PROC));
        ps_obj_free(thr);
        return ps_fail(W32_ERROR_TOO_MANY_OPEN_FILES);
    }
    pi->hProcess = hProc;
    pi->hThread = hThr;
    pi->processId = (W32_DWORD)pid;
    pi->threadId = (W32_DWORD)pid;      /* main thread: tid == pid */
    return 1;
}

W32ABI W32_BOOL CreateProcessW(W32_LPCWSTR app, W32_LPWSTR cmdline,
                               W32_SECURITY_ATTRIBUTES *procSa,
                               W32_SECURITY_ATTRIBUTES *threadSa,
                               W32_BOOL inherit, W32_DWORD flags,
                               void *env, W32_LPCWSTR cwd,
                               const W32_STARTUPINFOW *si,
                               W32_PROCESS_INFORMATION *pi) {
    char *app8 = NULL;
    char *cmd8 = NULL;
    char *cwd8 = NULL;
    int in = -1;
    int out = -1;
    int err = -1;
    int useStd = 0;
    W32_BOOL ok;

    (void)procSa;
    (void)threadSa;
    if (!si || !pi)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (si->cb != sizeof(W32_STARTUPINFOW))
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (app) {
        app8 = ps_w16_dup(app);
        if (!app8)
            return 0;
    }
    if (cmdline) {
        app8 = app8;            /* (kept for symmetry; freed below) */
        cmd8 = ps_w16_dup(cmdline);
        if (!cmd8) {
            free(app8);
            return 0;
        }
    }
    if (cwd) {
        cwd8 = ps_w16_dup(cwd);
        if (!cwd8) {
            free(app8);
            free(cmd8);
            return 0;
        }
    }
    if (si->flags & W32_STARTF_USESTDHANDLES) {
        useStd = 1;
        in = w32_handle_to_fd(si->hStdInput);
        out = w32_handle_to_fd(si->hStdOutput);
        err = w32_handle_to_fd(si->hStdError);
    }
    ok = ps_spawn(app8, cmd8, env, cwd8, inherit, flags, in, out, err, useStd,
                  pi);
    free(app8);
    free(cmd8);
    free(cwd8);
    return ok;
}

W32ABI W32_BOOL CreateProcessA(W32_LPCSTR app, W32_LPSTR cmdline,
                               W32_SECURITY_ATTRIBUTES *procSa,
                               W32_SECURITY_ATTRIBUTES *threadSa,
                               W32_BOOL inherit, W32_DWORD flags,
                               void *env, W32_LPCSTR cwd,
                               const W32_STARTUPINFOA *si,
                               W32_PROCESS_INFORMATION *pi) {
    int in = -1;
    int out = -1;
    int err = -1;
    int useStd = 0;
    char **envp = NULL;
    int envc = 0;
    W32_BOOL ok;

    (void)procSa;
    (void)threadSa;
    if (!si || !pi)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (si->cb != sizeof(W32_STARTUPINFOA))
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (si->flags & W32_STARTF_USESTDHANDLES) {
        useStd = 1;
        in = w32_handle_to_fd(si->hStdInput);
        out = w32_handle_to_fd(si->hStdOutput);
        err = w32_handle_to_fd(si->hStdError);
    }
    /* The A environment block is ANSI bytes; with ACP == UTF-8 it splits
     * and reuses directly. */
    if (env) {
        const char *p = (const char *)env;
        int cap = 16;
        envp = (char **)malloc((size_t)(cap + 1) * sizeof(char *));
        if (!envp)
            return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        while (*p) {
            size_t n = strlen(p);
            char *one = (char *)malloc(n + 1);
            if (!one) {
                ps_free_argv(envp, envc);
                return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
            }
            memcpy(one, p, n + 1);
            if (envc >= cap) {
                cap *= 2;
                envp = (char **)realloc(envp,
                    (size_t)(cap + 1) * sizeof(char *));
                if (!envp) {
                    free(one);
                    return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
                }
            }
            if (strchr(one, '='))
                envp[envc++] = one;
            else
                free(one);
            p += n + 1;
        }
        envp[envc] = NULL;
    }
    /* ps_spawn takes a W16 block or NULL; hand it NULL-inherit plus the
     * converted envp by stashing... simpler: ps_spawn's env path only
     * understands W16.  Convert A->W16 first. */
    if (envp) {
        /* Rebuild as a W16 block: count units first. */
        size_t total = 1;
        int i;
        W32_WCHAR *block;
        W32_WCHAR *w;
        for (i = 0; i < envc; i++)
            total += strlen(envp[i]) + 1;
        block = (W32_WCHAR *)malloc(total * sizeof(W32_WCHAR));
        if (!block) {
            ps_free_argv(envp, envc);
            return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
        }
        w = block;
        for (i = 0; i < envc; i++) {
            size_t need = 0;
            size_t need2 = 0;
            int rc = w32_utf8_to_utf16(envp[i], strlen(envp[i]), NULL, 0,
                &need);
            if (rc == W32_UTF_OK) {
                rc = w32_utf8_to_utf16(envp[i], strlen(envp[i]), w, need,
                    &need2);
                if (rc == W32_UTF_OK && need2 == need) {
                    w += need;
                    *w++ = 0;
                }
            }
        }
        *w = 0;
        ps_free_argv(envp, envc);
        ok = ps_spawn(app, cmdline, block, cwd, inherit, flags, in, out, err,
                      useStd, pi);
        free(block);
        return ok;
    }
    return ps_spawn(app, cmdline, NULL, cwd, inherit, flags, in, out, err,
                    useStd, pi);
}

W32ABI void GetStartupInfoA(W32_STARTUPINFOA *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->cb = sizeof(*out);
    out->hStdInput = GetStdHandle(W32_STD_INPUT_HANDLE);
    out->hStdOutput = GetStdHandle(W32_STD_OUTPUT_HANDLE);
    out->hStdError = GetStdHandle(W32_STD_ERROR_HANDLE);
}

W32ABI void GetStartupInfoW(W32_STARTUPINFOW *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->cb = sizeof(*out);
    out->hStdInput = GetStdHandle(W32_STD_INPUT_HANDLE);
    out->hStdOutput = GetStdHandle(W32_STD_OUTPUT_HANDLE);
    out->hStdError = GetStdHandle(W32_STD_ERROR_HANDLE);
}

static struct ps_obj *ps_proc_of(W32_HANDLE h, int want) {
    struct ps_obj *o = (struct ps_obj *)w32_handle_get_obj(h,
        W32_HANDLE_KIND_PROC);
    if (!o || o->tag != want)
        return NULL;
    return o;
}

W32ABI W32_BOOL GetExitCodeProcess(W32_HANDLE h, W32_DWORD *code) {
    struct ps_obj *o;
    int st;
    int r;

    if (!code)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (h == (W32_HANDLE)(intptr_t)-1) {
        *code = W32_STILL_ACTIVE;       /* asking about yourself: running */
        return 1;
    }
    o = ps_proc_of(h, PS_TAG_PROCESS);
    if (!o)
        return ps_fail(W32_ERROR_INVALID_HANDLE);
    ps_sweep();
    if (o->exited) {
        *code = (W32_DWORD)o->exit_code;
        return 1;
    }
    if (ps_ring_find(o->pid, &o->exit_code, NULL)) {
        o->exited = 1;
        *code = (W32_DWORD)o->exit_code;
        return 1;
    }
    r = waitpid(o->pid, &st, WNOHANG);
    if (r == o->pid) {
        if (WIFEXITED(st))
            o->exit_code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st))
            o->exit_code = 128 + WTERMSIG(st);
        else
            o->exit_code = WSTOPSIG(st);
        o->exited = 1;
        o->exit_ft = ps_now_ft();
        *code = (W32_DWORD)o->exit_code;
        return 1;
    }
    if (r < 0 && errno == ECHILD) {
        /* Not our child (OpenProcess) or reaped elsewhere: probe life. */
        if (kill(o->pid, 0) != 0 && errno == ESRCH) {
            if (ps_ring_find(o->pid, &o->exit_code, NULL)) {
                o->exited = 1;
                *code = (W32_DWORD)o->exit_code;
                return 1;
            }
            /* Dead and never seen: the code is genuinely unknown. */
            *code = 0;
            o->exited = 1;
            o->exit_code = 0;
            return 1;
        }
    }
    *code = W32_STILL_ACTIVE;
    return 1;
}

W32ABI W32_BOOL TerminateProcess(W32_HANDLE h, W32_DWORD code) {
    struct ps_obj *o;

    (void)code;                 /* SIGKILL keeps no code */
    if (h == (W32_HANDLE)(intptr_t)-1) {
        kill(getpid(), SIGKILL);
        return ps_fail(W32_ERROR_ACCESS_DENIED);    /* unreachable */
    }
    o = ps_proc_of(h, PS_TAG_PROCESS);
    if (!o)
        return ps_fail(W32_ERROR_INVALID_HANDLE);
    ps_sweep();
    if (o->exited)
        return ps_fail(W32_ERROR_ACCESS_DENIED);
    if (kill(o->pid, SIGKILL) != 0) {
        if (errno == ESRCH)
            return ps_fail(W32_ERROR_ACCESS_DENIED);
        return ps_fail(w32_error_from_c(-1));
    }
    return 1;
}

W32ABI W32_HANDLE OpenProcess(W32_DWORD access, W32_BOOL inherit,
                              W32_DWORD pid) {
    struct ps_obj *o = NULL;
    W32_HANDLE h;

    (void)access;               /* no per-right checks exist */
    (void)inherit;              /* handles do not cross exec */
    if (pid == 0)
        return NULL;
    if (kill((int)pid, 0) != 0) {
        if (errno == ESRCH) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return NULL;
        }
        w32_set_last_error(w32_error_from_c(-1));
        return NULL;
    }
    o = (struct ps_obj *)calloc(1, sizeof(*o));
    if (!o) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    o->tag = PS_TAG_PROCESS;
    o->pid = (int)pid;
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_PROC, o, ps_obj_free);
    if (!h) {
        ps_obj_free(o);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return NULL;
    }
    return h;
}

W32ABI W32_BOOL GetProcessTimes(W32_HANDLE h, W32_FILETIME *creation,
                               W32_FILETIME *exit, W32_FILETIME *kernel,
                               W32_FILETIME *user) {
    struct ps_obj *o;
    int self = 0;
    struct rusage ru;

    if (h == (W32_HANDLE)(intptr_t)-1)
        self = 1;
    else {
        o = ps_proc_of(h, PS_TAG_PROCESS);
        if (!o)
            return ps_fail(W32_ERROR_INVALID_HANDLE);
        if (o->pid == getpid())
            self = 1;
    }
    if (self) {
        if (getrusage(RUSAGE_SELF, &ru) != 0)
            return ps_fail(w32_error_from_c(-1));
        if (creation) {
            creation->dwLowDateTime = (W32_DWORD)(ps_birth_ft & 0xFFFFFFFFu);
            creation->dwHighDateTime = (W32_DWORD)(ps_birth_ft >> 32);
        }
        if (exit) {
            exit->dwLowDateTime = 0;
            exit->dwHighDateTime = 0;
        }
        if (kernel) {
            uint64_t k = ((uint64_t)(ru.ru_stime.tv_sec + 11644473600LL)) *
                10000000ULL + (uint64_t)ru.ru_stime.tv_usec * 10ULL;
            kernel->dwLowDateTime = (W32_DWORD)(k & 0xFFFFFFFFu);
            kernel->dwHighDateTime = (W32_DWORD)(k >> 32);
        }
        if (user) {
            uint64_t u = ((uint64_t)(ru.ru_utime.tv_sec + 11644473600LL)) *
                10000000ULL + (uint64_t)ru.ru_utime.tv_usec * 10ULL;
            user->dwLowDateTime = (W32_DWORD)(u & 0xFFFFFFFFu);
            user->dwHighDateTime = (W32_DWORD)(u >> 32);
        }
        return 1;
    }
    o = ps_proc_of(h, PS_TAG_PROCESS);
    if (creation) {
        creation->dwLowDateTime = (W32_DWORD)(o->birth_ft & 0xFFFFFFFFu);
        creation->dwHighDateTime = (W32_DWORD)(o->birth_ft >> 32);
    }
    if (exit) {
        W32_DWORD code;
        /* A status query refreshes the cached exit before we report it. */
        GetExitCodeProcess(h, &code);
        exit->dwLowDateTime = (W32_DWORD)(o->exit_ft & 0xFFFFFFFFu);
        exit->dwHighDateTime = (W32_DWORD)(o->exit_ft >> 32);
    }
    /* Foreign CPU clocks have no interface; zeros, documented. */
    if (kernel) {
        kernel->dwLowDateTime = 0;
        kernel->dwHighDateTime = 0;
    }
    if (user) {
        user->dwLowDateTime = 0;
        user->dwHighDateTime = 0;
    }
    return 1;
}

W32ABI W32_HANDLE GetCurrentProcess(void) {
    return (W32_HANDLE)(intptr_t)-1;
}

W32ABI W32_DWORD GetCurrentProcessId(void) {
    return (W32_DWORD)getpid();
}

/* ---- module names and handles -----------------------------------------------------
 *
 * The loader table backs every cookie: slot 0 is this process (bound at
 * init from argv[0] and the startup cwd), the rest are LoadLibrary records.
 * A cookie is the table address — opaque, stable while bound, rejected
 * after FreeLibrary clears the row.
 */

#define PS_MOD_MAX 32

static struct {
    int in_use;
    char path[4096];            /* host path, absolute */
} ps_mods[PS_MOD_MAX];

static void ps_mods_init_self(void) {
    static int done;
    const char *a0;
    if (done)
        return;
    done = 1;
    ps_mods[0].in_use = 1;
    a0 = (ps_argc_saved > 0 && ps_argv_saved && ps_argv_saved[0]) ?
        ps_argv_saved[0] : "/apps/w32run";
    if (a0[0] == '/') {
        size_t n = strlen(a0);
        if (n >= sizeof(ps_mods[0].path))
            n = sizeof(ps_mods[0].path) - 1;
        memcpy(ps_mods[0].path, a0, n);
        ps_mods[0].path[n] = '\0';
    } else {
        size_t c = strlen(ps_startup_cwd);
        size_t n = strlen(a0);
        if (c + 1 + n >= sizeof(ps_mods[0].path))
            n = sizeof(ps_mods[0].path) - c - 2;
        memcpy(ps_mods[0].path, ps_startup_cwd, c);
        ps_mods[0].path[c] = '/';
        memcpy(ps_mods[0].path + c + 1, a0, n);
        ps_mods[0].path[c + 1 + n] = '\0';
    }
}

static void *ps_mod_cookie(int slot) {
    return (void *)(uintptr_t)(0x4D000000u + (unsigned)slot);
}

static int ps_mod_slot(const void *cookie) {
    uintptr_t v = (uintptr_t)cookie;
    if (v < 0x4D000000u || v >= 0x4D000000u + PS_MOD_MAX)
        return -1;
    return (int)(v - 0x4D000000u);
}

/* Host absolute path -> "C:\..." DOS spelling. */
static void ps_dos_path(const char *host, char *dos, size_t cap) {
    size_t i = 0;
    size_t o = 0;
    if (cap < 4)
        return;
    dos[o++] = 'C';
    dos[o++] = ':';
    if (host[0] != '/') {
        dos[o++] = '\\';
    }
    for (i = 0; host[i] != '\0' && o + 1 < cap; i++)
        dos[o++] = (host[i] == '/') ? '\\' : host[i];
    dos[o] = '\0';
}

W32ABI W32_DWORD GetModuleFileNameA(W32_HANDLE mod, W32_LPSTR buf,
                                    W32_DWORD cch) {
    const char *host;
    char dos[4100];
    size_t n;
    int slot;

    ps_mods_init_self();
    if (!buf || cch == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!mod) {
        host = ps_mods[0].path;
    } else {
        slot = ps_mod_slot(mod);
        if (slot < 0 || !ps_mods[slot].in_use) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return 0;
        }
        host = ps_mods[slot].path;
    }
    ps_dos_path(host, dos, sizeof(dos));
    n = strlen(dos);
    if (n >= cch) {
        memcpy(buf, dos, cch - 1);
        buf[cch - 1] = '\0';
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return cch;
    }
    memcpy(buf, dos, n + 1);
    return (W32_DWORD)n;
}

W32ABI W32_DWORD GetModuleFileNameW(W32_HANDLE mod, W32_LPWSTR buf,
                                    W32_DWORD cch) {
    const char *host;
    char dos[4100];
    W32_DWORD need;
    int slot;

    ps_mods_init_self();
    if (!buf || cch == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!mod) {
        host = ps_mods[0].path;
    } else {
        slot = ps_mod_slot(mod);
        if (slot < 0 || !ps_mods[slot].in_use) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return 0;
        }
        host = ps_mods[slot].path;
    }
    ps_dos_path(host, dos, sizeof(dos));
    need = ps_copy_out(dos, buf, cch);
    if (need == 0)
        return 0;
    if (need > cch) {
        /* Truncated with NUL (ps_copy_out wrote nothing): redo short. */
        size_t n = strlen(dos);
        size_t take = (cch > 0) ? cch - 1 : 0;
        size_t i;
        if (take > n)
            take = n;
        for (i = 0; i < take; i++)
            buf[i] = (W32_WCHAR)(unsigned char)dos[i];
        if (cch > 0)
            buf[take] = 0;
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return cch;
    }
    return need - 1;
}

static int ps_basename_ci_equal(const char *path, const char *name) {
    const char *b = strrchr(path, '/');
    size_t i;
    b = b ? b + 1 : path;
    for (i = 0; ; i++) {
        unsigned char a = (unsigned char)b[i];
        unsigned char c = (unsigned char)name[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a - 'A' + 'a');
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (a != c)
            return 0;
        if (a == '\0')
            return 1;
    }
}

W32ABI void *GetModuleHandleW(W32_LPCWSTR name) {
    char *n8 = NULL;
    int i;

    ps_mods_init_self();
    if (!name)
        return ps_mod_cookie(0);
    n8 = ps_w16_dup(name);
    if (!n8)
        return NULL;
    if (n8[0] == '\0') {
        free(n8);
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    for (i = 0; i < PS_MOD_MAX; i++) {
        if (ps_mods[i].in_use && ps_basename_ci_equal(ps_mods[i].path, n8)) {
            free(n8);
            return ps_mod_cookie(i);
        }
    }
#ifndef AURALITE_W32_HOST_TEST
    /* Loader modules (builtins, mapped DLLs) live in the other table; a
     * real program asks for them by W name.  The host suite pins the
     * ps-table miss above, the guest fixture pins this arm. */
    {
        void *lh = w32_GetModuleHandleA(n8);
        if (lh) {
            free(n8);
            return lh;
        }
    }
#endif
    free(n8);
    w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
    return NULL;
}

W32ABI W32_BOOL GetModuleHandleExW(W32_DWORD flags, const void *nameOrAddr,
                                    void **modOut) {
    if (!modOut)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    *modOut = NULL;
    if (flags & ~(W32_GET_MODULE_HANDLE_EX_FLAG_PIN |
                  W32_GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                  W32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    ps_mods_init_self();
    if (flags & W32_GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        /* One module owns every address in this process. */
        if (!nameOrAddr)
            return ps_fail(W32_ERROR_INVALID_PARAMETER);
        *modOut = ps_mod_cookie(0);
        return 1;
    }
    /* PIN and UNCHANGED_REFCOUNT are no-ops: loads are permanent table
     * rows, never refcounted. */
    *modOut = GetModuleHandleW((W32_LPCWSTR)nameOrAddr);
    return (*modOut != NULL);
}

W32ABI W32_LPCWSTR GetCommandLineW(void) {
    if (!ps_cmdline_w) {
        /* Init never ran (a host-test harness, usually): the empty line. */
        static W32_WCHAR empty;
        return &empty;
    }
    return ps_cmdline_w;
}

/* ---- environment ---------------------------------------------------------------------- */

/* Live environment blocks.  FreeEnvironmentStrings consults this table
 * instead of reading a magic through the caller's pointer, so a stale or
 * foreign pointer is refused without touching (possibly freed) memory. */
#define PS_ENV_MAX 16
static void *ps_env_live[PS_ENV_MAX];

W32ABI W32_LPWSTR GetEnvironmentStringsW(void) {
    char **e = environ;
    size_t total = 1;
    W32_WCHAR *block;
    W32_WCHAR *w;
    size_t i;

    if (!e)
        e = NULL;
    for (; e && *e; e++)
        total += strlen(*e) + 1;
    block = (W32_WCHAR *)malloc(total * sizeof(W32_WCHAR));
    if (!block) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    for (i = 0; i < PS_ENV_MAX; i++) {
        if (!ps_env_live[i]) {
            ps_env_live[i] = block;
            break;
        }
    }
    if (i == PS_ENV_MAX) {
        free(block);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return NULL;
    }
    w = block;
    for (e = environ; e && *e; e++) {
        size_t need = 0;
        size_t need2 = 0;
        int rc = w32_utf8_to_utf16(*e, strlen(*e), NULL, 0, &need);
        if (rc != W32_UTF_OK)
            continue;           /* unrepresentable entry: skipped */
        rc = w32_utf8_to_utf16(*e, strlen(*e), w, need, &need2);
        if (rc != W32_UTF_OK || need2 != need)
            continue;
        w += need;
        *w++ = 0;
    }
    *w = 0;
    return (W32_LPWSTR)block;
}

W32ABI W32_BOOL FreeEnvironmentStringsW(W32_LPWSTR block) {
    size_t i;
    if (!block)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    for (i = 0; i < PS_ENV_MAX; i++) {
        if (ps_env_live[i] == (void *)block) {
            ps_env_live[i] = NULL;
            free(block);
            return 1;
        }
    }
    return ps_fail(W32_ERROR_INVALID_PARAMETER);
}

W32ABI W32_DWORD GetEnvironmentVariableA(W32_LPCSTR name, W32_LPSTR buf,
                                         W32_DWORD cch) {
    const char *v;
    size_t n;

    if (!name || !name[0])
        return 0;
    v = getenv(name);           /* ACP is UTF-8: direct */
    if (!v) {
        w32_set_last_error(W32_ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    n = strlen(v);
    if (cch == 0 || !buf)
        return (W32_DWORD)(n + 1);
    if (n >= cch) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return (W32_DWORD)(n + 1);
    }
    memcpy(buf, v, n + 1);
    return (W32_DWORD)n;
}

W32ABI W32_BOOL SetEnvironmentVariableW(W32_LPCWSTR name, W32_LPCWSTR value) {
    char *n8 = NULL;
    char *v8 = NULL;
    int rc;

    n8 = ps_w16_dup(name);
    if (!n8)
        return 0;
    if (n8[0] == '\0' || strchr(n8, '=')) {
        free(n8);
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    }
    if (!value) {
        rc = unsetenv(n8);
        free(n8);
        if (rc != 0)
            return ps_fail(w32_error_from_c(-1));
        return 1;
    }
    v8 = ps_w16_dup(value);
    if (!v8) {
        free(n8);
        return 0;
    }
    rc = setenv(n8, v8, 1);
    free(n8);
    free(v8);
    if (rc != 0)
        return ps_fail(w32_error_from_c(-1));
    return 1;
}

W32ABI W32_DWORD ExpandEnvironmentStringsW(W32_LPCWSTR src, W32_LPWSTR dst,
                                           W32_DWORD cch) {
    char *s8 = NULL;
    size_t cap = 256;
    size_t len = 0;
    char *out = NULL;
    const char *p;
    W32_DWORD need;

    s8 = ps_w16_dup(src);
    if (!s8)
        return 0;
    out = (char *)malloc(cap);
    if (!out) {
        free(s8);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    p = s8;
    while (*p) {
        if (*p == '%') {
            const char *end = strchr(p + 1, '%');
            if (end && end != p + 1) {
                char var[256];
                size_t vn = (size_t)(end - (p + 1));
                const char *v;
                if (vn >= sizeof(var))
                    vn = sizeof(var) - 1;
                memcpy(var, p + 1, vn);
                var[vn] = '\0';
                v = getenv(var);
                if (v) {
                    size_t vl = strlen(v);
                    while (len + vl + 1 > cap) {
                        cap *= 2;
                        out = (char *)realloc(out, cap);
                        if (!out) {
                            free(s8);
                            w32_set_last_error(
                                W32_ERROR_NOT_ENOUGH_MEMORY);
                            return 0;
                        }
                    }
                    memcpy(out + len, v, vl);
                    len += vl;
                    p = end + 1;
                    continue;
                }
                /* Unknown variable: kept literally, as on Windows. */
            } else if (end) {
                /* "%%": a literal percent. */
                p++;
                if (len + 2 > cap) {
                    cap *= 2;
                    out = (char *)realloc(out, cap);
                    if (!out) {
                        free(s8);
                        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                        return 0;
                    }
                }
                out[len++] = '%';
                p++;
                continue;
            }
        }
        if (len + 2 > cap) {
            cap *= 2;
            out = (char *)realloc(out, cap);
            if (!out) {
                free(s8);
                w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
        }
        out[len++] = *p++;
    }
    out[len] = '\0';
    free(s8);
    need = ps_copy_out(out, dst, cch);
    free(out);
    if (need == 0)
        return 0;
    return need - 1;
}

/* ---- directories -------------------------------------------------------------------------- */

W32ABI W32_DWORD GetCurrentDirectoryA(W32_DWORD cch, W32_LPSTR buf) {
    char cwd[4096];
    char dos[4100];
    size_t n;

    if (!getcwd(cwd, sizeof(cwd))) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    ps_dos_path(cwd, dos, sizeof(dos));
    n = strlen(dos);
    if (cch == 0 || !buf)
        return (W32_DWORD)(n + 1);
    if (n >= cch)
        return (W32_DWORD)(n + 1);
    memcpy(buf, dos, n + 1);
    return (W32_DWORD)n;
}

W32ABI W32_DWORD GetCurrentDirectoryW(W32_DWORD cch, W32_LPWSTR buf) {
    char cwd[4096];
    char dos[4100];
    W32_DWORD need;

    if (!getcwd(cwd, sizeof(cwd))) {
        w32_set_last_error(w32_error_from_c(-1));
        return 0;
    }
    ps_dos_path(cwd, dos, sizeof(dos));
    need = ps_copy_out(dos, buf, cch);
    if (need == 0)
        return 0;
    if (need > cch)
        return need;
    return need - 1;
}

static W32_BOOL ps_set_cwd(const char *utf8) {
    size_t n = strlen(utf8);
    size_t i;
    char *host;
    struct stat st;

    host = (char *)malloc(n + 1);
    if (!host)
        return ps_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
    for (i = 0; i < n; i++)
        host[i] = (utf8[i] == '\\') ? '/' : utf8[i];
    host[n] = '\0';
    if (((host[0] >= 'A' && host[0] <= 'Z') ||
         (host[0] >= 'a' && host[0] <= 'z')) && host[1] == ':') {
        if (host[0] != 'C' && host[0] != 'c') {
            free(host);
            return ps_fail(W32_ERROR_PATH_NOT_FOUND);
        }
        memmove(host, host + 2, n - 1);
    }
    if (stat(host, &st) != 0) {
        free(host);
        return ps_fail(W32_ERROR_PATH_NOT_FOUND);
    }
    if (!S_ISDIR(st.st_mode)) {
        free(host);
        return ps_fail(W32_ERROR_DIRECTORY);
    }
    if (chdir(host) != 0) {
        W32_DWORD code = w32_error_from_c(-1);
        free(host);
        w32_set_last_error(code);
        return 0;
    }
    free(host);
    return 1;
}

W32ABI W32_BOOL SetCurrentDirectoryA(W32_LPCSTR path) {
    if (!path)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    return ps_set_cwd(path);
}

W32ABI W32_BOOL SetCurrentDirectoryW(W32_LPCWSTR path) {
    char *u8 = ps_w16_dup(path);
    W32_BOOL ok;
    if (!u8)
        return 0;
    ok = ps_set_cwd(u8);
    free(u8);
    return ok;
}

static const char *ps_temp_dir(void) {
    const char *t = getenv("TMPDIR");
    if (!t)
        t = getenv("TEMP");
    if (!t)
        t = getenv("TMP");
    if (!t || !t[0])
        t = "/tmp";
    return t;
}

W32ABI W32_DWORD GetTempPathA(W32_DWORD cch, W32_LPSTR buf) {
    char dos[4100];
    size_t n;

    ps_dos_path(ps_temp_dir(), dos, sizeof(dos));
    n = strlen(dos);
    if (n == 0 || dos[n - 1] != '\\') {
        if (n + 1 < sizeof(dos)) {
            dos[n++] = '\\';
            dos[n] = '\0';
        }
    }
    if (cch == 0 || !buf)
        return (W32_DWORD)(n + 1);
    if (n >= cch)
        return (W32_DWORD)(n + 1);
    memcpy(buf, dos, n + 1);
    return (W32_DWORD)n;
}

W32ABI W32_DWORD GetTempPathW(W32_DWORD cch, W32_LPWSTR buf) {
    char dos[4100];
    size_t n;
    W32_DWORD need;

    ps_dos_path(ps_temp_dir(), dos, sizeof(dos));
    n = strlen(dos);
    if (n == 0 || dos[n - 1] != '\\') {
        if (n + 1 < sizeof(dos)) {
            dos[n++] = '\\';
            dos[n] = '\0';
        }
    }
    need = ps_copy_out(dos, buf, cch);
    if (need == 0)
        return 0;
    if (need > cch)
        return need;
    return need - 1;
}

/* No Windows directory exists; the conventional spellings keep path-joining
 * callers (fonts, drivers, system32) producing well-formed names. */
W32ABI W32_UINT GetWindowsDirectoryA(W32_LPSTR buf, W32_UINT cch) {
    static const char win[] = "C:\\win";
    size_t n = sizeof(win) - 1;
    if (cch == 0 || !buf)
        return (W32_UINT)(n + 1);
    if (n >= cch)
        return (W32_UINT)(n + 1);
    memcpy(buf, win, n + 1);
    return (W32_UINT)n;
}

W32ABI W32_UINT GetWindowsDirectoryW(W32_LPWSTR buf, W32_UINT cch) {
    static const char win[] = "C:\\win";
    W32_DWORD need = ps_copy_out(win, buf, cch);
    if (need == 0)
        return 0;
    if (need > cch)
        return need;
    return need - 1;
}

W32ABI W32_UINT GetSystemDirectoryA(W32_LPSTR buf, W32_UINT cch) {
    static const char sys[] = "C:\\win\\sys";
    size_t n = sizeof(sys) - 1;
    if (cch == 0 || !buf)
        return (W32_UINT)(n + 1);
    if (n >= cch)
        return (W32_UINT)(n + 1);
    memcpy(buf, sys, n + 1);
    return (W32_UINT)n;
}

/* ---- version ------------------------------------------------------------------------------ */

W32ABI W32_DWORD GetVersion(void) {
    return (PS_VER_BUILD << 16) | (PS_VER_MINOR << 8) | PS_VER_MAJOR;
}

W32ABI W32_BOOL GetVersionExW(W32_OSVERSIONINFOEXW *out) {
    if (!out)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (out->size != 276u && out->size != sizeof(W32_OSVERSIONINFOEXW))
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    {
        W32_DWORD size = out->size;
        memset(out, 0, size < sizeof(W32_OSVERSIONINFOEXW) ? size :
            sizeof(W32_OSVERSIONINFOEXW));
        out->size = size;
        out->major = PS_VER_MAJOR;
        out->minor = PS_VER_MINOR;
        out->build = PS_VER_BUILD;
        out->platformId = W32_VER_PLATFORM_WIN32_NT;
        if (size >= sizeof(W32_OSVERSIONINFOEXW)) {
            out->servicePackMajor = 0;
            out->servicePackMinor = 0;
            out->suiteMask = 0;
            out->productType = 1;       /* VER_NT_WORKSTATION */
        }
    }
    return 1;
}

W32ABI W32_BOOL GetProductInfo(W32_DWORD major, W32_DWORD minor,
                               W32_DWORD spMajor, W32_DWORD spMinor,
                               W32_DWORD *type) {
    if (!type)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (major != PS_VER_MAJOR || minor != PS_VER_MINOR || spMajor != 0 ||
        spMinor != 0)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    *type = W32_PRODUCT_PROFESSIONAL;
    return 1;
}

W32ABI W32_BOOL GetProcessAffinityMask(W32_HANDLE h, W32_DWORD_PTR *procMask,
                                       W32_DWORD_PTR *sysMask) {
    long n;
    W32_DWORD_PTR mask;

    if (h != (W32_HANDLE)(intptr_t)-1 && !ps_proc_of(h, PS_TAG_PROCESS) &&
        !ps_proc_of(h, PS_TAG_THREAD))
        return ps_fail(W32_ERROR_INVALID_HANDLE);
    if (!procMask || !sysMask)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    /* No per-process affinity API exists, so the process mask IS the system
     * mask: every online CPU. */
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0)
        n = 1;
    if (n >= 64)
        mask = ~0ULL;
    else
        mask = (n == 64) ? ~0ULL : ((1ULL << n) - 1u);
    *procMask = mask;
    *sysMask = mask;
    return 1;
}

/* ---- toolhelp: the process list ---------------------------------------------------------------
 *
 * The snapshot reads /proc once (numeric entries, status for Name/PPid,
 * cmdline for argv[0]) and sorts by pid, so Process32First/Next walk a
 * frozen, deterministic array.  Thread counts are 1 (no thread census),
 * priorities are 8/NORMAL (no scheduler API) — both documented, neither
 * measured.
 */

static int ps_cmp_pid(const void *a, const void *b) {
    const struct ps_snap_entry *ea = (const struct ps_snap_entry *)a;
    const struct ps_snap_entry *eb = (const struct ps_snap_entry *)b;
    if (ea->pid < eb->pid)
        return -1;
    if (ea->pid > eb->pid)
        return 1;
    return 0;
}

static int ps_all_digits(const char *s) {
    size_t i;
    if (s[0] == '\0')
        return 0;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
    }
    return 1;
}

W32ABI W32_HANDLE CreateToolhelp32Snapshot(W32_DWORD flags, W32_DWORD pid) {
    DIR *d;
    struct dirent *de;
    struct ps_snap_entry *arr = NULL;
    size_t cap = 64;
    size_t n = 0;
    struct ps_obj *o = NULL;
    W32_HANDLE h;

    if (flags != W32_TH32CS_SNAPPROCESS || pid != 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    d = opendir("/proc");
    if (!d) {
        w32_set_last_error(w32_error_from_c(-1));
        return (W32_HANDLE)-1;  /* INVALID_HANDLE_VALUE, not NULL */
    }
    arr = (struct ps_snap_entry *)malloc(cap * sizeof(*arr));
    if (!arr) {
        closedir(d);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return (W32_HANDLE)-1;
    }
    while ((de = readdir(d)) != NULL) {
        char spath[80];
        char buf[1024];
        FILE *f;
        size_t got;
        char *nl;
        long q;
        if (!ps_all_digits(de->d_name))
            continue;
        q = strtol(de->d_name, NULL, 10);
        if (q <= 0 || q > 0x7FFFFFFF)
            continue;
        if (n >= cap && cap < 512) {
            struct ps_snap_entry *narr;
            cap *= 2;
            narr = (struct ps_snap_entry *)realloc(arr,
                cap * sizeof(*arr));
            if (!narr)
                break;
            arr = narr;
        }
        if (n >= cap)
            break;              /* 512 processes: beyond any fixture */
        memset(&arr[n], 0, sizeof(arr[n]));
        arr[n].pid = (W32_DWORD)q;
        /* Name + PPid from status. */
        snprintf(spath, sizeof(spath), "/proc/%ld/status", q);
        f = fopen(spath, "r");
        if (f) {
            got = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[got] = '\0';
            {
                char *name = strstr(buf, "Name:");
                if (name) {
                    size_t k = 0;
                    name += 5;
                    while (*name == ' ' || *name == '\t')
                        name++;
                    while (name[k] && name[k] != '\n' &&
                           k + 1 < sizeof(arr[n].exe)) {
                        arr[n].exe[k] = name[k];
                        k++;
                    }
                    arr[n].exe[k] = '\0';
                }
                {
                    char *pp = strstr(buf, "PPid:");
                    if (pp)
                        arr[n].ppid = (W32_DWORD)strtol(pp + 5, NULL, 10);
                }
            }
        }
        /* argv[0] from cmdline wins over the short Name when present. */
        snprintf(spath, sizeof(spath), "/proc/%ld/cmdline", q);
        f = fopen(spath, "rb");
        if (f) {
            got = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            if (got > 0) {
                size_t k = 0;
                buf[got] = '\0';
                while (buf[k] && k + 1 < sizeof(arr[n].exe)) {
                    arr[n].exe[k] = buf[k];
                    k++;
                }
                arr[n].exe[k] = '\0';
            }
        }
        (void)nl;
        n++;
    }
    closedir(d);
    qsort(arr, n, sizeof(*arr), ps_cmp_pid);
    o = (struct ps_obj *)calloc(1, sizeof(*o));
    if (!o) {
        free(arr);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return (W32_HANDLE)-1;
    }
    o->tag = PS_TAG_SNAP;
    o->snap = arr;
    o->snap_n = n;
    o->snap_i = 0;
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_PROC, o, ps_obj_free);
    if (!h) {
        ps_obj_free(o);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return (W32_HANDLE)-1;
    }
    return h;
}

static W32_BOOL ps_process_entry(struct ps_obj *o, W32_PROCESSENTRY32W *out) {
    struct ps_snap_entry *e;
    size_t need = 0;
    size_t need2 = 0;
    size_t n;
    size_t i;
    int rc;

    if (o->snap_i >= o->snap_n)
        return ps_fail(W32_ERROR_NO_MORE_FILES);
    e = &o->snap[o->snap_i];
    memset(out, 0, sizeof(*out));
    out->size = sizeof(W32_PROCESSENTRY32W);
    out->processId = e->pid;
    out->cntThreads = 1;
    out->parentProcessId = e->ppid;
    out->basePriority = 8;
    n = strlen(e->exe);
    rc = w32_utf8_to_utf16(e->exe, n, NULL, 0, &need);
    if (rc == W32_UTF_OK && need < 260) {
        rc = w32_utf8_to_utf16(e->exe, n, out->exeFile, need, &need2);
        if (rc == W32_UTF_OK && need2 == need)
            out->exeFile[need] = 0;
    } else {
        /* Unrepresentable bytes: ASCII-fallback, never garbage. */
        for (i = 0; i < n && i < 259; i++) {
            unsigned char c = (unsigned char)e->exe[i];
            out->exeFile[i] = (c < 0x80) ? (W32_WCHAR)c : (W32_WCHAR)'?';
        }
        out->exeFile[i] = 0;
    }
    o->snap_i++;
    return 1;
}

W32ABI W32_BOOL Process32FirstW(W32_HANDLE snap, W32_PROCESSENTRY32W *out) {
    struct ps_obj *o;
    if (!out || out->size != sizeof(W32_PROCESSENTRY32W))
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    o = ps_proc_of(snap, PS_TAG_SNAP);
    if (!o)
        return ps_fail(W32_ERROR_INVALID_HANDLE);
    o->snap_i = 0;
    return ps_process_entry(o, out);
}

W32ABI W32_BOOL Process32NextW(W32_HANDLE snap, W32_PROCESSENTRY32W *out) {
    struct ps_obj *o;
    if (!out || out->size != sizeof(W32_PROCESSENTRY32W))
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    o = ps_proc_of(snap, PS_TAG_SNAP);
    if (!o)
        return ps_fail(W32_ERROR_INVALID_HANDLE);
    return ps_process_entry(o, out);
}

/* ---- machine ------------------------------------------------------------------------------ */

W32ABI W32_BOOL IsDebuggerPresent(void) {
    return 0;                   /* no debugger port exists to be present on */
}

W32ABI W32_BOOL IsProcessorFeaturePresent(W32_DWORD feature) {
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;

    switch (feature) {
    case W32_PF_FLOATING_POINT_PRECISION_ERRATA:
    case W32_PF_FLOATING_POINT_EMULATED:
        return 0;
    case W32_PF_COMPARE_EXCHANGE_DOUBLE:        /* CX8 */
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 8)) != 0;
    case W32_PF_MMX_INSTRUCTIONS_AVAILABLE:
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 23)) != 0;
    case W32_PF_XMMI_INSTRUCTIONS_AVAILABLE:    /* SSE */
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 25)) != 0;
    case W32_PF_XMMI64_INSTRUCTIONS_AVAILABLE:  /* SSE2 */
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 26)) != 0;
    case W32_PF_SSE3_INSTRUCTIONS_AVAILABLE:
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (c & (1u << 0)) != 0;
    case W32_PF_SSSE3_INSTRUCTIONS_AVAILABLE:
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (c & (1u << 9)) != 0;
    case W32_PF_SSE4_1_INSTRUCTIONS_AVAILABLE:
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (c & (1u << 19)) != 0;
    case W32_PF_SSE4_2_INSTRUCTIONS_AVAILABLE:
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (c & (1u << 20)) != 0;
    case W32_PF_AVX_INSTRUCTIONS_AVAILABLE:
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (c & (1u << 28)) != 0;
    case W32_PF_RDTSC_INSTRUCTION_AVAILABLE:
        if (!__get_cpuid(1, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 4)) != 0;
    case W32_PF_RDTSCP_INSTRUCTION_AVAILABLE:
        if (!__get_cpuid(0x80000001u, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 27)) != 0;
    case W32_PF_NX_ENABLED:
        if (!__get_cpuid(0x80000001u, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 20)) != 0;
    case W32_PF_3DNOW_INSTRUCTIONS_AVAILABLE:
        if (!__get_cpuid(0x80000001u, &a, &b, &c, &d))
            return 0;
        return (d & (1u << 31)) != 0;
    default:
        return 0;
    }
}

static void ps_fill_sysinfo(W32_SYSTEM_INFO *out) {
    long n;
    long pg;
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;

    memset(out, 0, sizeof(*out));
    out->arch = W32_PROCESSOR_ARCHITECTURE_AMD64;
    pg = sysconf(_SC_PAGESIZE);
    out->pageSize = (pg > 0) ? (W32_DWORD)pg : 4096u;
    out->minAppAddr = (void *)(uintptr_t)0x10000;
    out->maxAppAddr = (void *)(uintptr_t)0x00007FFFFFFFFFFFULL;
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0)
        n = 1;
    out->nProcessors = (W32_DWORD)n;
    if (n >= 64)
        out->activeMask = ~0ULL;
    else
        out->activeMask = (1ULL << n) - 1u;
    out->processorType = 8664u;   /* PROCESSOR_AMD_X8664 */
    out->allocGranularity = 65536u;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        unsigned fam = ((a >> 8) & 0xFu) + ((a >> 20) & 0xFFu);
        unsigned mod = ((a >> 4) & 0xFu) | ((a >> 12) & 0xF0u);
        out->processorLevel = (W32_WORD)fam;
        out->processorRevision = (W32_WORD)((mod << 8) | (a & 0xFu));
    } else {
        out->processorLevel = 6;
        out->processorRevision = 0;
    }
}

W32ABI void GetNativeSystemInfo(W32_SYSTEM_INFO *out) {
    if (!out)
        return;
    ps_fill_sysinfo(out);
}

W32ABI void GetSystemInfo(W32_SYSTEM_INFO *out) {
    /* No WOW64 exists: native and emulated are the same call. */
    GetNativeSystemInfo(out);
}

/* ---- small processes ---------------------------------------------------------------------------- */

W32ABI W32_BOOL Beep(W32_DWORD freq, W32_DWORD dur) {
    int fd;
    char msg[64];
    int n;

    if (freq < 37u || freq > 32767u)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (dur == 0)
        return 1;
    fd = open("/dev/audio", O_WRONLY);
    if (fd < 0)
        return 1;               /* no sounder: silent success */
    n = snprintf(msg, sizeof(msg), "BEEP %lu %lu", (unsigned long)freq,
        (unsigned long)dur);
    if (n > 0)
        write(fd, msg, (size_t)n);
    close(fd);
    return 1;
}

W32ABI W32_DWORD SleepEx(W32_DWORD ms, W32_BOOL alertable) {
    (void)alertable;            /* no APCs exist to alert with */
    Sleep(ms);
    return 0;
}

W32ABI void OutputDebugStringW(W32_LPCWSTR str) {
    char *u8;
    if (!str)
        return;
    u8 = ps_w16_dup(str);
    if (!u8)
        return;
    /* Debug output lands on stderr — the channel a developer watches. */
    write(2, u8, strlen(u8));
    write(2, "\n", 1);
    free(u8);
}

/* ---- restart registration: one honest slot ------------------------------------------------------------
 *
 * RegisterApplicationRestart records the command line and flags; the
 * registration is real within the process (Get returns it, Unregister
 * clears it) and aspirational outside it — no restart manager reads the
 * slot.  That boundary is documented, not blurred: the APIs round-trip.
 */

static struct {
    int registered;
    W32_WCHAR cmdline[2048];
    W32_DWORD flags;
} ps_restart;

W32ABI W32_HRESULT RegisterApplicationRestart(W32_LPCWSTR cmdline,
                                              W32_DWORD flags) {
    size_t n;
    size_t i;
    if (flags & ~0xFu)
        return (W32_HRESULT)0x80070057u;         /* E_INVALIDARG */
    if (!cmdline)
        return (W32_HRESULT)0x80070057u;
    n = w32_utf16_len(cmdline, 2048);
    if (n >= 2048)
        return (W32_HRESULT)0x80070057u;
    for (i = 0; i <= n; i++)
        ps_restart.cmdline[i] = cmdline[i];
    ps_restart.flags = flags;
    ps_restart.registered = 1;
    return 0;
}

W32ABI W32_HRESULT UnregisterApplicationRestart(void) {
    ps_restart.registered = 0;
    ps_restart.cmdline[0] = 0;
    ps_restart.flags = 0;
    return 0;
}

W32ABI W32_HRESULT GetApplicationRestartSettings(W32_LPWSTR cmdline,
                                                W32_DWORD *cch,
                                                W32_DWORD *flags) {
    size_t n;
    size_t i;
    if (!ps_restart.registered)
        return (W32_HRESULT)0x80004005u;         /* E_FAIL */
    if (!cch)
        return (W32_HRESULT)0x80070057u;
    n = w32_utf16_len(ps_restart.cmdline, 2048);
    if (cmdline && *cch > n) {
        for (i = 0; i <= n; i++)
            cmdline[i] = ps_restart.cmdline[i];
        if (flags)
            *flags = ps_restart.flags;
        *cch = (W32_DWORD)(n + 1);
        return 0;
    }
    *cch = (W32_DWORD)(n + 1);
    return (W32_HRESULT)0x8007017Au;             /* HRESULT(122) */
}

W32ABI W32_INT MulDiv(W32_INT a, W32_INT b, W32_INT c) {
    int64_t r;
    if (c == 0)
        return -1;
    r = (int64_t)a * (int64_t)b / (int64_t)c;
    if (r > 2147483647LL || r < -2147483648LL)
        return -1;
    return (W32_INT)r;
}

W32ABI void *EncodePointer(void *p) {
    uintptr_t v = (uintptr_t)p;
    if (!p)
        return NULL;
    return (void *)(v ^ (uintptr_t)ps_encode_cookie);
}

W32ABI void *DecodePointer(void *p) {
    uintptr_t v = (uintptr_t)p;
    if (!p)
        return NULL;
    return (void *)(v ^ (uintptr_t)ps_encode_cookie);
}

/* ---- memory census: the refused measurement ---------------------------------------------------------------
 *
 * Total physical pages are countable (sysconf); AVAILABLE pages are not —
 * no userspace interface reports them.  The Ex form refuses rather than
 * estimate; the void form (which cannot refuse) reports zeros, which is
 * the safe under-report: callers see "no memory known" instead of a
 * fabricated plenty.  Both choices are documented, neither is silent.
 */

W32ABI void GlobalMemoryStatus(W32_MEMORYSTATUS *out) {
    size_t n;
    /* The void form cannot refuse: fill what the caller's length covers
     * (a real binary passes 40) and report zeros, the safe under-report. */
    if (!out || out->len < sizeof(out->len))
        return;
    n = out->len < sizeof(*out) ? out->len : sizeof(*out);
    memset(out, 0, n);
    out->len = sizeof(*out);
}

W32ABI W32_BOOL GlobalMemoryStatusEx(W32_MEMORYSTATUSEX *out) {
    if (!out || out->len < sizeof(*out))
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    return ps_fail(W32_ERROR_NOT_SUPPORTED);
}

/* ---- the loader that is a table -----------------------------------------------------------------------
 *
 * LoadLibrary resolves a name (as-given, PATH, .dll appended — the
 * ALTERED_SEARCH_PATH arm resolves against the caller's directory instead
 * of the PATH), records the host path, and returns a cookie.  Nothing is
 * mapped: there is no dynamic loader beneath, and claiming otherwise would
 * be the lie this file refuses to tell.  DONT_RESOLVE and AS_DATAFILE are
 * honoured trivially (nothing resolves, everything is data); every other
 * flag combination is refused per-flag.
 */

static char *ps_resolve_dll(const char *name, int altered, const char *callerDir) {
    struct stat st;
    size_t n;
    char *cand;
    const char *path;
    char *copy;
    char *dir;

    n = strlen(name);
    if (n == 0 || n > 4000)
        return NULL;
    /* As-given (folded), then with .dll. */
    cand = (char *)malloc(n + 5);
    if (!cand)
        return NULL;
    {
        size_t i;
        for (i = 0; i < n; i++)
            cand[i] = (name[i] == '\\') ? '/' : name[i];
        cand[n] = '\0';
    }
    /* A drive prefix ("C:/x") roots at "/x". */
    if (((cand[0] >= 'A' && cand[0] <= 'Z') ||
         (cand[0] >= 'a' && cand[0] <= 'z')) && cand[1] == ':') {
        if (cand[0] != 'C' && cand[0] != 'c') {
            free(cand);
            return NULL;
        }
        memmove(cand, cand + 2, n - 1);
        n -= 2;
    }
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode))
        return cand;
    memcpy(cand + n, ".dll", 5);
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode))
        return cand;
    free(cand);
    if (strchr(name, '/') || strchr(name, '\\'))
        return NULL;
    /* Search: the caller's directory (altered) or the PATH. */
    if (altered && callerDir && callerDir[0]) {
        size_t dl = strlen(callerDir);
        cand = (char *)malloc(dl + 1 + n + 5);
        if (!cand)
            return NULL;
        memcpy(cand, callerDir, dl);
        cand[dl] = '/';
        memcpy(cand + dl + 1, name, n + 1);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode))
            return cand;
        memcpy(cand + dl + 1 + n, ".dll", 5);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode))
            return cand;
        free(cand);
        return NULL;
    }
    path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin:/apps";
    copy = (char *)malloc(strlen(path) + 1);
    if (!copy)
        return NULL;
    strcpy(copy, path);
    dir = copy;
    for (;;) {
        char *sep = strchr(dir, ':');
        size_t dl;
        if (sep)
            *sep = '\0';
        dl = strlen(dir);
        cand = (char *)malloc(dl + 1 + n + 5);
        if (!cand) {
            free(copy);
            return NULL;
        }
        memcpy(cand, dir, dl);
        cand[dl] = '/';
        memcpy(cand + dl + 1, name, n + 1);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
            free(copy);
            return cand;
        }
        memcpy(cand + dl + 1 + n, ".dll", 5);
        if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
            free(copy);
            return cand;
        }
        free(cand);
        if (!sep)
            break;
        dir = sep + 1;
    }
    free(copy);
    return NULL;
}

static void *ps_load_record(const char *hostpath) {
    int i;
    size_t n;
    ps_mods_init_self();
    for (i = 1; i < PS_MOD_MAX; i++) {
        if (!ps_mods[i].in_use) {
            n = strlen(hostpath);
            if (n >= sizeof(ps_mods[i].path))
                n = sizeof(ps_mods[i].path) - 1;
            memcpy(ps_mods[i].path, hostpath, n);
            ps_mods[i].path[n] = '\0';
            ps_mods[i].in_use = 1;
            return ps_mod_cookie(i);
        }
    }
    w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

static void *ps_load_common(const char *n8, W32_DWORD flags, int ex) {
    char *resolved = NULL;
    void *cookie;
    char callerDir[4096];

    if (!n8 || !n8[0]) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (ex) {
        if (flags & ~(W32_DONT_RESOLVE_DLL_REFERENCES |
                      W32_LOAD_WITH_ALTERED_SEARCH_PATH |
                      W32_LOAD_LIBRARY_AS_DATAFILE |
                      W32_LOAD_IGNORE_CODE_AUTHZ_LEVEL)) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return NULL;
        }
    } else if (flags != 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    callerDir[0] = '\0';
    if (flags & W32_LOAD_WITH_ALTERED_SEARCH_PATH) {
        const char *last;
        ps_mods_init_self();
        last = strrchr(ps_mods[0].path, '/');
        if (last) {
            size_t dl = (size_t)(last - ps_mods[0].path);
            if (dl >= sizeof(callerDir))
                dl = sizeof(callerDir) - 1;
            memcpy(callerDir, ps_mods[0].path, dl);
            callerDir[dl] = '\0';
        }
    }
    resolved = ps_resolve_dll(n8,
        (flags & W32_LOAD_WITH_ALTERED_SEARCH_PATH) != 0, callerDir);
    if (!resolved) {
        w32_set_last_error(W32_ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    cookie = ps_load_record(resolved);
    free(resolved);
    return cookie;
}

W32ABI void *LoadLibraryW(W32_LPCWSTR name) {
    char *n8 = ps_w16_dup(name);
    void *cookie;
    if (!n8)
        return NULL;
    cookie = ps_load_common(n8, 0, 0);
    free(n8);
    return cookie;
}

W32ABI void *LoadLibraryExA(W32_LPCSTR name, W32_HANDLE reserved,
                             W32_DWORD flags) {
    if (reserved != NULL) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!name) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return ps_load_common(name, flags, 1);      /* ACP is UTF-8 */
}

W32ABI void *LoadLibraryExW(W32_LPCWSTR name, W32_HANDLE reserved,
                             W32_DWORD flags) {
    char *n8;
    void *cookie;
    if (reserved != NULL) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    n8 = ps_w16_dup(name);
    if (!n8)
        return NULL;
    cookie = ps_load_common(n8, flags, 1);
    free(n8);
    return cookie;
}

/* Frees a ps-table cookie.  Sets no error: the two callers (FreeLibrary
 * here, w32_FreeLibrary in the loader) name their own failures. */
int ps_mod_free(void *mod) {
    int slot;
    ps_mods_init_self();
    if (!mod)
        return 0;
    slot = ps_mod_slot(mod);
    if (slot <= 0 || !ps_mods[slot].in_use)
        return 0;
    ps_mods[slot].in_use = 0;
    return 1;
}

W32ABI W32_BOOL FreeLibrary(void *mod) {
    if (!mod)
        return ps_fail(W32_ERROR_INVALID_PARAMETER);
    if (!ps_mod_free(mod))
        return ps_fail(W32_ERROR_INVALID_HANDLE);
    return 1;
}
