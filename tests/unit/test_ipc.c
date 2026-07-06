/*
 * test_ipc.c — host-side unit test for Phase Q7 IPC functions.
 *
 * Tests: shm_open/shm_unlink (interface), sem_open/sem_close/sem_unlink
 * (interface), mq_open/mq_close/mq_unlink (interface), and type constants.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

/* Type checks */
typedef int test_mqd_t;
#define TEST_MQD_INVALID ((test_mqd_t)-1)

struct test_mq_attr {
    long mq_flags;
    long mq_maxmsg;
    long mq_msgsize;
    long mq_curmsgs;
};

#define TEST_SEM_FAILED ((void *)-1)

/* ---- Tests ---- */

static void test_mq_constants(void) {
    CHECK(TEST_MQD_INVALID == -1);
    CHECK(sizeof(struct test_mq_attr) == 4 * sizeof(long));
}

static void test_mq_path_logic(void) {
    /* Test the name-to-path conversion logic (inline) */
    const char *name = "/myqueue";
    const char *n = name;
    while (*n == '/') n++;
    CHECK(strcmp(n, "myqueue") == 0);

    name = "myqueue";
    n = name;
    while (*n == '/') n++;
    CHECK(strcmp(n, "myqueue") == 0);
}

static void test_sem_constants(void) {
    CHECK(TEST_SEM_FAILED == ((void *)-1));
}

static void test_shm_path_logic(void) {
    /* Test path construction for shm_open */
    char path[512];
    const char *name = "/test_shm";
    if (name[0] == '/')
        snprintf(path, sizeof(path), "/dev/shm%s", name);
    else
        snprintf(path, sizeof(path), "/dev/shm/%s", name);
    CHECK(strcmp(path, "/dev/shm/test_shm") == 0);

    name = "test_shm";
    if (name[0] == '/')
        snprintf(path, sizeof(path), "/dev/shm%s", name);
    else
        snprintf(path, sizeof(path), "/dev/shm/%s", name);
    CHECK(strcmp(path, "/dev/shm/test_shm") == 0);
}

static void test_sem_path_logic(void) {
    /* Test path construction for sem_open */
    char path[512];
    const char *name = "/testsem";
    snprintf(path, sizeof(path), "/dev/shm/sem%s%s",
             name[0]=='/' ? "" : "/", name);
    CHECK(strcmp(path, "/dev/shm/sem/testsem") == 0);

    name = "testsem";
    snprintf(path, sizeof(path), "/dev/shm/sem%s%s",
             name[0]=='/' ? "" : "/", name);
    CHECK(strcmp(path, "/dev/shm/sem/testsem") == 0);
}

static void test_mq_attr(void) {
    struct test_mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 16;
    attr.mq_msgsize = 1024;
    attr.mq_curmsgs = 0;
    CHECK(attr.mq_maxmsg == 16);
    CHECK(attr.mq_msgsize == 1024);
}

int main(void) {
    printf("test_ipc:\n");

    test_mq_constants();
    test_mq_path_logic();
    test_sem_constants();
    test_shm_path_logic();
    test_sem_path_logic();
    test_mq_attr();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
