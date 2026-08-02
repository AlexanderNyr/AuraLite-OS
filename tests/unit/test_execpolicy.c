/*
 * test_execpolicy.c — host-side unit tests for the executable-installation
 * policy (phase F1 of FSLAYOUT_PLAN.md).
 *
 * WHY THIS TEST EXISTS
 *
 * exec_install_allowed() is a security predicate, and the interesting inputs
 * are precisely the ones a normal run never produces: "/opt/../etc/evil",
 * "/tmpfile", "//opt//x", "/opt" with nothing after it.  An allowlist that is
 * only ever asked about well-formed paths will look correct right up to the
 * moment someone asks it about a malformed one.
 *
 * The predicate is pure, so the shipping source is compiled in directly and
 * the test cannot drift from it.
 */

#include <stdio.h>
#include <string.h>

#include "kernel/fs/execpolicy.h"

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define CHECK(c) do {                                                   \
    if (!(c)) { printf("    L%d: %s\n", __LINE__, #c); return 0; }      \
} while (0)

/* Canonicalise and compare, reporting the actual result on a mismatch —
 * "expected /etc/evil, got /opt/etc/evil" localises a bug immediately,
 * where a bare pass/fail does not. */
static int canon_is(const char *in, const char *want) {
    char out[256];
    if (exec_path_canonical(in, out, sizeof(out)) != 0) {
        printf("    canonicalise('%s') failed, wanted '%s'\n", in, want);
        return 0;
    }
    if (strcmp(out, want) != 0) {
        printf("    canonicalise('%s') = '%s', wanted '%s'\n", in, out, want);
        return 0;
    }
    return 1;
}

/* ---- canonicalisation ------------------------------------------------- */

static int t_canon_plain(void) {
    CHECK(canon_is("/opt/matrix", "/opt/matrix"));
    CHECK(canon_is("/a/b/c", "/a/b/c"));
    return 1;
}

static int t_canon_root(void) {
    CHECK(canon_is("/", "/"));
    return 1;
}

static int t_canon_repeated_slashes(void) {
    CHECK(canon_is("//opt//matrix", "/opt/matrix"));
    CHECK(canon_is("/opt///x", "/opt/x"));
    return 1;
}

static int t_canon_trailing_slash(void) {
    CHECK(canon_is("/opt/matrix/", "/opt/matrix"));
    CHECK(canon_is("/opt/", "/opt"));
    return 1;
}

static int t_canon_dot(void) {
    CHECK(canon_is("/opt/./matrix", "/opt/matrix"));
    CHECK(canon_is("/./opt/matrix", "/opt/matrix"));
    CHECK(canon_is("/opt/matrix/.", "/opt/matrix"));
    return 1;
}

static int t_canon_dotdot(void) {
    CHECK(canon_is("/opt/../etc/evil", "/etc/evil"));
    CHECK(canon_is("/opt/sub/../matrix", "/opt/matrix"));
    CHECK(canon_is("/a/b/c/../../d", "/a/d"));
    return 1;
}

/* ".." at the root is absorbed rather than escaping — the same thing a
 * kernel does with "/..". */
static int t_canon_dotdot_above_root(void) {
    CHECK(canon_is("/..", "/"));
    CHECK(canon_is("/../..", "/"));
    CHECK(canon_is("/../opt/x", "/opt/x"));
    return 1;
}

static int t_canon_rejects_relative(void) {
    char out[256];
    CHECK(exec_path_canonical("opt/matrix", out, sizeof(out)) != 0);
    CHECK(exec_path_canonical("../etc", out, sizeof(out)) != 0);
    return 1;
}

static int t_canon_rejects_null_and_empty(void) {
    char out[256];
    CHECK(exec_path_canonical(NULL, out, sizeof(out)) != 0);
    CHECK(exec_path_canonical("/opt/x", NULL, sizeof(out)) != 0);
    CHECK(exec_path_canonical("", out, sizeof(out)) != 0);
    return 1;
}

/* A result that does not fit must fail, not truncate: a truncated path is a
 * different path, and a different path might be an allowed one. */
static int t_canon_rejects_overflow(void) {
    char small[8];
    CHECK(exec_path_canonical("/opt/matrix", small, sizeof(small)) != 0);
    return 1;
}

/* ---- the policy ------------------------------------------------------- */

static int t_allows_opt(void) {
    CHECK(exec_install_allowed("/opt/matrix"));
    CHECK(exec_install_allowed("/opt/life"));
    return 1;
}

static int t_allows_tmp(void) {
    CHECK(exec_install_allowed("/tmp/build.elf"));
    return 1;
}

static int t_allows_nested(void) {
    CHECK(exec_install_allowed("/opt/pkg/bin/tool"));
    return 1;
}

static int t_refuses_root(void) {
    CHECK(!exec_install_allowed("/evil"));
    return 1;
}

static int t_refuses_other_dirs(void) {
    CHECK(!exec_install_allowed("/etc/evil"));
    CHECK(!exec_install_allowed("/apps/evil"));
    CHECK(!exec_install_allowed("/bin/evil"));
    CHECK(!exec_install_allowed("/disk/evil"));
    CHECK(!exec_install_allowed("/fat/evil"));
    return 1;
}

/* THE case an allowlist gets bypassed on.  The check must run after
 * canonicalisation, or the prefix "/opt" matches and the file lands in /etc. */
static int t_refuses_traversal(void) {
    CHECK(!exec_install_allowed("/opt/../etc/evil"));
    CHECK(!exec_install_allowed("/tmp/../evil"));
    CHECK(!exec_install_allowed("/opt/../../etc/evil"));
    CHECK(!exec_install_allowed("/opt/a/../../etc/evil"));
    return 1;
}

/* Traversal that comes back inside is fine — the predicate judges the
 * destination, not the route. */
static int t_allows_traversal_that_returns(void) {
    CHECK(exec_install_allowed("/opt/sub/../matrix"));
    CHECK(exec_install_allowed("/etc/../opt/matrix"));
    return 1;
}

/* The classic off-by-one: "/tmpfile" starts with "/tmp" but is not in it. */
static int t_refuses_prefix_lookalike(void) {
    CHECK(!exec_install_allowed("/tmpfile"));
    CHECK(!exec_install_allowed("/optional/evil"));
    CHECK(!exec_install_allowed("/opt2/evil"));
    return 1;
}

/* The directory itself is not a file that can be created executable. */
static int t_refuses_the_directory_itself(void) {
    CHECK(!exec_install_allowed("/opt"));
    CHECK(!exec_install_allowed("/tmp"));
    CHECK(!exec_install_allowed("/opt/"));
    return 1;
}

static int t_refuses_relative(void) {
    CHECK(!exec_install_allowed("opt/matrix"));
    CHECK(!exec_install_allowed("matrix"));
    return 1;
}

static int t_refuses_null(void) {
    CHECK(!exec_install_allowed(NULL));
    CHECK(!exec_install_allowed(""));
    return 1;
}

static int t_normalises_before_judging(void) {
    CHECK(exec_install_allowed("//opt//matrix"));
    CHECK(exec_install_allowed("/opt/./matrix"));
    return 1;
}

/* A path too long to canonicalise must be refused, not accepted on the
 * strength of its first few characters. */
static int t_refuses_overlong(void) {
    char huge[1024];
    memset(huge, 'a', sizeof(huge));
    memcpy(huge, "/opt/", 5);
    huge[sizeof(huge) - 1] = '\0';
    CHECK(!exec_install_allowed(huge));
    return 1;
}

/* ---- the allowlist itself --------------------------------------------- */

static int t_allowlist_enumerable(void) {
    CHECK(exec_install_dir(0) != NULL);
    CHECK(exec_install_dir(1) != NULL);
    CHECK(exec_install_dir(-1) == NULL);
    /* Whatever the list is, every entry must permit a file inside it —
     * otherwise the list and the predicate disagree. */
    for (int i = 0; exec_install_dir(i); i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/probe", exec_install_dir(i));
        if (!exec_install_allowed(p)) {
            printf("    allowlist entry '%s' does not permit '%s'\n",
                   exec_install_dir(i), p);
            return 0;
        }
    }
    return 1;
}

/* /opt must be on the list: this is the whole point of the phase, and a
 * refactor that quietly dropped it would otherwise pass every other test. */
static int t_opt_is_allowed_explicitly(void) {
    int found = 0;
    for (int i = 0; exec_install_dir(i); i++) {
        if (strcmp(exec_install_dir(i), "/opt") == 0) found = 1;
    }
    CHECK(found);
    return 1;
}

int main(void) {
    printf("test_execpolicy: executable installation allowlist\n");

    RUN(t_canon_plain);
    RUN(t_canon_root);
    RUN(t_canon_repeated_slashes);
    RUN(t_canon_trailing_slash);
    RUN(t_canon_dot);
    RUN(t_canon_dotdot);
    RUN(t_canon_dotdot_above_root);
    RUN(t_canon_rejects_relative);
    RUN(t_canon_rejects_null_and_empty);
    RUN(t_canon_rejects_overflow);

    RUN(t_allows_opt);
    RUN(t_allows_tmp);
    RUN(t_allows_nested);
    RUN(t_refuses_root);
    RUN(t_refuses_other_dirs);
    RUN(t_refuses_traversal);
    RUN(t_allows_traversal_that_returns);
    RUN(t_refuses_prefix_lookalike);
    RUN(t_refuses_the_directory_itself);
    RUN(t_refuses_relative);
    RUN(t_refuses_null);
    RUN(t_normalises_before_judging);
    RUN(t_refuses_overlong);

    RUN(t_allowlist_enumerable);
    RUN(t_opt_is_allowed_explicitly);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
