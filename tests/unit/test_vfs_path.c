/*
 * test_vfs_path.c — host-side unit tests for the VFS lexical canonicaliser
 * (RESIDUE2 T4, kernel/fs/path.c).
 *
 * The canonicaliser is the floor under both the resolver rework and the
 * installation policy, so every rule it implements gets a named case here —
 * including the two that motivated the phase: "/tmp/../evil" (dot-dot through
 * a mount used to fail incidentally) and the root-absorption of "/..".
 *
 * The shipping source is compiled in directly (the keymap.c pattern): the
 * test cannot drift from the kernel.
 */

#include <stdio.h>
#include <string.h>

#include "kernel/fs/path.h"

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define CHECK(c) do {                                                   \
    if (!(c)) { printf("    L%d: %s\n", __LINE__, #c); return 0; }      \
} while (0)

static int canon_is(const char *in, const char *want) {
    char out[256];
    if (vfs_canonical_path(in, out, sizeof(out)) != 0) {
        printf("    canonicalise('%s') failed, wanted '%s'\n", in, want);
        return 0;
    }
    if (strcmp(out, want) != 0) {
        printf("    canonicalise('%s') = '%s', wanted '%s'\n", in, out, want);
        return 0;
    }
    return 1;
}

static int t_plain(void)           { return canon_is("/tmp/a.txt", "/tmp/a.txt"); }
static int t_root(void)            { return canon_is("/", "/"); }
static int t_repeated_slashes(void){ return canon_is("//tmp////a", "/tmp/a"); }
static int t_trailing_slash(void)  { return canon_is("/tmp/", "/tmp"); }
static int t_trailing_dot(void)    { return canon_is("/tmp/.", "/tmp"); }
static int t_dot_mid(void)         { return canon_is("/tmp/./a", "/tmp/a"); }
static int t_dotdot(void)          { return canon_is("/tmp/sub/../a", "/tmp/a"); }

/* THE box: dot-dot that crosses a mount boundary.  "/tmp/../evil" used to
 * reach tmpfs as the name "../evil" and fail for containing a slash; the
 * canonicaliser must hand the resolver "/evil" — where it then honestly
 * fails on the read-only initrd, the SAME failure a POSIX system gives. */
static int t_dotdot_through_mount(void) { return canon_is("/tmp/../evil", "/evil"); }

static int t_dotdot_at_root(void)  {
    return canon_is("/..", "/") && canon_is("/../..", "/") &&
           canon_is("/tmp/../..", "/");
}
static int t_many_dots(void)       {
    return canon_is("/a/b/c/../../../d", "/d") &&
           canon_is("/a/b/../../../../d", "/d");
}
static int t_dotdir_is_component(void) {
    /* ".hidden" is a NAME, not a "." component. */
    return canon_is("/tmp/.hidden/..", "/tmp") &&
           canon_is("/tmp/..file", "/tmp/..file");
}
static int t_dotdotdot_name(void)  { return canon_is("/tmp/...", "/tmp/..."); }

static int t_rejects_relative(void) {
    char out[256];
    CHECK(vfs_canonical_path("tmp/a", out, sizeof(out)) == -1);
    CHECK(vfs_canonical_path("./a", out, sizeof(out)) == -1);
    CHECK(out[0] == '\0');
    return 1;
}
static int t_rejects_null(void) {
    char out[4];
    CHECK(vfs_canonical_path(NULL, out, sizeof(out)) == -1);
    CHECK(vfs_canonical_path("/a", NULL, 16) == -1);
    CHECK(vfs_canonical_path("/a", out, 0) == -1);
    CHECK(vfs_canonical_path("/a", out, 1) == -1);   /* needs room for "/\0" */
    return 1;
}
static int t_rejects_overflow(void) {
    char big[600], out[16];
    for (size_t i = 0; i < sizeof(big) - 1; i++) big[i] = 'x';
    big[sizeof(big) - 1] = '\0';
    /* 599 chars cannot fit a 16-byte out. */
    CHECK(vfs_canonical_path(big, out, sizeof(out)) == -1);
    /* A path that barely fits a right-sized buffer still canonicalises. */
    char fit[8];
    CHECK(vfs_canonical_path("/abc", fit, sizeof(fit)) == 0);
    CHECK(strcmp(fit, "/abc") == 0);
    return 1;
}

/* Mount-table shape: the canonicaliser must never emit an empty component
 * or a trailing slash, because find_mount() longest-prefix matching and the
 * FS lookups both assume "/mount/rel/name" with no holes. */
static int t_output_shape(void) {
    char out[256];
    const char *cases[] = { "/tmp//", "/tmp/./", "/tmp/xx/..", "//", "/a//b//" };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(vfs_canonical_path(cases[i], out, sizeof(out)) == 0);
        CHECK(out[0] == '/');
        size_t l = strlen(out);
        CHECK(l == 1 || out[l - 1] != '/');            /* no trailing slash */
        for (size_t j = 0; j + 1 < l; j++) {           /* no "//" hole */
            if (out[j] == '/' && out[j + 1] == '/') {
                printf("    '%s' -> '%s' has '//'\n", cases[i], out);
                return 0;
            }
        }
    }
    return 1;
}

int main(void) {
    printf("test_vfs_path: lexical path canonicalisation (T4)\n");

    RUN(t_plain);
    RUN(t_root);
    RUN(t_repeated_slashes);
    RUN(t_trailing_slash);
    RUN(t_trailing_dot);
    RUN(t_dot_mid);
    RUN(t_dotdot);
    RUN(t_dotdot_through_mount);
    RUN(t_dotdot_at_root);
    RUN(t_many_dots);
    RUN(t_dotdir_is_component);
    RUN(t_dotdotdot_name);
    RUN(t_rejects_relative);
    RUN(t_rejects_null);
    RUN(t_rejects_overflow);
    RUN(t_output_shape);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
