/* tests/unit/test_home_passwd.c — HOME_PLAN H0 gate, host-side.
 *
 * The /etc/passwd parser decides whose home a uid gets, and that decision is
 * compiled into the kernel (kernel/fs/home.c).  Testing it only through a
 * QEMU boot would mean every grammar case costs a boot, so this test does
 * what tests/unit/test_sh_expand.c does for the shell expander: it compiles
 * the SHIPPED parser body and drives it directly.
 *
 * Compiled twice by the Makefile:
 *   - without a macro: the parser as the kernel sees it (positive cases);
 *   - with -DTEST_MUTATE_PARSE=1: a doctored build where the 6-field
 *     default-shell rule is broken, to prove the gates can fail.
 *
 * Exit status: 0 all pass; 1 a check failed.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The parser and its record type live in the kernel tree and pull in the
 * kernel's kprintf/string headers only through home.h's dependencies.  The
 * test defines the small surface parsing needs so the body compiles on the
 * host unchanged; nothing here re-implements parsing. */
#define AURALITE_TEST_HOME 1

#include "../../kernel/fs/home.h"

/* The kernel VFS surface home.c references but this test never calls: the
 * parser is the subject, home_init() is not.  The real headers are included
 * FIRST so the stub definitions below use their exact declared prototypes --
 * a stub whose type disagrees is a second, drifting declaration, which is
 * the class of mistake this test is supposed to catch elsewhere. */
#include "../../kernel/fs/vfs.h"
#include "../../kernel/fs/tmpfs.h"

/* The parser only needs memcpy/memset/strlen/strcmp from the kernel string
 * header, and those are the host's libc here.  home.c includes
 * "kernel/lib/kprintf.h" and "kernel/mm/kheap.h" for home_init(); those
 * compile fine on the host (they are header-only declarations), so the
 * shims below are only for the link-time symbols this binary never calls.
 *
 * parse_passwd_line() is static in home.c.  Including the .c directly is
 * deliberate: it is the same body the kernel links, compiled a second time,
 * so the test cannot drift from the shipped parser (the SH6a precedent).
 *
 * Link with -Wl,--unresolved-symbols=ignore-all is NOT used: stubs for the
 * few external names keep the link honest. */
