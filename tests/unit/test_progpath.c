/*
 * test_progpath.c — host-side unit tests for the program search path
 * (phase F2 of FSLAYOUT_PLAN.md).
 *
 * WHY THIS TEST EXISTS
 *
 * prog_resolve() builds a path string per candidate directory, and the
 * interesting part is the joining: "/" already ends in a separator and every
 * other entry does not, so a naive concatenation produces either "//hello" or
 * "/appshello" depending on which case was written first. Neither is caught
 * by the integration test, because both would simply fail to open and the
 * search would move on to the entry that happens to work.
 *
 * The shipping source is compiled in, with open()/close() replaced by a stub
 * that answers from a list of "files that exist". That makes the search order
 * observable, which the real filesystem does not.
 *
 * progpath.c includes "unistd.h" and "fcntl.h" meaning AURALITE's headers.
 * tests/unit/pathstub/ supplies host stand-ins for them and is placed ahead of
 * the system include path, so this test never sees glibc's declarations. It
 * used to, and got away with it until a CI machine with _FORTIFY_SOURCE
 * enabled turned glibc's open() into an inline definition that collided with
 * the stub below.
 */

#include <stdio.h>
#include <string.h>

/* ---- stub filesystem -------------------------------------------------- */

#define MAX_EXIST 16
static const char *existing[MAX_EXIST];
static int existing_count = 0;

/* Every path prog_resolve() probed, in order. */
#define MAX_PROBES 32
static char probes[MAX_PROBES][128];
static int probe_count = 0;

static void fs_reset(void) {
    existing_count = 0;
    probe_count = 0;
}

static void fs_add(const char *path) {
    if (existing_count < MAX_EXIST) existing[existing_count++] = path;
}

/* The stubs the compiled-in source will call.  They are DEFINED here and
 * DECLARED by tests/unit/pathstub/unistd.h, which the include path puts ahead
 * of glibc's — see that directory's fcntl.h for why.  Declaring them here as
 * well would collide with glibc's fortified inline open() on any machine
 * where _FORTIFY_SOURCE is on by default, which is exactly how this test
 * broke in CI while passing locally. */
int open(const char *path, int flags, ...) {
    (void)flags;
    if (probe_count < MAX_PROBES) {
        snprintf(probes[probe_count], sizeof(probes[0]), "%s", path);
        probe_count++;
    }
    for (int i = 0; i < existing_count; i++) {
        if (strcmp(existing[i], path) == 0) return 3;   /* any valid fd */
    }
    return -1;
}

int close(int fd) { (void)fd; return 0; }

/* The unit under test.  Included rather than linked so the stubs above are
 * the ones it calls. */
#include "lib/libc/src/progpath.c"

