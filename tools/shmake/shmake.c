/* tools/shmake/shmake.c -- AuraLite's POSIX-subset make (SELFHOST_PLAN.md SH6e).
 *
 * The host Makefile is GNU-make syntax with generated-file gymnastics; full
 * GNU make compatibility is explicitly not this plan's job (D5).  shmake is
 * the subset build.sh needs:
 *
 *   - explicit rules `target: prereq...` with tab-indented recipes
 *   - variables `CC = tcc`, `$(CC)`, `$@` `$<` `$^`, command-line NAME=val
 *   - `.PHONY`
 *   - timestamp comparison: a target is rebuilt if it is missing, phony,
 *     or older than a prerequisite; otherwise it is skipped
 *
 * Out of scope (GNU make, and SH6f/SH8): pattern rules, `include`, `ifeq`,
 * `$(wildcard)`/`$(shell)`, VPATH, order-only `|`, `-j`, `sh -c`.
 *
 * Recipes do not go through system()/sh -c: AuraLite has no /bin/sh (D10:
 * the shell is a builtin of init, and system() returns ENOSYS).  The
 * expanded recipe is split on whitespace and exec'd -- spawnv in-guest,
 * fork+execvp on the host.  Redirects in recipes are out of scope; the
 * shell already has them.
 *
 * Host unit test: tests/unit/test_shmake.sh.
 * In-guest gate:  tests/integration/cases/test_selfhost_shmake.sh.
 *
 * Portability: plain C99, no GNU extensions, no VLAs, tcc-compatible.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __AURALITE__
/* AuraLite's struct stat lives in <unistd.h>; spawnv is the working
 * "run a program in a new address space" path.  fork()+exec was measured
 * to #PF for pipeline children (ledger SH-40) and is not used here. */
#endif

#define MAX_RULES    128
#define MAX_PREREQS   32
#define MAX_RECIPES   16
#define MAX_VARS     128
#define MAX_LINE     512
#define MAX_NAME     128
#define MAX_ARGV      32
#define MAX_EXPAND  2048
#define ARENA_SIZE (96 * 1024)
#define EXPAND_DEPTH  16

static char  arena[ARENA_SIZE];
static size_t arena_used;

static int dry_run;
static int n_rebuilt;
static int n_uptodate;

static void die(const char *msg) {
    fprintf(stderr, "shmake: %s\n", msg);
    exit(2);
}

static void dief(const char *a, const char *b) {
    fprintf(stderr, "shmake: %s%s\n", a, b);
    exit(2);
}

static void *areq(size_t n) {
    n = (n + 7u) & ~7u;
    if (arena_used + n > sizeof arena) die("out of memory");
    void *p = arena + arena_used;
    arena_used += n;
    return p;
}

static char *astrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)areq(n);
    memcpy(p, s, n);
    return p;
}

/* ---- variables ------------------------------------------------------- */

enum { VAR_FILE = 0, VAR_CMDLINE = 1 };

struct var {
    const char *name;
    const char *value;
    int origin;                 /* VAR_FILE or VAR_CMDLINE */
};

static struct var vars[MAX_VARS];
static int nvars;

static struct var *var_find(const char *name) {
    int i;
    for (i = 0; i < nvars; i++) {
        if (strcmp(vars[i].name, name) == 0) return &vars[i];
    }
    return 0;
}

static void var_set(const char *name, const char *value, int origin) {
    struct var *v = var_find(name);
    if (v) {
        if (v->origin == VAR_CMDLINE && origin == VAR_FILE) return;
        v->value = astrdup(value);
        v->origin = origin;
        return;
    }
    if (nvars >= MAX_VARS) die("too many variables");
    vars[nvars].name = astrdup(name);
    vars[nvars].value = astrdup(value);
    vars[nvars].origin = origin;
    nvars++;
}

/* ---- rules ----------------------------------------------------------- */

struct rule {
    const char *target;
    const char *prereqs[MAX_PREREQS];
    int nprereq;
    const char *recipes[MAX_RECIPES];
    int nrecipe;
    int phony;
    int visiting;               /* 1 while building, for cycle detect */
    int built;                  /* already considered this run */
};

static struct rule rules[MAX_RULES];
static int nrules;
static struct rule *cur_rule;
static const char *default_target;

