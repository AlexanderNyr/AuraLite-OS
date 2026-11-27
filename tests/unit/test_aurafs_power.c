/* M6p: deterministic volatile-device-cache model, NOT hardware proof.
 * Reuse the actual-driver stubs without running the historical suite here. */
#define main aurafs_historical_main
#include "test_aurafs_m6.c"
#undef main
#include <setjmp.h>
#include <sys/wait.h>
#include <unistd.h>

static uint8_t stable[DISK_SIZE], seed[DISK_SIZE];
static uint8_t dirty[DISK_SIZE / 512];
static jmp_buf crash_point;
static int armed, event_count, cut_at, model_flushes, fail_barrier;
static int persist_forward;
static int commit_policy; /* 0 none, 1 half dirty sectors, 2 all */
static unsigned model_writes;
static const char *last_event;
static uint64_t old_generation;
#define POWER_LENGTH 9000
static uint8_t old_bytes[POWER_LENGTH], new_bytes[POWER_LENGTH];
/* M6q: reachable degraded seeds from an aborted first publication. */
static uint8_t healthy_seed[DISK_SIZE], aborted_bytes[POWER_LENGTH];
static uint8_t survivor_image[BTRFS_NODE_SIZE];
static int repeated_mode;
static uint64_t survivor_offset, replay_cursor;

static void power_event(const char *name) {
    if (!armed) return;
    last_event = name;
    if (++event_count == cut_at) longjmp(crash_point, 1);
}
static int power_read(void *ctx, uint64_t lba, uint32_t n, void *buf) {
    (void)ctx;
    if (lba > DISK_SIZE / 512 || n > DISK_SIZE / 512 - lba) return -EIO;
    memcpy(buf, disk + lba * 512, n * 512); /* reads see volatile cache */
    if (lba == BTRFS_SUPER_OFFSET / 512 || lba == AURAFS_MIRROR_OFFSET / 512)
        power_event("checkpoint-read");
    return 0;
}
static int power_write(void *ctx, uint64_t lba, uint32_t n, const void *buf) {
    (void)ctx;
    if (lba > DISK_SIZE / 512 || n > DISK_SIZE / 512 - lba) return -EIO;
    power_event("before-write");
    memcpy(disk + lba * 512, buf, n * 512);
    memset(dirty + lba, 1, n);
    model_writes++;
    power_event("accepted-volatile-write");
    return 0; /* NO automatic persistence */
}
static int power_flush(void *ctx) {
    (void)ctx;
    model_flushes++;
    power_event("before-flush");
    unsigned pending = 0, committed = 0;
    for (unsigned i = 0; i < sizeof(dirty); i++) pending += dirty[i] != 0;
    int failed = model_flushes == fail_barrier;
    unsigned budget = failed ? (commit_policy == 0 ? 0 :
                                commit_policy == 1 ? pending / 2 : pending) : pending;
    /* Both LBA orders are tested. Forward can publish a checkpoint header
     * before higher-addressed data; reverse can persist its SHA trailer first.
     * Each copied 512-byte sector is the model's atomic persistence unit. */
    for (unsigned pos = 0; pos < sizeof(dirty) && committed < budget; pos++) {
        unsigned i = persist_forward ? pos : (unsigned)sizeof(dirty) - 1 - pos;
        if (!dirty[i]) continue;
        memcpy(stable + i * 512, disk + i * 512, 512);
        dirty[i] = 0; committed++;
        power_event("persisted-sector-during-flush");
    }
    power_event("after-flush-effects");
    return failed ? -EIO : 0;
}
static void lose_power(void) {
    armed = 0;
    memcpy(disk, stable, sizeof(disk)); /* discard ALL unpersisted writes */
    memset(dirty, 0, sizeof(dirty));
    bc_init(); /* discard filesystem cache too; no clean unmount */
    allow_format = allow_dual = 0;
    fail_barrier = 0;
}
static void restore_seed(int initial) {
    armed = 0; cut_at = event_count = model_flushes = fail_barrier = 0;
    commit_policy = 0;
    if (initial) memset(stable, 0, sizeof(stable));
    else memcpy(stable, seed, sizeof(stable));
    lose_power();
    if (initial) allow_format = allow_dual = 1;
    else assert(btrfs_init(0) == 0 && bm.dual);
}
static void check_payload(const uint8_t *expected) {
    uint8_t out[POWER_LENGTH];
    struct vnode *v = btrfs_lookup(NULL, "power-file");
    assert(v && btrfs_read(v, 0, out, sizeof(out)) == sizeof(out));
    assert(!memcmp(out, expected, sizeof(out)));
}
static void replace_payload(void) {
    struct vnode *v = btrfs_lookup(NULL, "power-file");
    assert(v && btrfs_write(v, 0, new_bytes, sizeof(new_bytes)) == sizeof(new_bytes));
}
static void check_recovery(int initial, int expected_new, int require_mounted) {
    lose_power();
    if (repeated_mode) {
        /* A second interrupted publication must never overwrite the only
         * checkpoint that was valid when recovery started. */
        assert(!memcmp(stable + survivor_offset, survivor_image, sizeof(survivor_image)));
    }
    unsigned before = model_writes;
    int a = aurafs_validate_super(stable + BTRFS_SUPER_OFFSET, DISK_SIZE) == 0;
    int b = aurafs_validate_super(stable + AURAFS_MIRROR_OFFSET, DISK_SIZE) == 0;
    int rc = btrfs_init(0);
    assert(model_writes == before); /* selection never repairs/reformats */
    assert((rc == 0) == (a || b));
    if (require_mounted) assert(rc == 0);
    if (initial && rc < 0) { assert(!bm.mounted); return; }
    assert(rc == 0 && bm.dual);
    if (!initial) {
        uint64_t generation = bm.generation - 1;
        assert(generation == old_generation || generation == old_generation + 1);
        if (expected_new >= 0) assert(generation == old_generation + (unsigned)expected_new);
        check_payload(generation == old_generation ? old_bytes : new_bytes);
    }
    int recovered_new = !initial && bm.generation - 1 == old_generation + 1;
    /* Prove surviving metadata can be extended, not just SHA-validated. */
    struct vnode *v = btrfs_create(NULL, "after-power");
    assert(v && btrfs_write(v, 0, "ALIVE", 5) == 5);
    assert(btrfs_sync(NULL) == 0);
    lose_power(); before = model_writes;
    assert(btrfs_init(0) == 0 && model_writes == before);
    char out[8]; v = btrfs_lookup(NULL, "after-power");
    assert(v && btrfs_read(v, 0, out, sizeof(out)) == 5 && !memcmp(out, "ALIVE", 5));
    if (!initial) check_payload(recovered_new ? new_bytes : old_bytes);
}
/* Each child isolates retained cache allocations and non-local power cuts.
 * The jump abandons all stack frames; no driver code runs after the cut. */
