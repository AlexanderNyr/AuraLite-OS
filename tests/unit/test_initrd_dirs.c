/*
 * test_initrd_dirs.c — host-side unit tests for the USTAR initrd parser and
 * its derived directory view (phase F0 of FSLAYOUT_PLAN.md).
 *
 * WHY THIS TEST EXISTS
 *
 * The initrd file table is flat.  Directories are not stored; they are
 * *derived* from the path prefixes of the files, and readdir() reconstructs
 * a hierarchy from that derivation.  Derived state is exactly the kind of
 * thing that looks right in one case and is wrong in the next — a file at the
 * root and a file two levels down exercise completely different branches of
 * child_of().
 *
 * The real kernel source is compiled in (not copied), so this cannot drift
 * from the shipping parser.  Only kmalloc/kfree/kprintf are stubbed; the tar
 * images are built here in memory, byte for byte in USTAR layout, so the test
 * covers the header walk as well as the directory logic.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/fs/vfs.h"
#include "kernel/fs/initrd.h"

/* ---- stubs the parser needs ---- */

void *kmalloc(uint64_t size) { return malloc((size_t)size); }
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

/* ---- USTAR image construction ---------------------------------------- */

#define TAR_BLOCK 512

struct tar_builder {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
};

static void tb_init(struct tar_builder *tb) {
    tb->cap = 256 * 1024;
    tb->buf = calloc(1, tb->cap);
    tb->len = 0;
}

static void tb_free(struct tar_builder *tb) { free(tb->buf); tb->buf = NULL; }

/* Write an octal field exactly as tar does: right-aligned, zero-padded,
 * NUL-terminated within `width`. */
static void octal_field(char *dst, int width, uint64_t value) {
    for (int i = width - 2; i >= 0; i--) {
        dst[i] = (char)('0' + (int)(value & 7u));
        value >>= 3;
    }
    dst[width - 1] = '\0';
}

/* Append one regular-file entry.  `name` is used verbatim, so the test can
 * feed in the "./" prefix the real tar emits. */
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

/* Append a hard-link entry: type '1', no data, linkname naming an earlier
 * entry.  This is what tar emits for `ln target alias`, and it is how F3
 * ships compatibility aliases without doubling the image. */
static void tb_add_link(struct tar_builder *tb, const char *name,
                        const char *target) {
    char *hdr = (char *)(tb->buf + tb->len);
    memset(hdr, 0, TAR_BLOCK);
    strncpy(hdr, name, 99);
    octal_field(hdr + 100, 8, 0644);
    octal_field(hdr + 108, 8, 0);
    octal_field(hdr + 116, 8, 0);
    octal_field(hdr + 124, 12, 0);          /* a link carries no data */
    octal_field(hdr + 136, 12, 0);
    hdr[156] = '1';                         /* typeflag: hard link */
    strncpy(hdr + 157, target, 99);
    memcpy(hdr + 257, "ustar", 5);
    tb->len += TAR_BLOCK;
}

/* Append an explicit directory entry: type '5', trailing slash, no data. */
static void tb_add_dir(struct tar_builder *tb, const char *name) {
    char *hdr = (char *)(tb->buf + tb->len);
    memset(hdr, 0, TAR_BLOCK);
    strncpy(hdr, name, 99);
    octal_field(hdr + 100, 8, 0755);
    octal_field(hdr + 108, 8, 0);
    octal_field(hdr + 116, 8, 0);
    octal_field(hdr + 124, 12, 0);
    octal_field(hdr + 136, 12, 0);
    hdr[156] = '5';                         /* typeflag: directory */
    memcpy(hdr + 257, "ustar", 5);
    tb->len += TAR_BLOCK;
}

/* Two zero blocks terminate the archive. */
static void tb_finish(struct tar_builder *tb) {
    memset(tb->buf + tb->len, 0, 2 * TAR_BLOCK);
    tb->len += 2 * TAR_BLOCK;
}

/* ---- readdir helpers -------------------------------------------------- */

static struct vfs_dirent ents[64];

static int list(const char *path, int *out_n) {
    struct vnode *vn = initrd_ops.lookup(NULL, path);
    if (!vn) return 0;
    if (vn->type != VFS_TYPE_DIR) return 0;
    *out_n = initrd_ops.readdir(vn, ents, 64);
    return 1;
}

static const struct vfs_dirent *find(int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(ents[i].name, name) == 0) return &ents[i];
    }
    return NULL;
}

/* ---- fixtures --------------------------------------------------------- */

static struct tar_builder g_tb;

/* The layout the plan moves towards, plus a root-level file for the
 * compatibility case and a two-level path to catch grandchild leakage. */