static struct rule *rule_find(const char *target) {
    int i;
    for (i = 0; i < nrules; i++) {
        if (strcmp(rules[i].target, target) == 0) return &rules[i];
    }
    return 0;
}

static struct rule *rule_add(const char *target) {
    struct rule *r = rule_find(target);
    if (r) return r;
    if (nrules >= MAX_RULES) die("too many rules");
    r = &rules[nrules++];
    memset(r, 0, sizeof *r);
    r->target = astrdup(target);
    if (!default_target && target[0] != '.') default_target = r->target;
    return r;
}

static void mark_phony(const char *name) {
    struct rule *r = rule_add(name);
    r->phony = 1;
}

/* ---- expand ---------------------------------------------------------- */

static const char *var_lookup(const char *name,
                              const char *at, const char *lt, const char *hat) {
    if (name[0] && name[1] == 0) {
        if (name[0] == '@') return at ? at : "";
        if (name[0] == '<') return lt ? lt : "";
        if (name[0] == '^') return hat ? hat : "";
    }
    {
        struct var *v = var_find(name);
        if (v) return v->value;
    }
    return "";
}

static int expand_into(char *dst, int dstsz, const char *src, int depth,
                       const char *at, const char *lt, const char *hat);

static int expand_name(char *dst, int dstsz, int *poff, const char *value,
                       int depth, const char *at, const char *lt, const char *hat) {
    char inner[MAX_EXPAND];
    int n;
    if (depth >= EXPAND_DEPTH) die("variable expansion nested too deep");
    n = expand_into(inner, (int)sizeof inner, value, depth + 1, at, lt, hat);
    if (n < 0) return -1;
    if (*poff + n >= dstsz) return -1;
    memcpy(dst + *poff, inner, (size_t)n);
    *poff += n;
    return 0;
}

static int expand_into(char *dst, int dstsz, const char *src, int depth,
                       const char *at, const char *lt, const char *hat) {
    int o = 0;
    const char *p = src;
    if (depth >= EXPAND_DEPTH) die("variable expansion nested too deep");
    while (*p) {
        if (*p != '$') {
            if (o + 1 >= dstsz) return -1;
            dst[o++] = *p++;
            continue;
        }
        p++;
        if (*p == '$') {
            if (o + 1 >= dstsz) return -1;
            dst[o++] = '$';
            p++;
            continue;
        }
        if (*p == '(' || *p == '{') {
            char close = (*p == '(') ? ')' : '}';
            char name[MAX_NAME];
            int n = 0;
            p++;
            while (*p && *p != close) {
                if (n + 1 >= MAX_NAME) die("variable name too long");
                name[n++] = *p++;
            }
            if (*p != close) die("unterminated $(");
            p++;
            name[n] = 0;
            if (expand_name(dst, dstsz, &o,
                            var_lookup(name, at, lt, hat),
                            depth, at, lt, hat) != 0)
                return -1;
            continue;
        }
        if (*p) {
            char name[2];
            name[0] = *p++;
            name[1] = 0;
            if (expand_name(dst, dstsz, &o,
                            var_lookup(name, at, lt, hat),
                            depth, at, lt, hat) != 0)
                return -1;
            continue;
        }
    }
    dst[o] = 0;
    return o;
}

/* ---- parse ----------------------------------------------------------- */

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
        e--;
        *e = 0;
    }
    return s;
}

static void strip_comment(char *s) {
    char *p;
    for (p = s; *p; p++) {
        if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = 0;
            return;
        }
    }
}

static int is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static void add_prereq(struct rule *r, const char *name) {
    int i;
    if (!name[0]) return;
    for (i = 0; i < r->nprereq; i++) {
        if (strcmp(r->prereqs[i], name) == 0) return;
    }
    if (r->nprereq >= MAX_PREREQS) dief("too many prerequisites on ", r->target);
    r->prereqs[r->nprereq++] = astrdup(name);
}

static void parse_prereqs(struct rule *r, char *rest) {
    char *p = rest;
    while (*p) {
        char *start;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
        add_prereq(r, start);
    }
}

