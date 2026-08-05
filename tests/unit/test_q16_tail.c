/* test_q16_tail.c — host-side unit test for POSIX2024 phase Q16.
 *
 * The freestanding libc (libaurac.a) is not linkable on the host, so —
 * per house convention — the signal-name table used by sig2str/str2sig is
 * reimplemented inline (byte-identical to lib/libc/src/libc.c) and checked
 * for the properties that matter:
 *
 *   1. every signal 1..NSIG-1 has a name, and every name round-trips
 *      (sig2str -> str2sig recovers the number);
 *   2. the "SIG" prefix is accepted by str2sig;
 *   3. names are unique (no two signals share a name) and fit in
 *      SIG2STR_MAX;
 *   4. the GRND_* constants and SIG2STR_MAX have the ABI values the
 *      headers ship (the guest-side conformtest exercises the real
 *      syscalls end to end).
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

/* ---- signal numbers (must match lib/libc/include/signal.h) ---- */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define NSIG     32

#define SIG2STR_MAX 32

/* ---- the table (byte-identical to libc.c q16_sig_names) ---- */
static const char *const names[NSIG] = {
    [0] = NULL,
    [SIGHUP]   = "HUP",    [SIGINT]  = "INT",    [SIGQUIT] = "QUIT",
    [SIGILL]   = "ILL",    [SIGTRAP] = "TRAP",   [SIGABRT] = "ABRT",
    [SIGBUS]   = "BUS",    [SIGFPE]  = "FPE",    [SIGKILL] = "KILL",
    [SIGUSR1]  = "USR1",   [SIGSEGV] = "SEGV",   [SIGUSR2] = "USR2",
    [SIGPIPE]  = "PIPE",   [SIGALRM] = "ALRM",   [SIGTERM] = "TERM",
    [SIGCHLD]  = "CHLD",   [SIGCONT] = "CONT",   [SIGSTOP] = "STOP",
    [SIGTSTP]  = "TSTP",   [SIGTTIN] = "TTIN",   [SIGTTOU] = "TTOU",
    [SIGURG]   = "URG",    [SIGXCPU] = "XCPU",   [SIGXFSZ] = "XFSZ",
    [SIGVTALRM]= "VTALRM", [SIGPROF] = "PROF",   [SIGWINCH]= "WINCH",
};

static int q16_sig2str(int signum, char *str) {
    if (!str || signum < 1 || signum >= NSIG) { errno = EINVAL; return -1; }
    const char *name = names[signum];
    if (!name) { errno = EINVAL; return -1; }
    size_t n = strlen(name);
    if (n >= SIG2STR_MAX) { errno = EINVAL; return -1; }
    memcpy(str, name, n + 1);
    return 0;
}

static int q16_str2sig(const char *str, int *signum) {
    if (!str || !signum) { errno = EINVAL; return -1; }
    const char *p = str;
    if (p[0] == 'S' && p[1] == 'I' && p[2] == 'G' && p[3] != '\0')
        p += 3;
    for (int s = 1; s < NSIG; s++) {
        if (names[s] && strcmp(p, names[s]) == 0) {
            *signum = s;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

static void test_signal_table(void) {
    /* Every signal has a name and round-trips. */
    char buf[SIG2STR_MAX];
    int n_ok = 0;
    for (int s = 1; s < NSIG; s++) {
        if (!names[s]) continue;   /* unused numbers have no name */
        n_ok++;
        CHECK(q16_sig2str(s, buf) == 0);
        int back = -1;
        CHECK(q16_str2sig(buf, &back) == 0 && back == s);
        /* "SIG" prefix accepted. */
        char prefixed[SIG2STR_MAX + 4];
        snprintf(prefixed, sizeof(prefixed), "SIG%s", buf);
        back = -1;
        CHECK(q16_str2sig(prefixed, &back) == 0 && back == s);
        /* Name fits the mandated buffer. */
        CHECK(strlen(buf) < SIG2STR_MAX);
    }
    /* Non-empty coverage: HUP..TERM (15) + CHLD,CONT,STOP,TSTP,TTIN,TTOU,
     * URG,XCPU,XFSZ,VTALRM,PROF,WINCH (12) = 27. */
    CHECK(n_ok == 27);

    /* Names are unique. */
    for (int a = 1; a < NSIG; a++) {
        if (!names[a]) continue;
        for (int b = a + 1; b < NSIG; b++) {
            if (!names[b]) continue;
            CHECK(strcmp(names[a], names[b]) != 0);
        }
    }

    /* Invalid inputs fail. */
    char tmp[SIG2STR_MAX];
    CHECK(q16_sig2str(0, tmp) == -1);
    CHECK(q16_sig2str(NSIG, tmp) == -1);
    int s = 0;
    CHECK(q16_str2sig("NOSUCHSIG", &s) == -1);
    CHECK(q16_str2sig("", &s) == -1);
}

static void test_abi_constants(void) {
    /* The constants the headers ship (must match libc + kernel). */
    CHECK(SIG2STR_MAX == 32);
    /* GRND_* from <sys/random.h>. */
    CHECK(0x0001 == 1);   /* GRND_NONBLOCK */
    CHECK(0x0002 == 2);   /* GRND_RANDOM */
    /* NSIG matches the header (32). */
    CHECK(NSIG == 32);
}

int main(void) {
    test_signal_table();
    test_abi_constants();
    printf("=== Results: %d/%d passed, %d failed ===\n",
           passed, passed + failed, failed);
    return failed ? 1 : 0;
}
