/* tests/unit/test_fsync_m3.c — FSMODERN M3: the durability contract.
 *
 * M3's acceptance criterion is a promise, and promises are exactly what a
 * boot log cannot check: a QEMU run shows that ONE file survived ONE power
 * cut, not that vfs_fsync() routes a request to the filesystem that owns the
 * vnode, reports its answer instead of its own opinion, falls back to the
 * mount-wide .sync when the per-inode hook is absent, and refuses (-EOPNOTSUPP)
 * when the filesystem has neither.  Each of those is one branch, and the wrong
 * branch is invisible in a passing crash run:
 *
 *   - if step 2 silently ignored .fsync's return, a filesystem that FAILED to
 *     sync would report success -- the crash test would still pass for a
 *     filesystem that happened to work;
 *   - if step 3 picked the wrong mount, two filesystems sharing the cache
 *     would sync the wrong one, which on this tree is the difference between
 *     /fat persisting and not;
 *   - if step 4 returned 0, "we cannot promise" would look like "we promised;
 *     the current code's predecessor did exactly that, for every filesystem.
 *
 * What is REAL here and what is a stub, and why:
 *
 *   - kernel/fs/vfsmount.c is the SHIPPED file.  The mount table, the
 *     owner-by-ops-identity lookup and the fallback routing are the parts M3
 *     added, so they get no reimplementation to disagree with.
 *   - vfs_fsync/vfs_fdatasync are a verbatim transcription of the bodies in
 *     kernel/fs/vfs.c (which cannot link on the host: it drags in the thread
 *     layer, pipes, page cache and slab).  Same ordering, same return policy.
 *     The transcription is marked and keeps the shipped function's comments
 *     so that a later change to vfs.c that forgets this file is visible as a
 *     diff between the two.
 *   - page_cache_flush/_range are no-ops that COUNT calls: they are the only
 *     observable in a 5-line function, and "mapped pages are flushed first"
 *     is a claim somebody will eventually reorder.
 *
 * The test is built with -Werror and links against nothing but libc, so it
 * costs a second in `make test-unit` and names the branch that is wrong.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kernel/fs/vfs.h"
#include "kernel/fs/vfsmount.h"
#include "kernel/lib/errno.h"

/* ---- test plumbing ------------------------------------------------- */

static int passed = 0, failed = 0;
#define RUN(f) do { int b_ = failed; f(); if (failed == b_) { passed++; \
                       printf("  ok   %s\n", #f); } } while (0)
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); \
                       failed++; } } while (0)