static void parse_var_line(char *s) {
    char *eq = strchr(s, '=');
    char *name, *val, *nend;
    if (!eq) return;
    name = trim(s);
    *eq = 0;
    nend = eq;
    while (nend > name && (nend[-1] == ' ' || nend[-1] == '\t')) {
        nend--;
        *nend = 0;
    }
    if (!is_ident_start((unsigned char)name[0])) dief("bad variable name: ", name);
    val = trim(eq + 1);
    var_set(name, val, VAR_FILE);
}

static void parse_rule_line(char *s) {
    char *colon = strchr(s, ':');
    char *target;
    if (!colon) dief("not a rule or variable: ", s);
    *colon = 0;
    target = trim(s);
    if (!target[0]) die("empty target");
    if (strcmp(target, ".PHONY") == 0) {
        char *rest = trim(colon + 1);
        char *p = rest;
        cur_rule = 0;           /* .PHONY has no recipes of its own */
        while (*p) {
            char *start;
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
            mark_phony(start);
        }
        return;
    }
    cur_rule = rule_add(target);
    parse_prereqs(cur_rule, colon + 1);
}

static int first_eq_or_colon(const char *s) {
    const char *p;
    for (p = s; *p; p++) {
        if (*p == '=') return '=';
        if (*p == ':') return ':';
    }
    return 0;
}

static void parse_makefile(FILE *fp, const char *path) {
    char line[MAX_LINE];
    char joined[MAX_EXPAND];
    int lineno = 0;
    joined[0] = 0;
    (void)path;
    while (fgets(line, (int)sizeof line, fp)) {
        size_t n;
        char *s;
        int is_recipe;
        lineno++;
        n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = 0;
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (n && line[n - 1] == '\\') {
            line[n - 1] = 0;
            if (strlen(joined) + strlen(line) + 2 >= sizeof joined)
                die("line continuation too long");
            strcat(joined, line);
            strcat(joined, " ");
            continue;
        }
        if (joined[0]) {
            if (strlen(joined) + strlen(line) + 1 >= sizeof joined)
                die("line continuation too long");
            strcat(joined, line);
            if (strlen(joined) >= sizeof line) die("line too long after continuation");
            memcpy(line, joined, strlen(joined) + 1);
            joined[0] = 0;
        }
        is_recipe = (line[0] == '\t');
        if (is_recipe) {
            if (!cur_rule) {
                fprintf(stderr, "shmake: recipe before rule at line %d\n", lineno);
                exit(2);
            }
            if (cur_rule->nrecipe >= MAX_RECIPES)
                dief("too many recipe lines on ", cur_rule->target);
            s = line + 1;
            while (*s == '\t') s++;
            cur_rule->recipes[cur_rule->nrecipe++] = astrdup(s);
            continue;
        }
        s = trim(line);
        if (!s[0]) continue;
        strip_comment(s);
        s = trim(s);
        if (!s[0]) continue;
        if (first_eq_or_colon(s) == '=') parse_var_line(s);
        else if (first_eq_or_colon(s) == ':') parse_rule_line(s);
        else {
            fprintf(stderr, "shmake: %s:%d: not a rule or variable: %s\n",
                    path, lineno, s);
            exit(2);
        }
    }
}

/* ---- timestamps & exec ----------------------------------------------- */

/* AuraLite's SYS_STAT/SYS_OPEN copy the path verbatim; only the AT-family
 * dispatcher joins the thread cwd.  Relative `stat("a.in")` after `-C`
 * therefore misses a file that exists.  Always join getcwd() ourselves. */
static int make_abs(const char *in, char *out, int outsz) {
    size_t cl;
    int n;
    char cwd[512];
    if (!in || !in[0]) return -1;
    if (in[0] == '/') {
        if ((int)strlen(in) >= outsz) return -1;
        memcpy(out, in, strlen(in) + 1);
        return 0;
    }
    if (!getcwd(cwd, sizeof cwd)) return -1;
    cl = strlen(cwd);
    if (cl > 0 && cwd[cl - 1] == '/')
        n = snprintf(out, (size_t)outsz, "%s%s", cwd, in);
    else
        n = snprintf(out, (size_t)outsz, "%s/%s", cwd, in);
    if (n < 0 || n >= outsz) return -1;
    return 0;
}

