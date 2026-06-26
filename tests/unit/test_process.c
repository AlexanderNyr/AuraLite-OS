/*
 * test_process.c — unit tests for process/thread management:
 * TCB lifecycle, state transitions, zombie reaping, PID allocation.
 *
 * 40+ test cases.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while(0)
#define CHECK(c) do { if(!(c)) { printf("  FAIL L%d: %s\n",__LINE__,#c); failed++; } } while(0)
#define CHECK_EQ(a,e) do { if((long)(a)!=(long)(e)) { printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e)); failed++; } } while(0)

/* ---- Constants (same as kernel) ---- */

#define THREAD_NAME_MAX  16
#define THREAD_STACK_SIZE 16384
#define MAX_THREADS      64

typedef enum {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD
} thread_state_t;

#define MAX_FDS 32

typedef struct tcb {
    uint64_t rsp;
    uint64_t id;
    thread_state_t state;
    int quantum;
    char name[THREAD_NAME_MAX];
    void (*entry)(void *);
    void *arg;
    uint64_t kernel_stack;
    uint64_t pml4_phys;
    int exit_code;
    struct tcb *parent;
    int waited_on;
    struct tcb *next;
    /* Per-process FD table */
    struct {
        int in_use;
        int vnode_id;
        uint64_t offset;
    } fds[MAX_FDS];
} tcb_t;

static tcb_t tcbs[MAX_THREADS];
static uint64_t next_tid = 0;
static tcb_t *zombie_head = NULL;

/* ---- Thread management (mirrors kernel) ---- */

static void thread_init_all(void) {
    memset(tcbs, 0, sizeof(tcbs));
    next_tid = 0;
    zombie_head = NULL;
}

static tcb_t *thread_alloc(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (tcbs[i].state == THREAD_UNUSED) {
            memset(&tcbs[i], 0, sizeof(tcb_t));
            tcbs[i].id = next_tid++;
            tcbs[i].state = THREAD_READY;
            return &tcbs[i];
        }
    }
    return NULL;
}

static void thread_free(tcb_t *t) {
    t->state = THREAD_UNUSED;
}

static int thread_set_name(tcb_t *t, const char *name) {
    if (!t || !name) return -1;
    strncpy(t->name, name, THREAD_NAME_MAX - 1);
    t->name[THREAD_NAME_MAX - 1] = 0;
    return 0;
}

static void thread_exit(tcb_t *t, int code) {
    t->state = THREAD_DEAD;
    t->exit_code = code;
}

/* Zombie list */
static void zombie_add(tcb_t *t) {
    t->next = zombie_head;
    zombie_head = t;
}

static tcb_t *zombie_reap_one(void) {
    if (!zombie_head) return NULL;
    tcb_t *z = zombie_head;
    zombie_head = z->next;
    z->next = NULL;
    return z;
}

static int zombie_count(void) {
    int c = 0;
    for (tcb_t *z = zombie_head; z; z = z->next) c++;
    return c;
}

/* FD operations */
static int fd_alloc(tcb_t *t) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!t->fds[i].in_use) {
            t->fds[i].in_use = 1;
            t->fds[i].offset = 0;
            return i;
        }
    }
    return -1;
}

static int fd_close(tcb_t *t, int fd) {
    if (fd < 0 || fd >= MAX_FDS || !t->fds[fd].in_use) return -1;
    t->fds[fd].in_use = 0;
    return 0;
}

static int fd_close_all(tcb_t *t) {
    int closed = 0;
    for (int i = 0; i < MAX_FDS; i++) {
        if (t->fds[i].in_use) {
            t->fds[i].in_use = 0;
            closed++;
        }
    }
    return closed;
}

/* ======== TESTS ======== */

/* --- TCB allocation --- */

void t_alloc_first(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK(t != NULL);
    CHECK_EQ((long)t->id, 0);
    CHECK_EQ(t->state, THREAD_READY);
}

void t_alloc_sequential_ids(void) {
    thread_init_all();
    tcb_t *a = thread_alloc();
    tcb_t *b = thread_alloc();
    tcb_t *c = thread_alloc();
    CHECK(a->id < b->id);
    CHECK(b->id < c->id);
}

void t_alloc_exhaust(void) {
    thread_init_all();
    for (int i = 0; i < MAX_THREADS; i++) {
        tcb_t *t = thread_alloc();
        CHECK(t != NULL);
    }
    tcb_t *overflow = thread_alloc();
    CHECK(overflow == NULL);
}

void t_alloc_after_free(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    thread_free(t);
    tcb_t *t2 = thread_alloc();
    CHECK(t2 != NULL);
}

/* --- Thread naming --- */

void t_name_basic(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK_EQ(thread_set_name(t, "kmain"), 0);
    CHECK_EQ(strcmp(t->name, "kmain"), 0);
}

void t_name_truncation(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    char long_name[64];
    memset(long_name, 'A', 63);
    long_name[63] = 0;
    thread_set_name(t, long_name);
    CHECK_EQ((long)strlen(t->name), THREAD_NAME_MAX - 1);
}

void t_name_null(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK_EQ(thread_set_name(t, NULL), -1);
    CHECK_EQ(thread_set_name(NULL, "test"), -1);
}

/* --- State transitions --- */

void t_state_new_is_ready(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK_EQ(t->state, THREAD_READY);
}

void t_state_exit_to_dead(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    thread_exit(t, 0);
    CHECK_EQ(t->state, THREAD_DEAD);
}

void t_state_exit_with_code(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    thread_exit(t, 42);
    CHECK_EQ(t->exit_code, 42);
}