static void build_layout(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./init",            "INIT",     4);
    tb_add(&g_tb, "./apps/calc",       "CALC",     4);
    tb_add(&g_tb, "./apps/editor",     "EDITOR",   6);
    tb_add(&g_tb, "./demos/glcube",    "CUBE",     4);
    tb_add(&g_tb, "./tests/gl/gltest", "GLTEST",   6);
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);
}

static void build_flat(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./hello", "HI", 2);
    tb_add(&g_tb, "./calc",  "CA", 2);
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);
}

/* ---- tests: parsing --------------------------------------------------- */

static int t_flat_files_parse(void) {
    build_flat();
    struct vnode *vn = initrd_ops.lookup(NULL, "hello");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_FILE);
    CHECK(vn->size == 2);
    tb_free(&g_tb);
    return 1;
}

static int t_flat_root_lists_both(void) {
    build_flat();
    int n = 0;
    CHECK(list("", &n));
    CHECK(n == 2);
    CHECK(find(n, "hello") != NULL);
    CHECK(find(n, "calc") != NULL);
    tb_free(&g_tb);
    return 1;
}

/* An image with no subdirectories must still have a usable root: the
 * regression this guards is the root vanishing once it became dirs[0]. */
static int t_root_exists_in_flat_image(void) {
    build_flat();
    struct vnode *vn = initrd_ops.lookup(NULL, "");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_DIR);
    tb_free(&g_tb);
    return 1;
}

static int t_file_in_subdir_resolves(void) {
    build_layout();
    struct vnode *vn = initrd_ops.lookup(NULL, "apps/calc");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_FILE);
    CHECK(vn->size == 4);
    tb_free(&g_tb);
    return 1;
}

static int t_two_level_file_resolves(void) {
    build_layout();
    struct vnode *vn = initrd_ops.lookup(NULL, "tests/gl/gltest");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_FILE);
    CHECK(vn->size == 6);
    tb_free(&g_tb);
    return 1;
}

static int t_file_read_returns_content(void) {
    build_layout();
    struct vnode *vn = initrd_ops.lookup(NULL, "apps/editor");
    CHECK(vn != NULL);
    char buf[16] = {0};
    int64_t r = initrd_ops.read(vn, 0, buf, sizeof(buf));
    CHECK(r == 6);
    CHECK(memcmp(buf, "EDITOR", 6) == 0);
    tb_free(&g_tb);
    return 1;
}

/* ---- tests: the directory view ---------------------------------------- */

static int t_dir_vnode_for_subdir(void) {
    build_layout();
    struct vnode *vn = initrd_ops.lookup(NULL, "apps");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_DIR);
    tb_free(&g_tb);
    return 1;
}

static int t_intermediate_dir_exists(void) {
    build_layout();
    /* "tests/gl" is implied by the file path; nothing declares it. */
    struct vnode *vn = initrd_ops.lookup(NULL, "tests/gl");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_DIR);
    tb_free(&g_tb);
    return 1;
}

/* The whole point of F0: "ls /" must not show "apps/calc". */
static int t_root_shows_dirs_not_paths(void) {
    build_layout();
    int n = 0;
    CHECK(list("", &n));
    CHECK(find(n, "apps/calc") == NULL);
    const struct vfs_dirent *d = find(n, "apps");
    CHECK(d != NULL);
    CHECK(d->type == VFS_TYPE_DIR);
    const struct vfs_dirent *f = find(n, "init");
    CHECK(f != NULL);
    CHECK(f->type == VFS_TYPE_FILE);
    CHECK(f->size == 4);
    /* init, apps, demos, tests — and nothing else. */
    CHECK(n == 4);
    tb_free(&g_tb);
    return 1;
}

static int t_subdir_lists_its_files(void) {
    build_layout();
    int n = 0;
    CHECK(list("apps", &n));
    CHECK(n == 2);
    CHECK(find(n, "calc") != NULL);
    CHECK(find(n, "editor") != NULL);
    CHECK(find(n, "apps/calc") == NULL);
    tb_free(&g_tb);
    return 1;
}

/* A directory whose only child is another directory must not leak the
 * grandchild file into its own listing. */
static int t_dir_with_only_subdir(void) {
    build_layout();
    int n = 0;
    CHECK(list("tests", &n));
    CHECK(n == 1);
    const struct vfs_dirent *d = find(n, "gl");
    CHECK(d != NULL);
    CHECK(d->type == VFS_TYPE_DIR);
    CHECK(find(n, "gltest") == NULL);
    tb_free(&g_tb);
    return 1;
}