static int file_mtime(const char *path, uint64_t *out) {
    struct stat st;
    char abs[512];
    if (make_abs(path, abs, (int)sizeof abs) != 0) return -1;
    if (stat(abs, &st) != 0) return -1;
#ifdef __AURALITE__
    *out = st.st_mtime;
#else
    *out = (uint64_t)st.st_mtime;
#endif
    return 0;
}

static int split_argv(char *s, char **argv, int max) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (n + 1 >= max) die("too many arguments in recipe");
        argv[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    argv[n] = 0;
    return n;
}

#ifdef __AURALITE__
static int resolve_cmd(const char *cmd, char *out, int outsz) {
    static const char *dirs[] = { "/bin/", "/apps/", "/demos/", "/tests/", "./", 0 };
    int i;
    struct stat st;
    if (strchr(cmd, '/')) {
        if ((int)strlen(cmd) >= outsz) return 0;
        memcpy(out, cmd, strlen(cmd) + 1);
        return stat(out, &st) == 0;
    }
    for (i = 0; dirs[i]; i++) {
        int n = snprintf(out, (size_t)outsz, "%s%s", dirs[i], cmd);
        if (n < 0 || n >= outsz) continue;
        if (stat(out, &st) == 0) return 1;
    }
    return 0;
}
#endif