static void run_case(int initial, int cut, int barrier, int policy) {
    pid_t pid = fork(); assert(pid >= 0);
    if (pid == 0) {
        restore_seed(initial);
        cut_at = cut; fail_barrier = barrier; commit_policy = policy;
        armed = 1;
        if (setjmp(crash_point) == 0) {
            int rc;
            if (initial) rc = btrfs_init(0);
            else { replace_payload(); rc = btrfs_sync(NULL); }
            armed = 0;
            if (cut) assert(!"requested crash point was not reached");
            if (barrier) assert(rc < 0);
            else assert(rc == 0);
        }
        int expected = cut ? -1 : !barrier || (barrier == 2 && policy == 2);
        /* A half-persisted retry can complete an older torn image: same
         * generation/root addresses, newly durable data from the pre-barrier.
         * Failed partial publication may therefore recover OLD or NEW. */
        if (repeated_mode && barrier == 2 && policy == 1) expected = -1;
        if (initial && barrier) {
            int valid = (aurafs_validate_super(stable + BTRFS_SUPER_OFFSET, DISK_SIZE) == 0) +
                        (aurafs_validate_super(stable + AURAFS_MIRROR_OFFSET, DISK_SIZE) == 0);
            int expected_valid = barrier <= 2 ? 0 : barrier == 3 ?
                                 (policy == 2 ? 1 : 0) : (policy == 2 ? 2 : 1);
            assert(valid == expected_valid);
        }
        check_recovery(initial, expected, !initial || (!cut && !barrier));
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "power case failed: initial=%d cut=%d barrier=%d policy=%d forward=%d status=%d\n",
                initial, cut, barrier, policy, persist_forward, status);
        assert(!"power case child failed");
    }
}
static int count_events(int initial) {
    restore_seed(initial); armed = 1;
    if (initial) assert(btrfs_init(0) == 0);
    else { replace_payload(); assert(btrfs_sync(NULL) == 0); }
    armed = 0;
    return event_count;
}
static void model_selftest(void) {
    restore_seed(1);
    uint8_t in[512], out[512]; memset(in, 0x5a, sizeof(in));
    assert(power_write(NULL, 0, 1, in) == 0);
    assert(power_read(NULL, 0, 1, out) == 0 && !memcmp(in, out, sizeof(in)));
    assert(stable[0] == 0); lose_power(); assert(disk[0] == 0);
    assert(power_write(NULL, 0, 1, in) == 0 && power_flush(NULL) == 0);
    lose_power(); assert(!memcmp(disk, in, sizeof(in)));
}
static void test_repeated_power(void) {
    unsigned cases = 0, seeds = 0;
    int replay_events = 0;
    for (int flip = 0; flip <= 1; flip++) {
        repeated_mode = 0;
        restore_seed(1); assert(btrfs_init(0) == 0);
        struct vnode *v = btrfs_create(NULL, "power-file");
        assert(v && btrfs_write(v, 0, old_bytes, sizeof(old_bytes)) == sizeof(old_bytes));
        assert(btrfs_sync(NULL) == 0);
        if (flip) assert(btrfs_sync(NULL) == 0);
        old_generation = bm.generation - 1;
        survivor_offset = bm.active_super;
        replay_cursor = bm.next_free_block;
        memcpy(healthy_seed, stable, sizeof(stable));
        memcpy(survivor_image, stable + survivor_offset, sizeof(survivor_image));
        for (int first_order = 0; first_order <= 1; first_order++) {
            memcpy(seed, healthy_seed, sizeof(seed)); restore_seed(0);
            persist_forward = first_order;
            v = btrfs_lookup(NULL, "power-file");
            assert(v && btrfs_write(v, 0, aborted_bytes, sizeof(aborted_bytes)) == sizeof(aborted_bytes));
            uint64_t aborted_end = bm.next_free_block;
            assert(aborted_end > replay_cursor);
            model_flushes = 0; fail_barrier = 2; commit_policy = 1;
            assert(btrfs_sync(NULL) < 0); /* data durable, target checkpoint torn */
            lose_power();
            unsigned before = model_writes;
            int barriers_before = model_flushes;
            int valid = (aurafs_validate_super(stable + BTRFS_SUPER_OFFSET, DISK_SIZE) == 0) +
                        (aurafs_validate_super(stable + AURAFS_MIRROR_OFFSET, DISK_SIZE) == 0);
            assert(valid == 1);
            assert(btrfs_init(0) == 0 && model_writes == before && model_flushes == barriers_before);
            assert(bm.active_super == survivor_offset && bm.generation - 1 == old_generation);
            assert(bm.next_free_block == replay_cursor);
            assert(!memcmp(stable + survivor_offset, survivor_image, sizeof(survivor_image)));
            assert(memcmp(stable + replay_cursor, healthy_seed + replay_cursor,
                          aborted_end - replay_cursor)); /* nonzero orphan suffix */
            check_payload(old_bytes); /* NEVER the abandoned first pattern */
            memcpy(seed, stable, sizeof(seed));
            repeated_mode = 1; seeds++;
            int events = count_events(0);
            if (!replay_events) replay_events = events;
            else assert(events == replay_events);
            for (persist_forward = 0; persist_forward <= 1; persist_forward++) {
                for (int cut = 1; cut <= events; cut++) {
                    run_case(0, cut, 0, 0); cases++;
                }
                run_case(0, 0, 0, 0); cases++;
                for (int barrier = 1; barrier <= 2; barrier++) {
                    for (int policy = 0; policy <= 2; policy++) {
                        run_case(0, 0, barrier, policy); cases++;
                    }
                }
            }
            repeated_mode = 0;
        }
    }
    printf("AuraFS M6q: PASS (%u second-crash cases, %u reachable single-copy seeds, %d replay events; both slots/first-and-second sector orders; survivor unchanged, no aborted-pattern leakage)\n",
           cases, seeds, replay_events);
}