static int t_deep_dir_lists_file(void) {
    build_layout();
    int n = 0;
    CHECK(list("tests/gl", &n));
    CHECK(n == 1);
    CHECK(find(n, "gltest") != NULL);
    tb_free(&g_tb);
    return 1;
}

static int t_trailing_slash_resolves_dir(void) {
    build_layout();
    struct vnode *vn = initrd_ops.lookup(NULL, "apps/");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_DIR);
    tb_free(&g_tb);
    return 1;
}

/* "apps/calc/" asserts a directory; it must not resolve to the file. */
static int t_trailing_slash_rejects_file(void) {
    build_layout();
    CHECK(initrd_ops.lookup(NULL, "apps/calc/") == NULL);
    tb_free(&g_tb);
    return 1;
}

static int t_missing_paths_are_null(void) {
    build_layout();
    CHECK(initrd_ops.lookup(NULL, "nope") == NULL);
    CHECK(initrd_ops.lookup(NULL, "apps/nope") == NULL);
    CHECK(initrd_ops.lookup(NULL, "app") == NULL);      /* prefix, not a dir */
    CHECK(initrd_ops.lookup(NULL, "appsx/calc") == NULL);
    tb_free(&g_tb);
    return 1;
}

/* A name that is a prefix of a directory must not be mistaken for it — the
 * classic off-by-one in prefix matching. */
static int t_prefix_is_not_a_match(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./apps/calc", "C", 1);
    tb_add(&g_tb, "./appsuite",  "S", 1);
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    int n = 0;
    CHECK(list("apps", &n));
    CHECK(n == 1);
    CHECK(find(n, "calc") != NULL);
    CHECK(find(n, "uite") == NULL);
    tb_free(&g_tb);
    return 1;
}

/* readdir must honour the caller's array bound. */
static int t_readdir_respects_max(void) {
    build_layout();
    struct vnode *vn = initrd_ops.lookup(NULL, "apps");
    CHECK(vn != NULL);
    struct vfs_dirent small[1];
    int n = initrd_ops.readdir(vn, small, 1);
    CHECK(n == 1);
    tb_free(&g_tb);
    return 1;
}

static int t_writes_still_refused(void) {
    build_layout();
    struct vnode *vn = initrd_ops.lookup(NULL, "apps/calc");
    CHECK(vn != NULL);
    CHECK(initrd_ops.write(vn, 0, "X", 1) < 0);
    tb_free(&g_tb);
    return 1;
}

/* Every file must remain reachable when the image mixes root-level and
 * nested entries — the compatibility-alias shape phase F3 will produce. */
static int t_mixed_image_all_reachable(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./calc",      "A", 1);
    tb_add(&g_tb, "./apps/calc", "B", 1);
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    struct vnode *root_file = initrd_ops.lookup(NULL, "calc");
    CHECK(root_file != NULL);
    CHECK(root_file->type == VFS_TYPE_FILE);
    char a = 0, b = 0;
    CHECK(initrd_ops.read(root_file, 0, &a, 1) == 1);

    struct vnode *nested = initrd_ops.lookup(NULL, "apps/calc");
    CHECK(nested != NULL);
    CHECK(initrd_ops.read(nested, 0, &b, 1) == 1);
    CHECK(a == 'A');
    CHECK(b == 'B');
    tb_free(&g_tb);
    return 1;
}

/* ---- tests: hard links and explicit directories (phase F3) ------------ */

/* The shape F3 produces: the real program in a subdirectory, a root-level
 * alias linked to it. */
static int t_hard_link_alias_resolves(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./apps/calc", "CALCDATA", 8);
    tb_add_link(&g_tb, "./calc", "./apps/calc");
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    struct vnode *real = initrd_ops.lookup(NULL, "apps/calc");
    struct vnode *alias = initrd_ops.lookup(NULL, "calc");
    CHECK(real != NULL);
    CHECK(alias != NULL);
    CHECK(alias->type == VFS_TYPE_FILE);
    CHECK(alias->size == 8);

    char buf[16] = {0};
    CHECK(initrd_ops.read(alias, 0, buf, sizeof(buf)) == 8);
    CHECK(memcmp(buf, "CALCDATA", 8) == 0);
    tb_free(&g_tb);
    return 1;
}

/* An alias must be readable at an offset too — a link that shares the
 * target's data offset but not its size would read past the end. */