static int run_argv(char *const argv[]) {
    pid_t pid;
    int st = 0;
#ifdef __AURALITE__
    char resolved[256];
    if (!resolve_cmd(argv[0], resolved, (int)sizeof resolved)) {
        fprintf(stderr, "shmake: %s: not found\n", argv[0]);
        return 127;
    }
    pid = spawnv(resolved, argv);
    if (pid < 0) {
        fprintf(stderr, "shmake: spawn %s: errno %d\n", resolved, errno);
        return 126;
    }
#else
    pid = fork();
    if (pid < 0) {
        perror("shmake: fork");
        return 126;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "shmake: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
#endif
    if (waitpid(pid, &st, 0) < 0) {
        perror("shmake: waitpid");
        return 1;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 1;
}

static int run_recipe_line(const char *expanded) {
    char buf[MAX_EXPAND];
    char *argv[MAX_ARGV];
    int n;
    if ((int)strlen(expanded) >= MAX_EXPAND) die("recipe too long");
    memcpy(buf, expanded, strlen(expanded) + 1);
    n = split_argv(buf, argv, MAX_ARGV);
    if (n == 0) return 0;
    if (dry_run) return 0;
    return run_argv(argv);
}

/* ---- build ----------------------------------------------------------- */

static int build_target(const char *name);

static int prereq_newer(struct rule *r, uint64_t t_mtime, int t_missing) {
    int i;
    for (i = 0; i < r->nprereq; i++) {
        uint64_t pm;
        struct rule *pr = rule_find(r->prereqs[i]);
        if (file_mtime(r->prereqs[i], &pm) != 0) {
            /* missing prereq is fine if it has a rule that will produce it;
             * after build_target it should exist unless phony. */
            if (pr && pr->phony) continue;
            if (t_missing) return 1;
            return 1;
        }
        if (t_missing || pm > t_mtime) return 1;
    }
    return 0;
}

static int build_target(const char *name) {
    struct rule *r = rule_find(name);
    uint64_t t_mtime = 0;
    int t_missing;
    int i, need, status;
    char hat[MAX_EXPAND];
    const char *lt;

    if (!r) {
        if (file_mtime(name, &t_mtime) == 0) return 0;  /* source file */
        fprintf(stderr, "shmake: no rule to make target '%s'\n", name);
        return 2;
    }
    if (r->visiting) {
        fprintf(stderr, "shmake: circular dependency on '%s'\n", name);
        return 2;
    }
    if (r->built) return 0;

    r->visiting = 1;
    for (i = 0; i < r->nprereq; i++) {
        status = build_target(r->prereqs[i]);
        if (status != 0) {
            r->visiting = 0;
            return status;
        }
    }

    t_missing = (file_mtime(r->target, &t_mtime) != 0);
    need = r->phony || t_missing || prereq_newer(r, t_mtime, t_missing);

    if (!need) {
        printf("shmake: '%s' is up to date\n", r->target);
        fflush(stdout);
        n_uptodate++;
        r->visiting = 0;
        r->built = 1;
        return 0;
    }

    hat[0] = 0;
    for (i = 0; i < r->nprereq; i++) {
        if (hat[0]) {
            if (strlen(hat) + 1 + strlen(r->prereqs[i]) + 1 >= sizeof hat)
                die("prerequisites too long");
            strcat(hat, " ");
        }
        strcat(hat, r->prereqs[i]);
    }
    lt = (r->nprereq > 0) ? r->prereqs[0] : "";

    if (r->nrecipe == 0) {
        /* dependency-only rule: nothing to run. */
        r->visiting = 0;
        r->built = 1;
        return 0;
    }

    for (i = 0; i < r->nrecipe; i++) {
        char exp[MAX_EXPAND];
        if (expand_into(exp, (int)sizeof exp, r->recipes[i], 0,
                        r->target, lt, hat) < 0)
            die("recipe expansion overflow");
        printf("%s\n", exp);
        fflush(stdout);
        status = run_recipe_line(exp);
        if (status != 0) {
            fprintf(stderr, "shmake: recipe for '%s' failed with status %d\n",
                    r->target, status);
            r->visiting = 0;
            return status;
        }
    }
    n_rebuilt++;
    r->visiting = 0;
    r->built = 1;
    return 0;
}

/* ---- main ------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr, "usage: shmake [-C dir] [-f makefile] [-n] [NAME=val ...] [target ...]\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *makefile = 0;
    const char *chdir_to = 0;
    const char *targets[32];
    int ntargets = 0;
    int i, status = 0;
    FILE *fp;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) usage();
        if (strcmp(argv[i], "-n") == 0) { dry_run = 1; continue; }
        if (strcmp(argv[i], "-C") == 0) {
            if (++i >= argc) usage();
            chdir_to = argv[i];
            continue;
        }
        if (strncmp(argv[i], "-C", 2) == 0 && argv[i][2]) {
            chdir_to = argv[i] + 2;
            continue;
        }
        if (strcmp(argv[i], "-f") == 0) {
            if (++i >= argc) usage();
            makefile = argv[i];
            continue;
        }
        if (strncmp(argv[i], "-f", 2) == 0 && argv[i][2]) {
            makefile = argv[i] + 2;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "shmake: unknown option %s\n", argv[i]);
            usage();
        }
        {
            const char *eq = strchr(argv[i], '=');
            if (eq && eq != argv[i] && argv[i][0] != '-') {
                char name[MAX_NAME];
                size_t nl = (size_t)(eq - argv[i]);
                if (nl >= sizeof name) die("variable name too long");
                memcpy(name, argv[i], nl);
                name[nl] = 0;
                var_set(name, eq + 1, VAR_CMDLINE);
                continue;
            }
        }
        if (ntargets >= 32) die("too many targets");
        targets[ntargets++] = argv[i];
    }

    if (chdir_to) {
        if (chdir(chdir_to) != 0) {
            fprintf(stderr, "shmake: -C %s: %s\n", chdir_to, strerror(errno));
            return 2;
        }
    }

    if (!makefile) {
        char cand[512];
        if (make_abs("Makefile", cand, (int)sizeof cand) == 0
            && access(cand, R_OK) == 0)
            makefile = astrdup(cand);
        else if (make_abs("makefile", cand, (int)sizeof cand) == 0
                 && access(cand, R_OK) == 0)
            makefile = astrdup(cand);
        else
            die("no Makefile found");
    } else {
        char abs[512];
        if (make_abs(makefile, abs, (int)sizeof abs) == 0)
            makefile = astrdup(abs);
    }

    fp = fopen(makefile, "r");
    if (!fp) {
        fprintf(stderr, "shmake: %s: %s\n", makefile, strerror(errno));
        return 2;
    }
    parse_makefile(fp, makefile);
    fclose(fp);

    if (!default_target && ntargets == 0) die("no targets");

    if (ntargets == 0) {
        targets[0] = default_target;
        ntargets = 1;
    }

    for (i = 0; i < ntargets; i++) {
        status = build_target(targets[i]);
        if (status != 0) return status;
    }

    printf("[shmake] rebuilt=%d uptodate=%d\n", n_rebuilt, n_uptodate);
    fflush(stdout);
    return 0;
}