void *tmpfs_volume_home(void) { return NULL; }
void *kmalloc(uint64_t n) { (void)n; return NULL; }
void  kfree(void *p) { (void)p; }
void  kprintf(const char *fmt, ...) { (void)fmt; }
int   vfs_mount(const char *p, const struct vfs_ops *ops, void *d) {
    (void)p; (void)ops; (void)d; return -1;
}
int   vfs_open(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
int64_t vfs_read(int fd, void *b, uint64_t n) {
    (void)fd; (void)b; (void)n; return -1;
}
int64_t vfs_write(int fd, const void *b, uint64_t n) {
    (void)fd; (void)b; (void)n; return -1;
}
int   vfs_close(int fd) { (void)fd; return -1; }
int   vfs_mkdir(const char *p, uint32_t mode) { (void)p; (void)mode; return -1; }
int   vfs_stat(const char *p, struct vfs_stat *st) { (void)p; (void)st; return -1; }
int   vfs_unlink(const char *p) { (void)p; return -1; }

/* The mutation build removes ONE shipped rule -- the 6-field default shell
 * -- by pre-empting its definition.  Editing a #define here would prove
 * nothing about the parser; this removes the parser's own behaviour, and
 * the "6-field default shell" check above must then go red.  The gate is
 * the Makefile rule that runs this build and REQUIRES exit 1. */
#ifdef TEST_MUTATE_PARSE
#define parse_passwd_line parse_passwd_line_shipped
#include "../../kernel/fs/home.c"
#undef parse_passwd_line

/* A redefinition that refuses the 6-field form: the planted defect. */
static int parse_passwd_line(const char *line, size_t len,
                             struct home_user *u) {
    const char *save = line;
    /* Count the colons; 6 fields means 5 separators. */
    int colons = 0;
    for (size_t i = 0; i < len; i++) if (line[i] == ':') colons++;
    (void)save;
    if (colons == 5) return -1;   /* planted: the form the parser defaults */
    return parse_passwd_line_shipped(line, len, u);
}
#else
#include "../../kernel/fs/home.c"
#endif

static int failures;
static int checks;

static void check(const char *what, int got, int want) {
    checks++;
    if (got == want) {
        return;
    }
    printf("FAIL %s (got %d want %d)\n", what, got, want);
    failures++;
}

static int parse(const char *line, struct home_user *u) {
    return parse_passwd_line(line, strlen(line), u);
}

int main(void) {
    struct home_user u;

    /* --- canonical records ------------------------------------------- */
    check("7-field record",
          parse("alice:x:1000:1000:Alice:/home/alice:/bin/sh", &u), 1);
    check("7-field uid", (int)u.uid, 1000);
    check("7-field gid", (int)u.gid, 1000);
    check("7-field dir", strcmp(u.dir, "/home/alice") == 0, 1);
    check("7-field shell", strcmp(u.shell, "/bin/sh") == 0, 1);
    check("7-field gecos", strcmp(u.gecos, "Alice") == 0, 1);

    check("6-field record (shell defaulted)",
          parse("bob:x:1001:1001:Bob:/home/bob", &u), 1);
    /* The default-shell rule is the one the shipped parser provides and the
     * mutation below removes, so this is the check that must go red there. */
#ifndef TEST_MUTATE_PARSE
    check("6-field default shell", strcmp(u.shell, "/bin/sh") == 0, 1);
#else
    check("(mutation: the default-shell rule is gone)",
          strcmp(u.shell, "/bin/sh") == 0, 0);
#endif

    /* --- refusals ---------------------------------------------------- */
    check("5-field record refused",
          parse("carol:x:1002:1002:Carol", &u), -1);
    check("non-numeric uid refused",
          parse("eve:x:uid:1000:Eve:/home/eve:/bin/sh", &u), -1);
    check("empty uid refused",
          parse("eve:x::1000:Eve:/home/eve:/bin/sh", &u), -1);
    check("uid > 2^32-1 refused",
          parse("eve:x:4294967296:1:Eve:/home/eve:/bin/sh", &u), -1);
    check("empty name refused",
          parse(":x:1003:1003:nameless:/home/x:/bin/sh", &u), -1);
    check("8-field record refused",
          parse("x:x:1:1:x:/home/x:/bin/sh:extra", &u), -1);

    /* --- skips -------------------------------------------------------- */
    check("blank line skipped", parse("\n", &u), 0);
    check("comment skipped",
          parse("# root:x:0:0:root:/root:/bin/sh", &u), 0);

    /* --- host-edited file artefacts ----------------------------------- */
    check("last line without newline",
          parse("dave:x:1003:1003:Dave:/home/dave:/bin/bash", &u), 1);
    check("CRLF line end",
          parse("frank:x:1004:1004:Frank:/home/frank:/bin/sh\r\n", &u), 1);
    check("CRLF shell has no CR", strchr(u.shell, '\r') == NULL, 1);

    /* --- the whole-buffer path ---------------------------------------- */
    {
        static const char file[] =
            "# comment\n"
            "\n"
            "root:x:0:0:root:/root:/bin/sh\n"
            "user:x:1000:1000:AuraLite user:/home/user:/bin/sh\n"
            "broken\n";
        int bad = 0;
        int kept = parse_passwd(file, sizeof(file) - 1, &bad);
        check("file: two records", kept, 2);
        check("file: one malformed", bad, 1);
        check("file: record 0 is root", strcmp(users[0].name, "root") == 0, 1);
        check("file: record 1 is user", strcmp(users[1].name, "user") == 0, 1);
        check("file: root home", strcmp(users[0].dir, "/root") == 0, 1);
    }

    /* --- boundary: a uid exactly at 2^32-1 is legal -------------------- */
    check("uid 4294967295 accepted",
          parse("max:x:4294967295:4294967295:M:/home/max:/bin/sh", &u), 1);
    check("uid 4294967295 value", u.uid == 0xFFFFFFFFu, 1);

    printf("test_home_passwd: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
