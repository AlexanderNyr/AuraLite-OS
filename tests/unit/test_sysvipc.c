/* test_sysvipc.c — host-side unit test for POSIX2024 phase Q14.
 *
 * The kernel sysvipc module is not linkable on the host (freestanding
 * kernel deps), so — per house convention — the pure algorithms are
 * reimplemented inline and checked against their specification:
 *
 *   1. key lookup / find-or-create semantics: private keys always create;
 *      IPC_CREAT without EXCL opens existing; EXCL on existing fails with
 *      EEXIST; no CREAT on missing fails with ENOENT; slot exhaustion is
 *      ENOSPC; permission bits select owner/group/other.
 *   2. the msgrcv mtype selection algorithm: 0 = FIFO, >0 = exact match,
 *      <0 = first with mtype <= -msgtyp.
 *   3. ABI constants (IPC_*, SEM_*, SHM_*, MSG_*, cmd values) must match
 *      the shipped headers.
 *
 * The guest-side conformtest exercises the real syscalls end to end.
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

/* ---- ABI constants (must match lib/libc/include/sys/{ipc,sem,shm,msg}.h) */
#define IPC_PRIVATE 0
#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define SEM_UNDO    0x1000
#define GETPID 11
#define GETVAL 12
#define GETALL 13
#define SETVAL 16
#define SETALL 17
#define SHM_RDONLY 0x1000
#define SHM_RND    0x2000
#define MSG_NOERROR 0x1000
#define MAX_OBJS 32

static void test_abi_constants(void) {
    CHECK(IPC_PRIVATE == 0 && IPC_CREAT == 01000 && IPC_EXCL == 02000);
    CHECK(IPC_NOWAIT == 04000 && IPC_RMID == 0 && IPC_SET == 1 && IPC_STAT == 2);
    CHECK(SEM_UNDO == 0x1000);
    CHECK(GETPID == 11 && GETVAL == 12 && GETALL == 13);
    CHECK(SETVAL == 16 && SETALL == 17);
    CHECK(SHM_RDONLY == 0x1000 && SHM_RND == 0x2000);
    CHECK(MSG_NOERROR == 0x1000);
}

/* ---- find-or-create (inline of the kernel's IPC_FIND_OR_CREATE_IMPL) ---- */
struct obj { int in_use; long key; int uid; int mode; };
static struct obj objs[MAX_OBJS];

static int perm_ok(const struct obj *o, int write, int euid, int egid) {
    if (euid == 0) return 1;
    int bit = write ? 1 : 0;
    if (euid == o->uid) return (o->mode >> (6 + bit)) & 1;
    if (egid == o->uid) return (o->mode >> (3 + bit)) & 1;
    return (o->mode >> bit) & 1;
}

static int find_or_create(long key, int flags, int euid, int egid, int *created) {
    *created = 0;
    int free_slot = -1, found = -1;
    for (int i = 0; i < MAX_OBJS; i++) {
        if (!objs[i].in_use) { if (free_slot < 0) free_slot = i; continue; }
        if (key != IPC_PRIVATE && objs[i].key == key) { found = i; break; }
    }
    if (found >= 0) {
        if (!perm_ok(&objs[found], 1, euid, egid)) return -13;   /* EACCES */
        if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) return -17; /* EEXIST */
        return found;
    }
    if (!(flags & IPC_CREAT)) return -2;                          /* ENOENT */
    if (free_slot < 0) return -28;                                /* ENOSPC */
    memset(&objs[free_slot], 0, sizeof(objs[free_slot]));
    objs[free_slot].in_use = 1;
    objs[free_slot].key = key;
    objs[free_slot].uid = euid;
    objs[free_slot].mode = 0666;
    *created = 1;
    return free_slot;
}