int main(void) {
    const struct blkdev_ops ops = { .read = power_read, .write = power_write,
                                   .sector_count = capacity, .flush = power_flush };
    assert(blkdev_register("aurafs-power", &ops, NULL, 512) == 0);
    for (unsigned i = 0; i < POWER_LENGTH; i++) {
        old_bytes[i] = (uint8_t)(i * 17 + 3);
        new_bytes[i] = (uint8_t)(i * 7 + 91);
        aborted_bytes[i] = (uint8_t)(i * 29 + 163);
    }
    model_selftest();
    unsigned cases = 0;
    int initial_events = 0, sync_events = 0;
    for (int flip = 0; flip <= 1; flip++) {
        restore_seed(1); assert(btrfs_init(0) == 0);
        struct vnode *v = btrfs_create(NULL, "power-file");
        assert(v && btrfs_write(v, 0, old_bytes, sizeof(old_bytes)) == sizeof(old_bytes));
        assert(btrfs_sync(NULL) == 0);
        if (flip) assert(btrfs_sync(NULL) == 0); /* NEW targets the other slot */
        old_generation = bm.generation - 1;
        memcpy(seed, stable, sizeof(seed));
        if (!flip) initial_events = count_events(1);
        int events_now = count_events(0);
        if (!flip) sync_events = events_now;
        else assert(events_now == sync_events);
        assert(initial_events > 0 && sync_events > 0);
        for (persist_forward = 0; persist_forward <= 1; persist_forward++) {
            /* Initial-format cases are independent of OLD slot orientation. */
            for (int initial = 0; initial <= (flip ? 0 : 1); initial++) {
                int events = initial ? initial_events : sync_events;
                for (int cut = 1; cut <= events; cut++) {
                    run_case(initial, cut, 0, 0); cases++;
                }
                run_case(initial, 0, 0, 0); cases++; /* success must survive */
                int barriers = initial ? 4 : 2;
                for (int barrier = 1; barrier <= barriers; barrier++) {
                    for (int policy = 0; policy <= 2; policy++) {
                        run_case(initial, 0, barrier, policy); cases++;
                    }
                }
            }
        }
    }
    printf("AuraFS M6p: PASS (%u power-loss/failing-FLUSH cases; initial events=%d sync events=%d; both target slots and LBA orders, volatile reads != persistence, sector tears, read-only recovery, byte-exact OLD/NEW)\n",
           cases, initial_events, sync_events);
    test_repeated_power();
    return 0;
}