static int t_hard_link_partial_read(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./apps/calc", "ABCDEFGH", 8);
    tb_add_link(&g_tb, "./calc", "./apps/calc");
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    struct vnode *alias = initrd_ops.lookup(NULL, "calc");
    CHECK(alias != NULL);
    char buf[16] = {0};
    CHECK(initrd_ops.read(alias, 4, buf, sizeof(buf)) == 4);
    CHECK(memcmp(buf, "EFGH", 4) == 0);
    /* Reading past the end returns nothing, not the next file's bytes. */
    CHECK(initrd_ops.read(alias, 8, buf, sizeof(buf)) == 0);
    tb_free(&g_tb);
    return 1;
}

/* A link with no resolvable target must be skipped, not left pointing at
 * whatever offset happened to be in the struct. */
static int t_dangling_link_is_skipped(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./apps/calc", "X", 1);
    tb_add_link(&g_tb, "./ghost", "./nowhere");
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    CHECK(initrd_ops.lookup(NULL, "ghost") == NULL);
    CHECK(initrd_ops.lookup(NULL, "apps/calc") != NULL);
    tb_free(&g_tb);
    return 1;
}

/* Both names appear in their respective listings. */
static int t_alias_and_target_both_listed(void) {
    tb_init(&g_tb);
    tb_add(&g_tb, "./apps/calc", "X", 1);
    tb_add_link(&g_tb, "./calc", "./apps/calc");
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    int n = 0;
    CHECK(list("", &n));
    CHECK(find(n, "calc") != NULL);
    CHECK(find(n, "apps") != NULL);
    CHECK(list("apps", &n));
    CHECK(n == 1);
    CHECK(find(n, "calc") != NULL);
    tb_free(&g_tb);
    return 1;
}

/* An explicit type-'5' entry registers a directory even with nothing in it —
 * no file path implies an empty directory. */
static int t_explicit_empty_directory(void) {
    tb_init(&g_tb);
    tb_add_dir(&g_tb, "./empty/");
    tb_add(&g_tb, "./hello", "H", 1);
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    struct vnode *vn = initrd_ops.lookup(NULL, "empty");
    CHECK(vn != NULL);
    CHECK(vn->type == VFS_TYPE_DIR);
    int n = 0;
    CHECK(list("empty", &n));
    CHECK(n == 0);
    tb_free(&g_tb);
    return 1;
}

/* A directory named both explicitly and implicitly must be registered once. */
static int t_explicit_and_implied_directory_not_duplicated(void) {
    tb_init(&g_tb);
    tb_add_dir(&g_tb, "./apps/");
    tb_add(&g_tb, "./apps/calc", "X", 1);
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    int n = 0;
    CHECK(list("", &n));
    CHECK(n == 1);                       /* just "apps", not twice */
    CHECK(find(n, "apps") != NULL);
    tb_free(&g_tb);
    return 1;
}

/* The type-'5' entry must not become a FILE called "apps". */
static int t_directory_entry_is_not_a_file(void) {
    tb_init(&g_tb);
    tb_add_dir(&g_tb, "./apps/");
    tb_add(&g_tb, "./apps/calc", "X", 1);
    tb_finish(&g_tb);
    initrd_init((uint64_t)(uintptr_t)g_tb.buf, g_tb.len);

    int n = 0;
    CHECK(list("", &n));
    const struct vfs_dirent *d = find(n, "apps");
    CHECK(d != NULL);
    CHECK(d->type == VFS_TYPE_DIR);
    tb_free(&g_tb);
    return 1;
}

int main(void) {
    printf("test_initrd_dirs: USTAR parser + derived directory view\n");

    RUN(t_flat_files_parse);
    RUN(t_flat_root_lists_both);
    RUN(t_root_exists_in_flat_image);
    RUN(t_file_in_subdir_resolves);
    RUN(t_two_level_file_resolves);
    RUN(t_file_read_returns_content);
    RUN(t_dir_vnode_for_subdir);
    RUN(t_intermediate_dir_exists);
    RUN(t_root_shows_dirs_not_paths);
    RUN(t_subdir_lists_its_files);
    RUN(t_dir_with_only_subdir);
    RUN(t_deep_dir_lists_file);
    RUN(t_trailing_slash_resolves_dir);
    RUN(t_trailing_slash_rejects_file);
    RUN(t_missing_paths_are_null);
    RUN(t_prefix_is_not_a_match);
    RUN(t_readdir_respects_max);
    RUN(t_writes_still_refused);
    RUN(t_mixed_image_all_reachable);

    RUN(t_hard_link_alias_resolves);
    RUN(t_hard_link_partial_read);
    RUN(t_dangling_link_is_skipped);
    RUN(t_alias_and_target_both_listed);
    RUN(t_explicit_empty_directory);
    RUN(t_explicit_and_implied_directory_not_duplicated);
    RUN(t_directory_entry_is_not_a_file);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