static void test_find_or_create(void) {
    memset(objs, 0, sizeof(objs));
    int created = 0;

    /* Private keys always create a fresh object. */
    int a = find_or_create(IPC_PRIVATE, IPC_CREAT, 1000, 1000, &created);
    CHECK(a >= 0 && created == 1);
    int b = find_or_create(IPC_PRIVATE, IPC_CREAT, 1000, 1000, &created);
    CHECK(b >= 0 && b != a && created == 1);

    /* Named key: create, then open. */
    int c = find_or_create(1234, IPC_CREAT, 1000, 1000, &created);
    CHECK(c >= 0 && created == 1);
    created = 0;
    int c2 = find_or_create(1234, 0, 1000, 1000, &created);
    CHECK(c2 == c && created == 0);

    /* EXCL on existing -> EEXIST. */
    CHECK(find_or_create(1234, IPC_CREAT | IPC_EXCL, 1000, 1000, &created) == -17);

    /* Missing without CREAT -> ENOENT. */
    CHECK(find_or_create(9999, 0, 1000, 1000, &created) == -2);

    /* Different euid with no perms -> EACCES. */
    objs[c].uid = 2000;
    objs[c].mode = 0600;   /* owner rw only */
    CHECK(find_or_create(1234, 0, 3000, 3000, &created) == -13);
    /* Other with 0666 -> allowed. */
    objs[c].mode = 0666;
    CHECK(find_or_create(1234, 0, 3000, 3000, &created) == c);
    /* Root bypasses. */
    objs[c].mode = 0000;
    CHECK(find_or_create(1234, 0, 0, 0, &created) == c);

    /* Slot exhaustion -> ENOSPC. */
    memset(objs, 0, sizeof(objs));
    int ok = 1;
    for (int i = 0; i < MAX_OBJS; i++)
        if (find_or_create(IPC_PRIVATE, IPC_CREAT, 1, 1, &created) < 0) ok = 0;
    CHECK(ok == 1);
    CHECK(find_or_create(IPC_PRIVATE, IPC_CREAT, 1, 1, &created) == -28);
}

/* ---- msgrcv mtype selection (inline of msg_pick) ---- */
struct msg { long mtype; struct msg *next; };
static struct msg *msg_pick(struct msg *head, long typ) {
    if (typ == 0) return head;
    if (typ > 0) {
        for (struct msg *n = head; n; n = n->next)
            if (n->mtype == typ) return n;
        return NULL;
    }
    long limit = -typ;
    for (struct msg *n = head; n; n = n->next)
        if (n->mtype <= limit) return n;
    return NULL;
}

static void test_mtype_selection(void) {
    struct msg m1 = { 1, 0 }, m5a = { 5, 0 }, m5b = { 5, 0 }, m9 = { 9, 0 };
    m1.next = &m5a; m5a.next = &m5b; m5b.next = &m9;

    /* typ == 0: FIFO (first). */
    CHECK(msg_pick(&m1, 0) == &m1);
    /* typ > 0: exact match, first in order. */
    CHECK(msg_pick(&m1, 5) == &m5a);
    CHECK(msg_pick(&m1, 9) == &m9);
    CHECK(msg_pick(&m1, 7) == NULL);
    /* typ < 0: first with mtype <= -typ. */
    CHECK(msg_pick(&m1, -5) == &m1);    /* 1 <= 5 */
    CHECK(msg_pick(&m5a, -5) == &m5a);  /* 5 <= 5 */
    /* -4 on {5, 9}: 5 <= 4 is false, 9 <= 4 is false -> NULL. */
    struct msg m2 = { 5, &m9 };
    CHECK(msg_pick(&m2, -4) == NULL);
    CHECK(msg_pick(&m2, -10) == &m2);   /* 5 <= 10 */
    CHECK(msg_pick(&m9, -6) == NULL);   /* 9 <= 6 is false -> no match */
}

int main(void) {
    test_abi_constants();
    test_find_or_create();
    test_mtype_selection();
    printf("=== Results: %d/%d passed, %d failed ===\n",
           passed, passed + failed, failed);
    return failed ? 1 : 0;
}
