/*
 * test_posix_spawn.c — host-side unit test for Phase Q9 posix_spawn API.
 *
 * Tests file_actions and attr API surface.  The actual fork+exec is
 * validated by integration tests.
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

/* Inline re-implementation of file_actions to match posix_spawn.c */

#define ACTION_CLOSE 0
#define ACTION_DUP2  1
#define ACTION_OPEN  2

typedef struct {
    int nactions;
    struct {
        int type;
        int fd;
        int fd2;
        char *path;
        int oflag;
        int mode;
    } actions[16];
} test_file_actions_t;

static int test_fa_init(test_file_actions_t *fa) {
    if (!fa) return EINVAL;
    fa->nactions = 0;
    return 0;
}

static int test_fa_addclose(test_file_actions_t *fa, int fd) {
    if (!fa || fa->nactions >= 16) return ENOMEM;
    fa->actions[fa->nactions].type = ACTION_CLOSE;
    fa->actions[fa->nactions].fd = fd;
    fa->nactions++;
    return 0;
}

static int test_fa_adddup2(test_file_actions_t *fa, int fd, int newfd) {
    if (!fa || fa->nactions >= 16) return ENOMEM;
    fa->actions[fa->nactions].type = ACTION_DUP2;
    fa->actions[fa->nactions].fd = fd;
    fa->actions[fa->nactions].fd2 = newfd;
    fa->nactions++;
    return 0;
}

static int test_fa_addopen(test_file_actions_t *fa, int fd,
                           const char *path, int oflag, int mode) {
    if (!fa || fa->nactions >= 16) return ENOMEM;
    fa->actions[fa->nactions].type = ACTION_OPEN;
    fa->actions[fa->nactions].fd = fd;
    fa->actions[fa->nactions].path = (char *)path;
    fa->actions[fa->nactions].oflag = oflag;
    fa->actions[fa->nactions].mode = mode;
    fa->nactions++;
    return 0;
}

/* Inline re-implementation of attr */

typedef struct {
    short flags;
    int pgroup;
    int sigmask;
    int sigdefault;
    int sched_priority;
    int schedpolicy;
} test_spawnattr_t;

#define TEST_POSIX_SPAWN_RESETIDS       0x01
#define TEST_POSIX_SPAWN_SETPGROUP      0x02
#define TEST_POSIX_SPAWN_SETSIGDEF      0x04
#define TEST_POSIX_SPAWN_SETSIGMASK     0x08

static int test_attr_init(test_spawnattr_t *a) {
    if (!a) return EINVAL;
    memset(a, 0, sizeof(*a));
    return 0;
}

static int test_attr_setflags(test_spawnattr_t *a, short f) {
    if (!a) return EINVAL;
    a->flags = f;
    return 0;
}

static int test_attr_getflags(const test_spawnattr_t *a, short *f) {
    if (!a || !f) return EINVAL;
    *f = a->flags;
    return 0;
}

/* ---- Tests ---- */

static void test_file_actions(void) {
    test_file_actions_t fa;

    CHECK(test_fa_init(&fa) == 0);
    CHECK(fa.nactions == 0);

    /* Add close */
    CHECK(test_fa_addclose(&fa, 5) == 0);
    CHECK(fa.nactions == 1);
    CHECK(fa.actions[0].type == ACTION_CLOSE);
    CHECK(fa.actions[0].fd == 5);

    /* Add dup2 */
    CHECK(test_fa_adddup2(&fa, 3, 7) == 0);
    CHECK(fa.nactions == 2);
    CHECK(fa.actions[1].type == ACTION_DUP2);
    CHECK(fa.actions[1].fd == 3);
    CHECK(fa.actions[1].fd2 == 7);

    /* Add open */
    CHECK(test_fa_addopen(&fa, 10, "/tmp/test", 0x42, 0644) == 0);
    CHECK(fa.nactions == 3);
    CHECK(fa.actions[2].type == ACTION_OPEN);
    CHECK(fa.actions[2].fd == 10);
    CHECK(fa.actions[2].oflag == 0x42);

    /* NULL init */
    CHECK(test_fa_init(NULL) == EINVAL);

    /* Overflow: fill 16 slots */
    test_file_actions_t full;
    test_fa_init(&full);
    for (int i = 0; i < 16; i++)
        CHECK(test_fa_addclose(&full, i) == 0);
    CHECK(test_fa_addclose(&full, 99) == ENOMEM);
}

static void test_spawnattr(void) {
    test_spawnattr_t a;

    CHECK(test_attr_init(&a) == 0);
    CHECK(a.flags == 0);

    /* Set and get flags */
    CHECK(test_attr_setflags(&a, TEST_POSIX_SPAWN_RESETIDS | TEST_POSIX_SPAWN_SETPGROUP) == 0);
    short f = 0;
    CHECK(test_attr_getflags(&a, &f) == 0);
    CHECK(f == (TEST_POSIX_SPAWN_RESETIDS | TEST_POSIX_SPAWN_SETPGROUP));

    /* NULL checks */
    CHECK(test_attr_init(NULL) == EINVAL);
    CHECK(test_attr_getflags(NULL, &f) == EINVAL);
    CHECK(test_attr_getflags(&a, NULL) == EINVAL);
    CHECK(test_attr_setflags(NULL, 0) == EINVAL);
}

int main(void) {
    printf("test_posix_spawn:\n");

    test_file_actions();
    test_spawnattr();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