#define CHECK_EQ(a, e) do { long a_ = (long)(a), e_ = (long)(e); \
        if (a_ != e_) { printf("  FAIL L%d: %s = %ld, want %ld\n", \
                               __LINE__, #a, a_, e_); failed++; } } while (0)

/* ---- stubs the SHIPPED vfsmount.c needs ---------------------------- *
 *
 * It includes kernel/lib/kprintf.h and kernel/lib/string.h.  Host libc
 * already provides the string functions with identical semantics, so only
 * kprintf is stubbed -- and it records what the mount receipt SAID, which
 * is half of M3's acceptance ("the receipt names WEAK").
 */

#define RECEIPT_MAX 8
static char receipts[RECEIPT_MAX][320];
static int  receipt_n;

/* The SHIPPED vfsm_mount calls kprintf(fmt, path) twice: the plain
 * "[vfs] mounted '%s'\n" shape and the WEAK one, whose sentence is baked
 * into the format string.  Capturing the FORMAT is exactly what the test
 * needs -- the mount path is the only argument, and whether the WEAK
 * sentence was in it is the claim being checked. */
void kprintf(const char *fmt, ...)
{
    if (receipt_n >= RECEIPT_MAX) return;
    snprintf(receipts[receipt_n], sizeof(receipts[0]), "%s", fmt);
    receipt_n++;
}

static int receipt_says(int i, const char *needle)
{
    return i < receipt_n && strstr(receipts[i], needle) != NULL;
}

/* ---- the page cache seam -------------------------------------------- */

static int page_flushes, range_flushes;

int page_cache_flush_range(struct ofd *file, uint64_t start, uint64_t end)
{
    (void)file; (void)start; (void)end;
    range_flushes++;
    return 0;
}

void page_cache_flush(struct ofd *file)
{
    (void)file;
    page_flushes++;
}

/* ---- vfs_fsync: transcribed from kernel/fs/vfs.c -------------------- *
 *
 * Keep in sync with vfs.c's body.  The transcription exists because vfs.c
 * pulls in pipes, the scheduler and slab allocation; the function under
 * test needs none of them.
 */

int vfs_fsync(struct ofd *o, int mode)
{
    if (!o || !o->vn) return -EINVAL;

    /* 1. Mapped pages. */
    page_cache_flush(o);

    /* 2. The filesystem's own per-inode sync, if it has one. */
    if (o->vn->ops && o->vn->ops->fsync)
        return o->vn->ops->fsync(o->vn, mode);

    /* 3. The mount-wide fallback, resolved through the mount table.  Same
     *    positive sentinels as vfs.c: see vfsmount.h. */
    int rc = vfsm_sync_owner_of(o->vn);
    if (rc == 0)                  return 0;
    if (rc == VFSM_SYNC_NO_HOOK)  return -EOPNOTSUPP;
    if (rc == VFSM_SYNC_NO_OWNER) return 0;
    return rc;
}

int vfs_fdatasync(struct ofd *o)
{
    return vfs_fsync(o, SYNC_DATA);
}

/* ---- fixtures ------------------------------------------------------- */

struct fake_fs {
    int        fsync_calls, fsync_mode, fsync_rc;
    int        sync_calls, sync_rc;
    void      *sync_ctx;        /* what .sync was handed */
};

static struct fake_fs fs_a, fs_b;

static int fa_fsync(struct vnode *vn, int mode)
{
    (void)vn;
    fs_a.fsync_calls++;
    fs_a.fsync_mode = mode;
    return fs_a.fsync_rc;
}

static int fa_sync(void *ctx) { fs_a.sync_calls++; fs_a.sync_ctx = ctx;
                                return fs_a.sync_rc; }
static int fb_sync(void *ctx) { fs_b.sync_calls++; fs_b.sync_ctx = ctx;
                                return fs_b.sync_rc; }

/* Table A: has BOTH a per-inode .fsync and a mount-wide .sync. */
static const struct vfs_ops ops_a = {
    .fsync      = fa_fsync,
    .sync       = fa_sync,
    .durability = FS_DURABILITY_EXACT,
};

/* Table B: mount-wide .sync only (the FAT32/diskfs shape M3 relies on). */
static const struct vfs_ops ops_b = {
    .sync       = fb_sync,
    .durability = FS_DURABILITY_EXACT,
};

/* Table C: NEITHER -- the caller must be told, not reassured. */
static const struct vfs_ops ops_c = {
    .durability = FS_DURABILITY_WEAK,
};

/* Table D: never mounted.  A vnode pointing at it is an ORPHAN, and the
 * contract says "nothing to do" rather than "synced".  It has to be its own
 * table: pointing the orphan at ops_a made the test pass or fail depending
 * on which mount a neighbouring test had left in the (static) mount table,
 * which is precisely the accident this fixture must not have. */
static const struct vfs_ops ops_d = {
    .durability = FS_DURABILITY_EXACT,
};

static struct vnode vn_a, vn_b, vn_c, vn_d, vn_orphan;
static struct ofd   ofd_a, ofd_b, ofd_c, ofd_orphan;

static void fixtures_reset(void)
{
    memset(&fs_a, 0, sizeof(fs_a));
    memset(&fs_b, 0, sizeof(fs_b));
    memset(&vn_a, 0, sizeof(vn_a));
    memset(&vn_b, 0, sizeof(vn_b));
    memset(&vn_c, 0, sizeof(vn_c));
    memset(&vn_d, 0, sizeof(vn_d));
    memset(&vn_orphan, 0, sizeof(vn_orphan));

    vn_a.ops = &ops_a; vn_b.ops = &ops_b; vn_c.ops = &ops_c; vn_d.ops = &ops_d;
    vn_orphan.ops = &ops_d;   /* a real table, but no mount owns it */
    (void)vn_d;

    memset(&ofd_a, 0, sizeof(ofd_a));
    memset(&ofd_b, 0, sizeof(ofd_b));
    memset(&ofd_c, 0, sizeof(ofd_c));
    memset(&ofd_orphan, 0, sizeof(ofd_orphan));
    ofd_a.vn = &vn_a; ofd_b.vn = &vn_b; ofd_c.vn = &vn_c;
    ofd_orphan.vn = &vn_orphan;

    page_flushes = range_flushes = 0;
    receipt_n = 0;
}

/* ---- 1. per-inode .fsync wins, and its ANSWER is the answer --------- */

static void t_per_inode_is_authoritative(void)
{
    fixtures_reset();
    CHECK_EQ(vfsm_mount("/inode", &ops_a, &fs_a), 0);

    CHECK_EQ(vfs_fsync(&ofd_a, SYNC_DATA), 0);
    CHECK_EQ(fs_a.fsync_calls, 1);
    CHECK_EQ(fs_a.fsync_mode, SYNC_DATA);
    CHECK_EQ(fs_a.sync_calls, 0);        /* the fallback must NOT also run */
    CHECK_EQ(page_flushes, 1);           /* mapped pages first */

    /* A failing .fsync must be reported as failure even though .sync would
     * have succeeded: reporting the fallback's success would overrule the
     * filesystem about its own inode, which is the exact lie M3 exists to
     * stop telling. */
    fs_a.fsync_rc = -EIO;
    CHECK_EQ(vfs_fsync(&ofd_a, SYNC_DATA), -EIO);
    CHECK_EQ(fs_a.sync_calls, 0);
}

/* ---- 2. no per-inode hook -> the mount-wide .sync, with its fs_data --- */

static void t_mount_wide_fallback(void)
{
    fixtures_reset();
    CHECK_EQ(vfsm_mount("/fallback", &ops_b, &fs_b), 0);

    CHECK_EQ(vfs_fsync(&ofd_b, SYNC_META), 0);
    CHECK_EQ(fs_b.sync_calls, 1);
    /* The context must be the mount's own fs_data: calling .sync(NULL) is
     * what would happen if this were resolved through vnode->mount_data,
     * which nothing in the tree fills. */
    CHECK(fs_b.sync_ctx == &fs_b);
    CHECK_EQ(page_flushes, 1);

    /* A FAILING mount-wide .sync must reach the caller.  Before
     * vfsm_sync_owner_of returned positive sentinels, a driver's -1 here
     * was indistinguishable from "no owner" and vfs_fsync answered 0 --
     * fsync() reporting success for a flush that failed. */
    fs_b.sync_rc = -1;
    CHECK_EQ(vfs_fsync(&ofd_b, SYNC_DATA), -EIO);
    fs_b.sync_rc = -EIO;
    CHECK_EQ(vfs_fsync(&ofd_b, SYNC_DATA), -EIO);
}

/* ---- 3. SYNC_META reaches the filesystem as SYNC_META ---------------- */

static void t_mode_is_passed_through(void)
{
    fixtures_reset();
    CHECK_EQ(vfsm_mount("/mode", &ops_a, &fs_a), 0);

    CHECK_EQ(vfs_fsync(&ofd_a, SYNC_META), 0);
    CHECK_EQ(fs_a.fsync_mode, SYNC_META);
    CHECK_EQ(vfs_fsync(&ofd_a, SYNC_DATA), 0);
    CHECK_EQ(fs_a.fsync_mode, SYNC_DATA);
    CHECK_EQ(fs_a.fsync_calls, 2);
}

/* ---- 4. neither hook -> -EOPNOTSUPP, never 0 ------------------------ */

static void t_no_hook_refuses(void)
{
    fixtures_reset();
    CHECK_EQ(vfsm_mount("/nohook", &ops_c, NULL), 0);

    CHECK_EQ(vfs_fsync(&ofd_c, SYNC_DATA), -EOPNOTSUPP);
    /* fdatasync is the same call: a filesystem that cannot sync cannot
     * datasync either, and pretending otherwise is how /tmp used to claim
     * durability it did not have. */
    CHECK_EQ(vfs_fdatasync(&ofd_c), -EOPNOTSUPP);
}

/* ---- 5. owner lookup is by ops IDENTITY, not by first match ---------- */

static void t_owner_is_by_ops_identity(void)
{
    fixtures_reset();
    /* Two mounts, in this order, so "return the first mount" and "return
     * the owning mount" give different answers. */
    CHECK_EQ(vfsm_mount("/first", &ops_a, &fs_a), 0);
    CHECK_EQ(vfsm_mount("/second", &ops_b, &fs_b), 0);

    CHECK_EQ(vfsm_owner_of(&vn_b), vfsm_owner_of(&vn_b));
    CHECK(vfsm_owner_of(&vn_b) != vfsm_owner_of(&vn_a));

    /* The decisive one: syncing vn_b must touch B's .sync, not A's. */
    CHECK_EQ(vfs_fsync(&ofd_b, SYNC_DATA), 0);
    CHECK_EQ(fs_b.sync_calls, 1);
    CHECK_EQ(fs_a.sync_calls, 0);
    CHECK(fs_b.sync_ctx == &fs_b);

    /* A vnode whose ops table is in NO mount: the contract says "nothing
     * to do", i.e. success -- the filesystem it belongs to is not mounted
     * here, so there is no mount whose .sync could be running. */
    CHECK_EQ(vfsm_owner_of(&vn_orphan), -1);
    CHECK_EQ(vfsm_sync_owner_of(&vn_orphan), VFSM_SYNC_NO_OWNER);
}

/* ---- 6. the mount receipt names WEAK, and only WEAK ------------------ */

static void t_receipt_names_weak(void)
{
    /* The receipt is what a user reads in the boot log, so it is part of
     * the acceptance criterion, not decoration.  This goes through the
     * SHIPPED vfsm_mount: whatever string it builds is what the boot log
     * gets. */
    fixtures_reset();
    CHECK_EQ(vfsm_mount("/weak", &ops_c, NULL), 0);       /* WEAK */
    CHECK_EQ(vfsm_mount("/exact", &ops_b, &fs_b), 0);     /* EXACT */

    CHECK_EQ(receipt_n, 2);
    CHECK(receipt_says(0, "DURABILITY WEAK"));
    CHECK(receipt_says(0, "may be lost on a power cut"));
    CHECK(receipt_says(0, "call sync before relying on it"));
    /* EXACT says nothing: nine EXACT mounts that each printed a line would
     * train a reader to skip the one that matters. */
    CHECK(!receipt_says(1, "DURABILITY"));

    /* And the class names, which the plan's wording uses verbatim. */
    CHECK(strcmp(fs_durability_name(FS_DURABILITY_EXACT), "EXACT") == 0);
    CHECK(strcmp(fs_durability_name(FS_DURABILITY_WEAK),  "WEAK")  == 0);
    CHECK(strcmp(fs_durability_name(FS_DURABILITY_VOLATILE), "VOLATILE") == 0);

    /* An ops table that does not say anything is EXACT by the documented
     * default -- the property that makes "the weak ones declare it" a
     * meaningful sentence. */
    static const struct vfs_ops silent = { 0 };
    CHECK_EQ(silent.durability, FS_DURABILITY_EXACT);

    /* VOLATILE (tmpfs and the volumes built on it) gets its OWN sentence.
     * "call sync before relying on it" is true for WEAK and FALSE for a
     * filesystem with no medium: sync succeeds there and changes nothing.
     * Telling a user to do something that cannot work is the same class of
     * defect as silent data loss, just slower. */
    static const struct vfs_ops volatile_ops = {
        .durability = FS_DURABILITY_VOLATILE,
    };
    receipt_n = 0;
    CHECK_EQ(vfsm_mount("/volatile", &volatile_ops, NULL), 0);
    CHECK_EQ(receipt_n, 1);
    CHECK(receipt_says(0, "DURABILITY VOLATILE"));
    CHECK(receipt_says(0, "lives in RAM"));
    CHECK(!receipt_says(0, "call sync before relying on it"));
}

/* ---- 7. NULL/edge inputs -------------------------------------------- */

static void t_edges(void)
{
    fixtures_reset();
    CHECK_EQ(vfs_fsync(NULL, SYNC_DATA), -EINVAL);
    struct ofd empty; memset(&empty, 0, sizeof(empty));
    CHECK_EQ(vfs_fsync(&empty, SYNC_DATA), -EINVAL);

    /* No mount table entry and no ops at all: vfsm_owner_of must not walk
     * off the table. */
    CHECK_EQ(vfsm_owner_of(NULL), -1);
    CHECK_EQ(vfsm_sync_owner_of(NULL), VFSM_SYNC_NO_OWNER);
    struct vnode noops; memset(&noops, 0, sizeof(noops));
    CHECK_EQ(vfsm_owner_of(&noops), -1);
    CHECK_EQ(vfsm_sync_owner_of(&noops), VFSM_SYNC_NO_OWNER);
}

int main(void)
{
    printf("test_fsync_m3: FSMODERN M3 durability contract\n");
    RUN(t_per_inode_is_authoritative);
    RUN(t_mount_wide_fallback);
    RUN(t_mode_is_passed_through);
    RUN(t_no_hook_refuses);
    RUN(t_owner_is_by_ops_identity);
    RUN(t_receipt_names_weak);
    RUN(t_edges);

    printf("test_fsync_m3: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
