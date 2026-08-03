/*
 * test_initrd_allocfail.c — host-side unit test for FIX_R4: when the vnode
 * pool cannot be allocated, initrd_init() must REPORT failure instead of
 * dereferencing the NULL pool.
 *
 * WHY THIS TEST EXISTS
 *
 * initrd_init() allocates one kmalloc()'d pool of vnodes after parsing the
 * tar image, and up to FIX_R4 the result was never checked: a failed
 * kmalloc made the very next memset() write to NULL.  kmalloc only fails
 * when the kernel heap is exhausted, so the bug was latent — but the boot
 * mounts the initrd unconditionally, so the latent path is a guaranteed
 * page fault exactly when the kernel can least afford it.
 *
 * The real kernel source is compiled in (as in test_initrd_dirs.c), with
 * only kmalloc/kfree/kprintf stubbed; the kmalloc stub can be told to fail.
 * Pre-fix this test would not report a failure — it would die of SIGSEGV,
 * which is precisely what "reports failure instead of faulting" means.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/fs/vfs.h"
#include "kernel/fs/initrd.h"

/* ---- stubs the parser needs ---- */

static int kmalloc_may_fail = 0;
void *kmalloc(uint64_t size) {
    if (kmalloc_may_fail) return NULL;
    return malloc((size_t)size);
}
void  kfree(void *p) { free(p); }

static int kprintf_quiet = 1;
void kprintf(const char *fmt, ...) { if (!kprintf_quiet) { (void)fmt; } }

/* ---- test harness ---- */

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define CHECK(c) do {                                                   \
    if (!(c)) { printf("    L%d: %s\n", __LINE__, #c); return 0; }      \
} while (0)

/* ---- USTAR image construction (same layout as test_initrd_dirs.c) ---- */

#define TAR_BLOCK 512

struct tar_builder {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
};

static void tb_init(struct tar_builder *tb) {
    tb->cap = 64 * 1024;
    tb->buf = calloc(1, tb->cap);
    tb->len = 0;
}

static void tb_free(struct tar_builder *tb) { free(tb->buf); tb->buf = NULL; }

static void octal_field(char *dst, int width, uint64_t value) {
    for (int i = width - 2; i >= 0; i--) {
        dst[i] = (char)('0' + (int)(value & 7u));
        value >>= 3;
    }
    dst[width - 1] = '\0';
}

static void tb_add(struct tar_builder *tb, const char *name,
                   const void *data, size_t size) {
    char *hdr = (char *)(tb->buf + tb->len);
    memset(hdr, 0, TAR_BLOCK);
    strncpy(hdr, name, 99);
    octal_field(hdr + 100, 8, 0644);        /* mode */
    octal_field(hdr + 108, 8, 0);           /* uid  */
    octal_field(hdr + 116, 8, 0);           /* gid  */
    octal_field(hdr + 124, 12, size);       /* size */
    octal_field(hdr + 136, 12, 0);          /* mtime */
    hdr[156] = '0';                         /* typeflag: regular file */
    memcpy(hdr + 257, "ustar", 5);
    tb->len += TAR_BLOCK;

    if (size) {
        memcpy(tb->buf + tb->len, data, size);
        tb->len += ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
    }
}

/* The parser stops at the first zero block; the zero-filled tail serves as
 * the archive terminator without an explicit end marker. */

/* ---- cases ---- */

/* The FIX_R4 gate: a failing allocator makes initrd_init() return an
 * error instead of faulting at the unchecked memset(). */
static int init_reports_failure_when_pool_alloc_fails(void) {
    static const char payload[] = "hello";
    struct tar_builder tb;
    tb_init(&tb);
    tb_add(&tb, "apps/probe", payload, 5);

    kmalloc_may_fail = 1;
    int r = initrd_init((uint64_t)(uintptr_t)tb.buf, tb.len);
    kmalloc_may_fail = 0;
    tb_free(&tb);

    CHECK(r != 0);
    return 1;
}

/* After the failed mount, the ops must stay harmless: the file table was
 * reported back as an empty image, so a lookup cannot reach the NULL pool. */
static int lookup_after_failed_init_is_harmless(void) {
    CHECK(initrd_ops.lookup(NULL, "apps/probe") == NULL);
    CHECK(initrd_ops.lookup(NULL, "anything") == NULL);
    return 1;
}

/* No files in the image means no pool is allocated, so a failing allocator
 * must not fail the mount of an empty tar. */
static int empty_image_needs_no_allocation(void) {
    struct tar_builder tb;
    tb_init(&tb);   /* zero blocks only: the terminator, no entries */

    kmalloc_may_fail = 1;
    int r = initrd_init((uint64_t)(uintptr_t)tb.buf, tb.len);
    kmalloc_may_fail = 0;
    tb_free(&tb);

    CHECK(r == 0);
    return 1;
}

/* The success path is untouched: with a working allocator the image mounts,
 * the file resolves through the ops, and its data reads back. */
static int good_init_mounts_and_reads(void) {
    static const char payload[] = "hello";
    struct tar_builder tb;
    tb_init(&tb);
    tb_add(&tb, "apps/probe", payload, 5);

    int r = initrd_init((uint64_t)(uintptr_t)tb.buf, tb.len);
    CHECK(r == 0);

    struct vnode *vn = initrd_ops.lookup(NULL, "apps/probe");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_FILE);
    CHECK(vn->size == 5);

    char buf[6] = { 0 };
    CHECK(initrd_ops.read(vn, 0, buf, 5) == 5);
    CHECK(strcmp(buf, "hello") == 0);

    tb_free(&tb);
    return 1;
}

int main(void) {
    RUN(init_reports_failure_when_pool_alloc_fails);
    RUN(lookup_after_failed_init_is_harmless);
    RUN(empty_image_needs_no_allocation);
    RUN(good_init_mounts_and_reads);

    printf("test_initrd_allocfail: %d/%d passed\n", passed, tn);
    return failed ? 1 : 0;
}
