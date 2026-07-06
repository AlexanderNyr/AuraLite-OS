/*
 * test_stdio_ext.c — host-side unit test for Phase Q2 stdio extensions.
 *
 * Tests the API surface (declarations) and basic semantics of: getdelim,
 * getline, dprintf/vdprintf, asprintf/vasprintf, fmemopen, open_memstream,
 * popen/pclose, and the locking stubs.
 *
 * Compiled with the host compiler; links against the host libc because
 * these functions exist on every POSIX system.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <fcntl.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

static void test_getdelim_getline(void) {
    /* Test getdelim with a memory-backed FILE* via fmemopen. */
    const char *data = "hello\nworld\ntest line";
    FILE *mem = fmemopen((void *)data, strlen(data), "r");
    CHECK(mem != NULL);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    /* Read "hello\n" via getline */
    n = getline(&line, &cap, mem);
    CHECK(n == 6);           /* "hello\n" */
    CHECK(strcmp(line, "hello\n") == 0);

    /* Read "world\n" via getdelim with '\n' */
    n = getdelim(&line, &cap, '\n', mem);
    CHECK(n == 6);           /* "world\n" */

    /* Read rest via getdelim with EOF */
    n = getdelim(&line, &cap, '\n', mem);
    CHECK(n == 9);           /* "test line" */
    CHECK(strcmp(line, "test line") == 0);

    /* EOF */
    n = getline(&line, &cap, mem);
    CHECK(n == -1);

    free(line);
    fclose(mem);
}

static void test_dprintf(void) {
    char buf[64];
    int p[2];
    CHECK(pipe(p) == 0);

    dprintf(p[1], "hello %d", 42);
    close(p[1]);

    ssize_t n = read(p[0], buf, sizeof(buf) - 1);
    CHECK(n > 0);
    buf[n] = '\0';
    CHECK(strcmp(buf, "hello 42") == 0);
    close(p[0]);
}

static void test_asprintf(void) {
    char *p = NULL;
    int n = asprintf(&p, "test %d %s", 42, "ok");
    CHECK(n == 10);
    CHECK(p != NULL);
    CHECK(strcmp(p, "test 42 ok") == 0);
    free(p);

    /* Edge: zero-length result via non-empty format */
    n = asprintf(&p, "%s", "");
    CHECK(n == 0);
    CHECK(p != NULL);
    CHECK(strcmp(p, "") == 0);
    free(p);
}

static void test_fmemopen(void) {
    const char *test_data = "fmemopen test data";
    size_t len = strlen(test_data);

    /* Read mode */
    FILE *f = fmemopen((void *)test_data, len, "r");
    CHECK(f != NULL);

    char buf[64];
    CHECK(fgets(buf, sizeof(buf), f) != NULL);
    CHECK(strcmp(buf, "fmemopen test data") == 0);
    CHECK(feof(f));
    fclose(f);

}

static void test_popen(void) {
    FILE *f = popen("echo hello", "r");
    CHECK(f != NULL);

    char buf[64];
    CHECK(fgets(buf, sizeof(buf), f) != NULL);
    /* popen("echo hello") should produce "hello\n" */
    CHECK(strcmp(buf, "hello\n") == 0);
    int status = pclose(f);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

static void test_locking_stubs(void) {
    /* These are no-op stubs in our implementation; test they compile and
     * have the expected return types. */
    FILE *f = fopen("/dev/null", "r");
    CHECK(f != NULL);

    flockfile(f);
    funlockfile(f);
    CHECK(ftrylockfile(f) == 0);

    fclose(f);
}

static void test_memstream(void) {
    /* open_memstream is POSIX.1-2024; test basic usage. */
    char *ptr = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&ptr, &size);
    if (!f) {
        /* Some hosts may not support open_memstream; skip. */
        printf("  (open_memstream not available on host, skipping)\n");
        return;
    }
    fprintf(f, "hello %d", 42);
    fclose(f);
    CHECK(ptr != NULL);
    CHECK(strcmp(ptr, "hello 42") == 0);
    CHECK(size == strlen("hello 42"));
    free(ptr);
}

int main(void) {
    printf("test_stdio_ext:\n");

    test_getdelim_getline();
    test_dprintf();
    test_asprintf();
    test_fmemopen();
    test_popen();
    test_locking_stubs();
    test_memstream();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
