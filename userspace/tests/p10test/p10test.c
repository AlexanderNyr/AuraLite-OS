/* p10test.c — standalone P10 (POSIX.1-2017) libc regression program.
 *
 * Unlike /selftest (which is long and currently halts mid-way on the P8 alarm
 * delivery path), this binary runs ONLY the P10 library surface so an
 * integration test can reach every assertion quickly and reliably:
 *   env vars, strtod/strtol family, extended math, fnmatch, POSIX regex,
 *   POSIX semaphores, inet_pton/ntop, getcwd, opendir/readdir.
 *
 * It deliberately avoids fork/exec/signals so there is no race with the
 * spawning shell's pending wait4. Output uses stable "P10TEST PASS/FAIL"
 * markers and ends with "P10TEST DONE: <pass>/<total>".
 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "math.h"
#include "regex.h"
#include "fnmatch.h"
#include "dirent.h"
#include "semaphore.h"
#include "sys/socket.h"
#include "arpa/inet.h"

static int g_pass = 0;
static int g_total = 0;

static void check(const char *name, int cond) {
    g_total++;
    if (cond) {
        g_pass++;
        printf("P10TEST PASS: %s\n", name);
    } else {
        printf("P10TEST FAIL: %s\n", name);
    }
}

int main(void) {
    printf("P10TEST START\n");

    /* ---- environment variables ---- */
    setenv("AURA_T", "hello", 1);
    const char *ev = getenv("AURA_T");
    check("setenv/getenv round-trip", ev && strcmp(ev, "hello") == 0);
    setenv("AURA_T", "world", 1);
    ev = getenv("AURA_T");
    check("setenv overwrite=1 replaces", ev && strcmp(ev, "world") == 0);
    setenv("AURA_T", "ignored", 0);
    ev = getenv("AURA_T");
    check("setenv overwrite=0 keeps", ev && strcmp(ev, "world") == 0);
    unsetenv("AURA_T");
    check("unsetenv removes", getenv("AURA_T") == NULL);

    /* ---- strtod / strtol / strtoul ---- */
    char *end = NULL;
    double d = strtod("3.5xyz", &end);
    check("strtod parses 3.5", d > 3.49 && d < 3.51 && end && *end == 'x');
    long lv = strtol("  -42rest", &end, 10);
    check("strtol parses -42", lv == -42 && end && *end == 'r');
    unsigned long uv = strtoul("ff", NULL, 16);
    check("strtoul hex ff==255", uv == 255);

    /* ---- extended math ---- */
    check("asin(1)==pi/2", asin(1.0) > 1.5707 && asin(1.0) < 1.5709);
    check("atan2(1,1)==pi/4", atan2(1.0, 1.0) > 0.7853 && atan2(1.0, 1.0) < 0.7854);
    check("fmod(7,3)==1", fmod(7.0, 3.0) > 0.999 && fmod(7.0, 3.0) < 1.001);

    /* ---- fnmatch ---- */
    check("fnmatch *.c matches foo.c", fnmatch("*.c", "foo.c", 0) == 0);
    check("fnmatch *.c rejects foo.h", fnmatch("*.c", "foo.h", 0) != 0);
    check("fnmatch PATHNAME star stops at slash",
          fnmatch("a/*", "a/b", FNM_PATHNAME) == 0 &&
          fnmatch("a/*", "a/b/c", FNM_PATHNAME) != 0);

    /* ---- POSIX regex (substring matcher) ---- */
    regex_t re;
    if (regcomp(&re, "ell", 0) == 0) {
        regmatch_t m[1];
        int rc = regexec(&re, "hello", 1, m, 0);
        check("regexec finds 'ell' in 'hello'",
              rc == 0 && m[0].rm_so == 1 && m[0].rm_eo == 4);
        check("regexec no match returns nonzero",
              regexec(&re, "world", 1, m, 0) != 0);
        regfree(&re);
    } else {
        check("regcomp 'ell'", 0);
        check("regexec finds 'ell' in 'hello'", 0);
    }

    /* ---- POSIX semaphore (futex-backed) ---- */
    sem_t sem;
    if (sem_init(&sem, 0, 1) == 0) {
        check("sem_wait on count=1 succeeds", sem_wait(&sem) == 0);
        check("sem_trywait on count=0 fails", sem_trywait(&sem) != 0);
        check("sem_post returns 0", sem_post(&sem) == 0);
        check("sem_wait after post succeeds", sem_wait(&sem) == 0);
        sem_destroy(&sem);
    } else {
        check("sem_init", 0);
    }

    /* ---- inet_pton / inet_ntop ---- */
    unsigned char ipb[4] = {0};
    check("inet_pton 127.0.0.1",
          inet_pton(AF_INET, "127.0.0.1", ipb) == 1 &&
          ipb[0] == 127 && ipb[3] == 1);
    char ipstr[16];
    unsigned char ip2[4] = {192, 168, 0, 5};
    check("inet_ntop 192.168.0.5",
          inet_ntop(AF_INET, ip2, ipstr, sizeof(ipstr)) != NULL &&
          strcmp(ipstr, "192.168.0.5") == 0);
    check("inet_pton rejects 256.0.0.1",
          inet_pton(AF_INET, "256.0.0.1", ipb) == 0);

    /* ---- getcwd ---- */
    char cwdbuf[256];
    char *cw = getcwd(cwdbuf, sizeof(cwdbuf));
    check("getcwd returns a path", cw != NULL && cwdbuf[0] != '\0');

    /* ---- opendir / readdir over / ---- */
    DIR *dp = opendir("/");
    if (dp) {
        int saw_entry = 0;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] != '\0') saw_entry = 1;
        }
        check("opendir/readdir / yields entries", saw_entry);
        closedir(dp);
    } else {
        check("opendir /", 0);
    }

    printf("P10TEST DONE: %d/%d\n", g_pass, g_total);
    if (g_pass == g_total) {
        printf("P10TEST ALL PASS\n");
        return 0;
    }
    printf("P10TEST %d FAILURES\n", g_total - g_pass);
    return 1;
}