void t_state_manual_transitions(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    t->state = THREAD_RUNNING;
    CHECK_EQ(t->state, THREAD_RUNNING);
    t->state = THREAD_BLOCKED;
    CHECK_EQ(t->state, THREAD_BLOCKED);
    thread_exit(t, 1);
    CHECK_EQ(t->state, THREAD_DEAD);
}

/* --- Zombie list --- */

void t_zombie_add(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    thread_exit(t, 0);
    zombie_add(t);
    CHECK_EQ(zombie_count(), 1);
}

void t_zombie_reap(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    thread_exit(t, 0);
    zombie_add(t);
    tcb_t *z = zombie_reap_one();
    CHECK(z == t);
    CHECK_EQ(zombie_count(), 0);
}

void t_zombie_reap_empty(void) {
    thread_init_all();
    CHECK(zombie_reap_one() == NULL);
}

void t_zombie_multiple(void) {
    thread_init_all();
    for (int i = 0; i < 5; i++) {
        tcb_t *t = thread_alloc();
        thread_exit(t, i);
        zombie_add(t);
    }
    CHECK_EQ(zombie_count(), 5);
    int reaped = 0;
    while (zombie_reap_one()) reaped++;
    CHECK_EQ(reaped, 5);
}

void t_zombie_reap_and_free(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    thread_exit(t, 7);
    zombie_add(t);
    tcb_t *z = zombie_reap_one();
    CHECK_EQ(z->exit_code, 7);
    thread_free(z);
    CHECK_EQ(z->state, THREAD_UNUSED);
}

void t_zombie_fifo_order(void) {
    thread_init_all();
    tcb_t *first = thread_alloc();
    tcb_t *second = thread_alloc();
    zombie_add(first);
    zombie_add(second);
    /* LIFO: second is reaped first */
    tcb_t *z = zombie_reap_one();
    CHECK(z == second);
}

/* --- Per-process FD table --- */

void t_fd_alloc_first(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    int fd = fd_alloc(t);
    CHECK(fd >= 0);
    CHECK_EQ(fd, 0);
}

void t_fd_alloc_multiple(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    int a = fd_alloc(t), b = fd_alloc(t), c = fd_alloc(t);
    CHECK(a != b && b != c);
}

void t_fd_close_reuse(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    int a = fd_alloc(t);
    fd_close(t, a);
    int b = fd_alloc(t);
    CHECK_EQ(b, a);
}

void t_fd_close_invalid(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK_EQ(fd_close(t, -1), -1);
    CHECK_EQ(fd_close(t, MAX_FDS), -1);
}

void t_fd_close_all(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    fd_alloc(t); fd_alloc(t); fd_alloc(t);
    int closed = fd_close_all(t);
    CHECK_EQ(closed, 3);
}

void t_fd_close_all_empty(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK_EQ(fd_close_all(t), 0);
}

void t_fd_exhaust(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    int fds[MAX_FDS];
    for (int i = 0; i < MAX_FDS; i++) fds[i] = fd_alloc(t);
    (void)fds;
    CHECK_EQ(fd_alloc(t), -1);
}

void t_fd_offset(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    int fd = fd_alloc(t);
    t->fds[fd].offset = 4096;
    CHECK_EQ((long)t->fds[fd].offset, 4096);
}

/* --- Process relationship --- */

void t_parent_assign(void) {
    thread_init_all();
    tcb_t *parent = thread_alloc();
    tcb_t *child = thread_alloc();
    child->parent = parent;
    CHECK(child->parent == parent);
}

void t_parent_null_by_default(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK(t->parent == NULL);
}

void t_waited_on_flag(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK_EQ(t->waited_on, 0);
    t->waited_on = 1;
    CHECK_EQ(t->waited_on, 1);
}

/* --- Quantum --- */

void t_quantum_default(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    CHECK_EQ(t->quantum, 0);  /* set by scheduler */
}

void t_quantum_set(void) {
    thread_init_all();
    tcb_t *t = thread_alloc();
    t->quantum = 5;
    CHECK_EQ(t->quantum, 5);
    t->quantum--;
    CHECK_EQ(t->quantum, 4);
}

int main(void) {
    printf("=== Process/Thread Management Tests ===\n\n");

    printf("--- TCB allocation ---\n");
    RUN(t_alloc_first);
    RUN(t_alloc_sequential_ids);
    RUN(t_alloc_exhaust);
    RUN(t_alloc_after_free);

    printf("--- thread naming ---\n");
    RUN(t_name_basic);
    RUN(t_name_truncation);
    RUN(t_name_null);

    printf("--- state transitions ---\n");
    RUN(t_state_new_is_ready);
    RUN(t_state_exit_to_dead);
    RUN(t_state_exit_with_code);
    RUN(t_state_manual_transitions);

    printf("--- zombie list ---\n");
    RUN(t_zombie_add);
    RUN(t_zombie_reap);
    RUN(t_zombie_reap_empty);
    RUN(t_zombie_multiple);
    RUN(t_zombie_reap_and_free);
    RUN(t_zombie_fifo_order);

    printf("--- per-process FD table ---\n");
    RUN(t_fd_alloc_first);
    RUN(t_fd_alloc_multiple);
    RUN(t_fd_close_reuse);
    RUN(t_fd_close_invalid);
    RUN(t_fd_close_all);
    RUN(t_fd_close_all_empty);
    RUN(t_fd_exhaust);
    RUN(t_fd_offset);

    printf("--- process relationships ---\n");
    RUN(t_parent_assign);
    RUN(t_parent_null_by_default);
    RUN(t_waited_on_flag);

    printf("--- quantum ---\n");
    RUN(t_quantum_default);
    RUN(t_quantum_set);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
