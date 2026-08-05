/* test_mq_notify.c — host-side unit test for POSIX2024 phase Q15.
 *
 * The freestanding libc (libaurac.a) is not linkable on the host, so —
 * exactly like test_ipc.c / test_signals.c — the logic under test is
 * reimplemented inline and the algorithms are checked against their
 * specification:
 *
 *   1. the queue record format ([len:4][data]) and the mq_receive dequeue
 *      algorithm: consume the first record, keep the tail, so an emptied
 *      queue is a 0-byte file (the state the mq_notify watcher keys on);
 *   2. the mq_notify registration state machine: first free slot, EBUSY on
 *      a second registration for the same queue (path), deregistration
 *      clears the slot, re-arm semantics after the queue is drained.
 *
 * This mirrors the guest-layer coverage (conformtest test_mq_notify),
 * which exercises the real syscall-backed implementation in QEMU.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

/* ------------------------------------------------------------------ */
/* 1. Queue record format + dequeue algorithm (inline of mq_receive)  */
/* ------------------------------------------------------------------ */

/* Simulated queue file contents. */
struct qfile {
    unsigned char data[256];
    size_t size;
};

/* Append a [len:4][data] record (inline of mq_send). */
static void q_append(struct qfile *f, const char *msg, size_t len) {
    unsigned int l = (unsigned int)len;
    memcpy(f->data + f->size, &l, sizeof(l));
    f->size += sizeof(l);
    memcpy(f->data + f->size, msg, len);
    f->size += len;
}

/* Dequeue the first record; returns its payload length, -1 on empty. */
static long q_dequeue(struct qfile *f, char *out, size_t outsz) {
    if (f->size < sizeof(unsigned int)) return -1;   /* empty queue */
    unsigned int len;
    memcpy(&len, f->data, sizeof(len));
    if (len > outsz) return -2;                       /* EMSGSIZE */
    if (f->size < sizeof(unsigned int) + len) return -3; /* short record */
    memcpy(out, f->data + sizeof(unsigned int), len);
    out[len] = 0;
    /* Rewrite the file with the tail. */
    size_t consumed = sizeof(unsigned int) + len;
    size_t tail = f->size - consumed;
    memmove(f->data, f->data + consumed, tail);
    f->size = tail;
    return (long)len;
}

static void test_record_format(void) {
    struct qfile f;
    char buf[64];

    memset(&f, 0, sizeof(f));
    q_append(&f, "ping", 4);
    q_append(&f, "pong!", 5);

    /* getattr-style count: walk records. */
    {
        size_t pos = 0, n = 0;
        while (pos + sizeof(unsigned int) <= f.size) {
            unsigned int l;
            memcpy(&l, f.data + pos, sizeof(l));
            pos += sizeof(unsigned int) + l;
            n++;
        }
        CHECK(n == 2);
    }

    memset(buf, 0, sizeof(buf));
    CHECK(q_dequeue(&f, buf, sizeof(buf)) == 4);
    CHECK(strcmp(buf, "ping") == 0);
    CHECK(f.size == 5 + sizeof(unsigned int));   /* "pong!" remains */

    memset(buf, 0, sizeof(buf));
    CHECK(q_dequeue(&f, buf, sizeof(buf)) == 5);
    CHECK(strcmp(buf, "pong!") == 0);
    CHECK(f.size == 0);                          /* emptied = 0 bytes */

    /* Empty queue -> EAGAIN (-1). */
    CHECK(q_dequeue(&f, buf, sizeof(buf)) == -1);

    /* EMSGSIZE when the buffer is too small (-2). */
    q_append(&f, "12345678", 8);
    CHECK(q_dequeue(&f, buf, 4) == -2);
}

/* ------------------------------------------------------------------ */
/* 2. mq_notify registration state machine (inline)                   */
/* ------------------------------------------------------------------ */

#define MQ_NOTIFY_MAX 8
#define PATH_MAX_T 256

struct reg {
    int active;
    char path[PATH_MAX_T];
    int stop;
    int done;
};
static struct reg regs[MQ_NOTIFY_MAX];

static int reg_find_slot(const char *path) {
    for (int i = 0; i < MQ_NOTIFY_MAX; i++)
        if (regs[i].active && strcmp(regs[i].path, path) == 0)
            return -1;                      /* EBUSY */
    for (int i = 0; i < MQ_NOTIFY_MAX; i++)
        if (!regs[i].active) return i;
    return -2;                              /* ENOSPC */
}

static int reg_register(const char *path) {
    int slot = reg_find_slot(path);
    if (slot < 0) return slot;
    memset(&regs[slot], 0, sizeof(regs[slot]));
    regs[slot].active = 1;
    strncpy(regs[slot].path, path, sizeof(regs[slot].path) - 1);
    return 0;
}

static int reg_deregister(const char *path) {
    for (int i = 0; i < MQ_NOTIFY_MAX; i++) {
        if (regs[i].active && strcmp(regs[i].path, path) == 0) {
            regs[i].stop = 1;
            regs[i].done = 1;               /* watcher exits */
            regs[i].active = 0;
            return 0;
        }
    }
    return 0;                               /* nothing registered */
}

static void test_registration_state_machine(void) {
    memset(regs, 0, sizeof(regs));

    /* First registration on a queue succeeds. */
    CHECK(reg_register("/tmp/mq_a") == 0);
    CHECK(regs[0].active == 1);

    /* A second registration on the SAME queue is EBUSY (-1). */
    CHECK(reg_register("/tmp/mq_a") == -1);

    /* A different queue takes another slot. */
    CHECK(reg_register("/tmp/mq_b") == 0);
    CHECK(regs[1].active == 1);

    /* Deregistration frees the slot; re-registration succeeds (re-arm). */
    CHECK(reg_deregister("/tmp/mq_a") == 0);
    CHECK(regs[0].active == 0);
    CHECK(reg_register("/tmp/mq_a") == 0);

    /* Deregistering an unregistered queue is a no-op success. */
    CHECK(reg_deregister("/tmp/mq_nonexistent") == 0);

    /* Fill the table: slot exhaustion -> ENOSPC (-2). */
    memset(regs, 0, sizeof(regs));
    {
        char p[32];
        int ok = 1;
        for (int i = 0; i < MQ_NOTIFY_MAX; i++) {
            snprintf(p, sizeof(p), "/tmp/mq_%d", i);
            if (reg_register(p) != 0) ok = 0;
        }
        CHECK(ok == 1);
        CHECK(reg_register("/tmp/mq_overflow") == -2);
    }
}

/* ------------------------------------------------------------------ */

int main(void) {
    test_record_format();
    test_registration_state_machine();
    printf("=== Results: %d/%d passed, %d failed ===\n",
           passed, passed + failed, failed);
    return failed ? 1 : 0;
}