/* ---- harness ---------------------------------------------------------- */

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define CHECK(c) do {                                                   \
    if (!(c)) { printf("    L%d: %s\n", __LINE__, #c); return 0; }      \
} while (0)

static int resolves_to(const char *name, const char *want) {
    char out[128];
    if (!prog_resolve(name, out, (int)sizeof(out))) {
        printf("    resolve('%s') found nothing, wanted '%s'\n", name, want);
        return 0;
    }
    if (strcmp(out, want) != 0) {
        printf("    resolve('%s') = '%s', wanted '%s'\n", name, out, want);
        return 0;
    }
    return 1;
}

/* ---- tests ------------------------------------------------------------ */

static int t_finds_in_bin(void) {
    fs_reset();
    fs_add("/bin/hello");
    CHECK(resolves_to("hello", "/bin/hello"));
    return 1;
}

static int t_finds_in_apps(void) {
    fs_reset();
    fs_add("/apps/calc");
    CHECK(resolves_to("calc", "/apps/calc"));
    return 1;
}

/* The layout that exists today: everything flat in the root. */
static int t_finds_in_root(void) {
    fs_reset();
    fs_add("/calc");
    CHECK(resolves_to("calc", "/calc"));
    return 1;
}

/* THE joining bug: "/" already ends in a separator, so the root candidate
 * must be "/calc" and not "//calc". */
static int t_root_join_has_one_slash(void) {
    fs_reset();
    fs_add("/calc");
    CHECK(resolves_to("calc", "/calc"));
    for (int i = 0; i < probe_count; i++) {
        if (strncmp(probes[i], "//", 2) == 0) {
            printf("    probed '%s' — doubled separator\n", probes[i]);
            return 0;
        }
    }
    return 1;
}

/* The mirror-image bug: a non-root entry must gain a separator. */
static int t_dir_join_has_a_slash(void) {
    fs_reset();
    /* Nothing exists, so every candidate is probed and can be inspected. */
    char out[128];
    prog_resolve("calc", out, (int)sizeof(out));
    CHECK(probe_count > 0);
    for (int i = 0; i < probe_count; i++) {
        if (strcmp(probes[i], "/bincalc") == 0 ||
            strcmp(probes[i], "/appscalc") == 0) {
            printf("    probed '%s' — missing separator\n", probes[i]);
            return 0;
        }
    }
    CHECK(strcmp(probes[0], "/bin/calc") == 0);
    return 1;
}

/* Order matters: a program in its proper directory must win over a
 * compatibility alias at the root, which is the situation F3 creates. */
static int t_proper_dir_beats_root_alias(void) {
    fs_reset();
    fs_add("/apps/calc");
    fs_add("/calc");
    CHECK(resolves_to("calc", "/apps/calc"));
    return 1;
}

static int t_search_order_is_as_documented(void) {
    fs_reset();
    char out[128];
    prog_resolve("x", out, (int)sizeof(out));
    CHECK(probe_count == prog_path_count());
    CHECK(strcmp(probes[0], "/bin/x") == 0);
    CHECK(strcmp(probes[1], "/apps/x") == 0);
    CHECK(strcmp(probes[2], "/demos/x") == 0);
    CHECK(strcmp(probes[3], "/tests/x") == 0);
    CHECK(strcmp(probes[4], "/opt/x") == 0);
    CHECK(strcmp(probes[5], "/x") == 0);
    return 1;
}

/* An explicit path bypasses the search entirely — including the probing. */
static int t_absolute_path_bypasses_search(void) {
    fs_reset();
    CHECK(resolves_to("/some/where/prog", "/some/where/prog"));
    CHECK(probe_count == 0);
    return 1;
}

/* ...and so does a relative one, because it also contains a slash. */
static int t_relative_path_bypasses_search(void) {
    fs_reset();
    CHECK(resolves_to("./prog", "./prog"));
    CHECK(probe_count == 0);
    return 1;
}

/* A path that does not exist still resolves: spawn() reports the failure, and
 * "not found on the search path" would be a misleading thing to say about a
 * file the caller named specifically. */
static int t_explicit_path_resolves_even_if_absent(void) {
    fs_reset();
    CHECK(resolves_to("/nope", "/nope"));
    return 1;
}

static int t_missing_name_fails(void) {
    fs_reset();
    char out[128];
    CHECK(prog_resolve("nothing_here", out, (int)sizeof(out)) == 0);
    CHECK(probe_count == prog_path_count());   /* it did look everywhere */
    return 1;
}

static int t_rejects_bad_arguments(void) {
    char out[128];
    CHECK(prog_resolve(NULL, out, (int)sizeof(out)) == 0);
    CHECK(prog_resolve("", out, (int)sizeof(out)) == 0);
    CHECK(prog_resolve("x", NULL, 128) == 0);
    CHECK(prog_resolve("x", out, 1) == 0);
    return 1;
}

/* A buffer too small must produce a terminated string, not a smear. */
static int t_truncation_is_terminated(void) {
    fs_reset();
    char out[6];
    prog_resolve("verylongprogramname", out, (int)sizeof(out));
    CHECK(out[sizeof(out) - 1] == '\0');
    return 1;
}

static int t_path_list_is_enumerable(void) {
    CHECK(prog_path_count() == 6);
    CHECK(prog_path_entry(-1) == NULL);
    CHECK(prog_path_entry(prog_path_count()) == NULL);
    for (int i = 0; i < prog_path_count(); i++) {
        CHECK(prog_path_entry(i) != NULL);
        CHECK(prog_path_entry(i)[0] == '/');
    }
    return 1;
}

/* "/" must be last, or a root alias would shadow the real location. */
static int t_root_is_searched_last(void) {
    CHECK(strcmp(prog_path_entry(prog_path_count() - 1), "/") == 0);
    return 1;
}

int main(void) {
    printf("test_progpath: program search path\n");

    RUN(t_finds_in_bin);
    RUN(t_finds_in_apps);
    RUN(t_finds_in_root);
    RUN(t_root_join_has_one_slash);
    RUN(t_dir_join_has_a_slash);
    RUN(t_proper_dir_beats_root_alias);
    RUN(t_search_order_is_as_documented);
    RUN(t_absolute_path_bypasses_search);
    RUN(t_relative_path_bypasses_search);
    RUN(t_explicit_path_resolves_even_if_absent);
    RUN(t_missing_name_fails);
    RUN(t_rejects_bad_arguments);
    RUN(t_truncation_is_terminated);
    RUN(t_path_list_is_enumerable);
    RUN(t_root_is_searched_last);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}
