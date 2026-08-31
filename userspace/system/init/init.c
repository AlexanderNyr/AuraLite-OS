/*
 * init.c — PID 1 / interactive shell for AuraLite OS.
 *
 * Runs as the first (and currently only) user process. Reads commands from
 * serial input (stdin = fd 0) and dispatches built-in commands. This satisfies
 * the Phase 11 gate criterion: "full boot to shell; ls / lists files."
 *
 * Built-in commands:
 *   ls [path]  — list files in a directory
 *   cat <file> — print a file's contents
 *   echo <...> — print arguments
 *   pwd        — print working directory
 *   uname      — print OS info
 *   free       — print memory stats
 *   kbd [name] — show/set keyboard layout
 *   help       — list commands
 *   exit       — halt
 */

#include "unistd.h"
#include "fcntl.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/wait.h"
#include "signal.h"

#include "sh_expand.h"
#include "sh_parse.h"

/* SELFHOST SH2: INPUT_MAX 256 -> 512 and MAX_ARGS 8 -> 32 so the guest
 * toolchain's link lines fit (a tcc link of the userland names crt0,
 * libc, malloc, env, string/stdlib extras, the app and libtcc1.a --
 * 9+ argv entries, beyond the old 8-slot cap, which silently truncated
 * the line and produced "unresolved reference to '__libc_start_main'"
 * from a tcc that never saw libc.o). */
#define INPUT_MAX 512
#define MAX_ARGS  32

/* Line buffer and token storage (in BSS, zero-filled by the ELF loader). */
static char input_line[INPUT_MAX];
static char *cmd_argv[MAX_ARGS];

/* ---- Job Control ---- */
#define MAX_JOBS 16
struct job {
    int   id;
    pid_t pgid;
    char  cmd[64];
    int   running;
};
static struct job job_list[MAX_JOBS];
static int next_job_id = 1;
static int in_subshell = 0;

/* ---- SELFHOST SH6a: exit status and script execution ----
 *
 * The shell had no notion of a command failing: every builtin returned void
 * and cmd_run_argv computed the child's wait status and then threw it away.
 * Nothing downstream could branch on an error, which is the first thing a
 * build script needs -- `sh build.sh kernel` must stop when a compile fails,
 * not carry on and report success.
 *
 * So SH6a lays the two pieces every later sub-phase stands on:
 *
 *   last_status  exit status of the most recent command, also what $? reads;
 *   cmd_sh()     `sh <file> [args...]` runs a file of commands in this same
 *                shell, with positional parameters and line-numbered errors.
 *
 * The script runs in-process rather than as a separate /bin/sh because the
 * builtins, the search path and the job table all live here; a separate
 * program would have to re-implement or re-expose all three.  See D10.
 */
#define SH_MAX_DEPTH 4          /* nested `sh` calls; build.sh needs 1 */
#define SH_ARG_MAX   64         /* per-argument copy limit */
#define SH_FILE_MAX  (64 * 1024)

struct sh_frame {
    const char *path;               /* script being run, for diagnostics */
    int         line;               /* 1-based line number inside it */
    int         argc;               /* positional params, argv-style */
    char       *argv[MAX_ARGS];
    char       *text;               /* whole file; owned by this frame */
    size_t      text_len;
    size_t      cursor;
    int         exit_req;           /* an `exit N` asked to stop the script */
    int         exit_code;
};
static struct sh_frame sh_stack[SH_MAX_DEPTH];
static int sh_depth = 0;
static int last_status = 0;

/* Positional arguments are copied here rather than aliased.  At the top level
 * they point into input_line, which the next prompt read overwrites; a nested
 * script's point into its parent's text buffer.  Copying removes both hazards
 * for 8 KiB of BSS. */
static char sh_argbuf[SH_MAX_DEPTH][MAX_ARGS][SH_ARG_MAX];

/* ---- SELFHOST SH6b: named variables, quotes and redirects ----
 *
 * Three limits, each chosen from something measured rather than guessed:
 *
 *   SH_MAX_TOKS   a line can carry at most MAX_ARGS arguments plus two tokens
 *                 per redirect; the longest command any integration case sends
 *                 is 14 words.  Twice MAX_ARGS plus slack is far beyond that,
 *                 and unlike the old strtok loop the parser REPORTS an
 *                 overflow instead of silently dropping the tail -- silent
 *                 truncation is ledger SH-14.
 *   SH_ARG_MAX_EXP  expansion can make a word longer than the line it came
 *                 from ($OUT/$1.o with a long $OUT), so arguments get their
 *                 own buffers rather than pointing into input_line.
 *   SH_MAX_VARS   `set` is for build configuration, not for data.
 */
#define SH_MAX_TOKS     (MAX_ARGS * 2 + 8)
#define SH_MAX_REDIR    4
#define SH_ARG_MAX_EXP  512
#define SH_MAX_VARS     32
#define SH_VAR_NAME_MAX 32
#define SH_VAR_VAL_MAX  128

/* Expanded arguments and redirect targets.  Static, because cmd_argv is read
 * after process_command's own frame is gone in the background-fork path. */
static char sh_expbuf[MAX_ARGS][SH_ARG_MAX_EXP];
static char sh_redirbuf[SH_MAX_REDIR][SH_ARG_MAX_EXP];

static char sh_var_name[SH_MAX_VARS][SH_VAR_NAME_MAX];
static char sh_var_val[SH_MAX_VARS][SH_VAR_VAL_MAX];
static struct sh_var sh_vars[SH_MAX_VARS];
static int sh_nvars = 0;

struct sh_redir {
    int         op;      /* SH_TOK_GT / SH_TOK_GGT / SH_TOK_LT */
    const char *path;
};

/* The variable table as the expander sees it. */
static const struct sh_var *sh_var_table(void) {
    for (int i = 0; i < sh_nvars; i++) {
        sh_vars[i].name  = sh_var_name[i];
        sh_vars[i].value = sh_var_val[i];
    }
    return sh_vars;
}

static int sh_var_index(const char *name) {
    for (int i = 0; i < sh_nvars; i++)
        if (strcmp(sh_var_name[i], name) == 0) return i;
    return -1;
}

/* Returns 0 on success, -1 when the name is malformed or the table is full. */
static int sh_var_set(const char *name, const char *value) {
    if (!name || !*name || !sh_is_name_start(name[0])) return -1;
    for (const char *q = name + 1; *q; q++)
        if (!sh_is_name_char(*q)) return -1;

    int i = sh_var_index(name);
    if (i < 0) {
        if (sh_nvars >= SH_MAX_VARS) return -1;
        i = sh_nvars++;
        strncpy(sh_var_name[i], name, SH_VAR_NAME_MAX - 1);
        sh_var_name[i][SH_VAR_NAME_MAX - 1] = '\0';
    }
    strncpy(sh_var_val[i], value, SH_VAR_VAL_MAX - 1);
    sh_var_val[i][SH_VAR_VAL_MAX - 1] = '\0';
    return 0;
}

static void sh_var_unset(const char *name) {
    int i = sh_var_index(name);
    if (i < 0) return;
    for (int k = i; k < sh_nvars - 1; k++) {
        strcpy(sh_var_name[k], sh_var_name[k + 1]);
        strcpy(sh_var_val[k],  sh_var_val[k + 1]);
    }
    sh_nvars--;
}

/* Recognise a bare `NAME=VALUE` word, which is an assignment rather than a
 * command.  Checked before the search path so `CC=tcc` cannot be mistaken for
 * a program named "CC=tcc". */
static int sh_try_assign(const char *word) {
    const char *eq = strchr(word, '=');
    if (!eq || eq == word) return 0;
    char name[SH_VAR_NAME_MAX];
    size_t n = (size_t)(eq - word);
    if (n >= sizeof name) return 0;
    memcpy(name, word, n);
    name[n] = '\0';
    if (!sh_is_name_start(name[0])) return 0;
    for (size_t i = 1; i < n; i++)
        if (!sh_is_name_char(name[i])) return 0;
    return sh_var_set(name, eq + 1) == 0;
}

/* Apply redirections by swapping fd 0/1, remembering what was there.
 *
 * The same mechanism serves builtins and spawned programs: a builtin writes to
 * fd 1 like anything else, and a child inherits the parent's descriptor table
 * at spawn.  stdout is flushed on both sides of the swap, because buffered
 * output must reach the ORIGINAL destination -- flushing after the dup2 would
 * put the previous command's tail into this command's file. */
static void sh_redir_restore(int saved_in, int saved_out) {
    fflush(stdout);
    if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
    if (saved_in  >= 0) { dup2(saved_in,  0); close(saved_in); }
}

static int sh_redir_apply(const struct sh_redir *r, int n,
                          int *saved_in, int *saved_out) {
    *saved_in = -1;
    *saved_out = -1;
    if (n == 0) return 0;

    fflush(stdout);
    for (int i = 0; i < n; i++) {
        int flags, target;
        switch (r[i].op) {
        case SH_TOK_LT: flags = O_RDONLY;                        target = 0; break;
        case SH_TOK_GT: flags = O_WRONLY | O_CREAT | O_TRUNC;    target = 1; break;
        default:        flags = O_WRONLY | O_CREAT | O_APPEND;   target = 1; break;
        }

        int fd = open(r[i].path, flags, 0644);
        if (fd < 0) {
            /* Restore BEFORE reporting, so the message reaches the terminal
             * rather than a file that was half-redirected already. */
            sh_redir_restore(*saved_in, *saved_out);
            *saved_in = -1;
            *saved_out = -1;
            printf("sh: %s: cannot open for %s\n",
                   r[i].path, sh_tok_name(r[i].op));
            return -1;
        }

        int *saved = (target == 0) ? saved_in : saved_out;
        if (*saved < 0) *saved = dup(target);
        if (*saved < 0 || dup2(fd, target) < 0) {
            close(fd);
            sh_redir_restore(*saved_in, *saved_out);
            *saved_in = -1;
            *saved_out = -1;
            printf("sh: cannot redirect fd %d\n", target);
            return -1;
        }
        close(fd);
    }
    return 0;
}

/* `set` with no arguments lists the table; with arguments it assigns. */
static int cmd_set(int argc, char **argv) {
    if (argc < 2) {
        for (int i = 0; i < sh_nvars; i++)
            printf("%s=%s\n", sh_var_name[i], sh_var_val[i]);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i]) {
            printf("set: %s: expected NAME=VALUE\n", argv[i]);
            return 2;
        }
        char name[SH_VAR_NAME_MAX];
        size_t n = (size_t)(eq - argv[i]);
        if (n >= sizeof name) {
            printf("set: name too long (max %d)\n", SH_VAR_NAME_MAX - 1);
            return 2;
        }
        memcpy(name, argv[i], n);
        name[n] = '\0';
        if (sh_var_set(name, eq + 1) != 0) {
            printf("set: %s: bad name or table full (max %d variables)\n",
                   name, SH_MAX_VARS);
            return 2;
        }
    }
    return 0;
}

static int cmd_unset(int argc, char **argv) {
    if (argc < 2) {
        puts("unset: missing variable name");
        return 2;
    }
    for (int i = 1; i < argc; i++) sh_var_unset(argv[i]);
    return 0;
}

/* Read a whole file into a fresh malloc'd buffer.  Scripts are read in full
 * so that a nested `sh` cannot clobber the outer script's buffer mid-run. */
static char *read_whole_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    size_t cap = 1024, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return 0; }

    /* The `+ 1` reserves the byte the NUL terminator needs after the loop.
     * Without it the invariant is still safe -- the growth check runs before
     * every read, so `len == cap` is always caught on the next pass -- but
     * only by an argument this subtle, and a boundary that needs a proof to
     * be correct is a boundary that will be broken by the next edit. */
    for (;;) {
        if (len + 256 + 1 > cap) {
            if (cap >= SH_FILE_MAX) {
                printf("sh: %s: script larger than %d bytes\n",
                       path, SH_FILE_MAX);
                free(buf);
                close(fd);
                return 0;
            }
            cap *= 2;
            if (cap > SH_FILE_MAX) cap = SH_FILE_MAX;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return 0; }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, 256);
        if (n < 0) { free(buf); close(fd); return 0; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* Pull the next line out of a frame's text, in place.  Returns 0 at EOF.
 * The line is NUL-terminated by overwriting the newline. */
static int sh_next_line(struct sh_frame *f, char **out) {
    if (f->cursor >= f->text_len) return 0;

    char *start = f->text + f->cursor;
    char *nl = (char *)memchr(start, '\n', f->text_len - f->cursor);
    if (nl) {
        *nl = '\0';
        f->cursor = (size_t)(nl - f->text) + 1;
    } else {
        f->cursor = f->text_len;
    }
    *out = start;
    return 1;
}

/* A line that carries nothing: empty, or whitespace, or a whole-line comment.
 * A '#' later in the line is NOT a comment yet -- that needs quoting rules to
 * get right (`echo a#b` is legal), which is SH6b. */
static int sh_line_is_blank(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return (*s == '\0' || *s == '#');
}

/* ---- SELFHOST SH6d: control-flow source ----
 *
 * if/while/for span lines, so they cannot live only in process_command.
 * A sh_src is either the running script frame or a collected body (an
 * array of line pointers into that frame's text).  Nested compounds
 * collect from the body they sit in, not from the frame -- otherwise a
 * nested `if` inside a `while` would steal lines past `done`.
 *
 * Keywords stay WORDS in the tokenizer.  Whether `if` opens a compound
 * depends on it being the first word of a line, which is a command-level
 * fact, not a token-level one.  Quoting `if` therefore keeps it a command
 * name, matching POSIX reserved-word rules for this subset.
 */
enum {
    SH_KW_NONE = 0,
    SH_KW_IF, SH_KW_THEN, SH_KW_ELIF, SH_KW_ELSE, SH_KW_FI,
    SH_KW_WHILE, SH_KW_DO, SH_KW_DONE,
    SH_KW_FOR, SH_KW_IN, SH_KW_BREAK
};

#define SH_MAX_BODY  64
#define SH_MAX_LOOP  1024

struct sh_src {
    struct sh_frame *f;     /* script frame; NULL at the prompt */
    char           **lines; /* collected body; takes precedence over f */
    int             nlines;
    int             iline;
    char           *unread; /* one-line pushback */
};

static int sh_loop_depth = 0;
static int sh_break_req  = 0;

static int sh_word_kw(const struct sh_tok *t)
{
    if (!t || t->type != SH_TOK_WORD) return SH_KW_NONE;
#define SH_K(s, id) \
    if (t->len == sizeof(s) - 1 && memcmp(t->text, s, t->len) == 0) return (id)
    SH_K("if",    SH_KW_IF);
    SH_K("then",  SH_KW_THEN);
    SH_K("elif",  SH_KW_ELIF);
    SH_K("else",  SH_KW_ELSE);
    SH_K("fi",    SH_KW_FI);
    SH_K("while", SH_KW_WHILE);
    SH_K("do",    SH_KW_DO);
    SH_K("done",  SH_KW_DONE);
    SH_K("for",   SH_KW_FOR);
    SH_K("in",    SH_KW_IN);
    SH_K("break", SH_KW_BREAK);
#undef SH_K
    return SH_KW_NONE;
}

static int sh_kw_open(int k)  { return k == SH_KW_IF || k == SH_KW_WHILE || k == SH_KW_FOR; }
static int sh_kw_close(int k) { return k == SH_KW_FI || k == SH_KW_DONE; }

static int sh_src_next(struct sh_src *s, char **out)
{
    if (!s) return 0;
    if (s->unread) {
        *out = s->unread;
        s->unread = 0;
        return 1;
    }
    if (s->lines) {
        if (s->iline >= s->nlines) return 0;
        *out = s->lines[s->iline++];
        return 1;
    }
    if (s->f) {
        char *line;
        if (!sh_next_line(s->f, &line)) return 0;
        s->f->line++;
        *out = line;
        return 1;
    }
    return 0;
}

static void sh_src_unread(struct sh_src *s, char *line)
{
    s->unread = line;
}

static int sh_stopped(void)
{
    if (sh_break_req) return 1;
    if (sh_depth > 0 && sh_stack[sh_depth - 1].exit_req) return 1;
    return 0;
}

static int sh_exec_line(struct sh_src *src, char *line);

static void add_job(pid_t pgid, const char *cmd) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_list[i].id == 0) {
            job_list[i].id = next_job_id++;
            job_list[i].pgid = pgid;
            strncpy(job_list[i].cmd, cmd, 63);
            job_list[i].cmd[63] = '\0';
            job_list[i].running = 1;
            printf("[%d] %d\n", job_list[i].id, (int)pgid);
            fflush(stdout);
            return;
        }
    }
}

static void remove_job(pid_t pgid, int status) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_list[i].id != 0 && job_list[i].pgid == pgid) {
            if (WIFSTOPPED(status)) {
                job_list[i].running = 0;
                printf("[%d] Stopped %s\n", job_list[i].id, job_list[i].cmd);
            } else {
                printf("[%d] Done %s\n", job_list[i].id, job_list[i].cmd);
                job_list[i].id = 0;
            }
            fflush(stdout);
            return;
        }
    }
}

static void cmd_jobs(void) {
    int count = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_list[i].id != 0) {
            const char *status = job_list[i].running ? "Running" : "Stopped";
            printf("[%d] %s %s\n", job_list[i].id, status, job_list[i].cmd);
            count++;
        }
    }
    if (count == 0) {
        printf("no jobs\n");
    }
    fflush(stdout);
}

static void cmd_fg(const char *arg) {
    int target_id = -1;
    if (arg) {
        if (*arg == '%') arg++;
        target_id = 0;
        while (*arg >= '0' && *arg <= '9') { target_id = target_id * 10 + (*arg++ - '0'); }
    }
    struct job *j = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_list[i].id != 0) {
            if (target_id <= 0 || job_list[i].id == target_id) { j = &job_list[i]; break; }
        }
    }
    if (!j) { puts("fg: no such job"); fflush(stdout); return; }
    printf("%s\n", j->cmd);
    fflush(stdout);
    tcsetpgrp(0, j->pgid);
    /* FIX_R6: SIGCONT the whole process GROUP (kill(-pgid)), not just the
     * leader — a job may have helper processes in its group (stoptest's
     * stdin pump), and they must resume as well or nobody consumes the
     * console's next control bytes. */
    if (!j->running) kill(-j->pgid, SIGCONT);
    int status = 0;
    waitpid(j->pgid, &status, WUNTRACED);
    tcsetpgrp(0, getpid());
    if (WIFSTOPPED(status)) {
        j->running = 0;
        printf("[%d] Stopped %s\n", j->id, j->cmd);
    } else {
        j->id = 0;
    }
    fflush(stdout);
}

static void cmd_bg(const char *arg) {
    int target_id = -1;
    if (arg) {
        if (*arg == '%') arg++;
        target_id = 0;
        while (*arg >= '0' && *arg <= '9') { target_id = target_id * 10 + (*arg++ - '0'); }
    }
    struct job *j = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (job_list[i].id != 0 && !job_list[i].running) {
            if (target_id <= 0 || job_list[i].id == target_id) { j = &job_list[i]; break; }
        }
    }
    if (!j) { puts("bg: no stopped job"); fflush(stdout); return; }
    j->running = 1;
    printf("[%d] %s &\n", j->id, j->cmd);
    fflush(stdout);
    kill(-j->pgid, SIGCONT);   /* FIX_R6: resume the whole group, as in fg */
}

static void cmd_sleep(const char *arg) {
    if (!arg) return;
    int sec = 0;
    while (*arg >= '0' && *arg <= '9') { sec = sec * 10 + (*arg++ - '0'); }
    if (sec <= 0) return;
    alarm((unsigned)sec);
    pause();
}

/* ---- Command implementations ---- */

static void cmd_ls(const char *path) {
    if (!path || !*path) {
        path = "/";
    }
    listdir(path);
}

/* SELFHOST SH6b: with no argument, cat reads fd 0.  Before this the `<`
 * redirect had nothing to feed -- every builtin that could consume stdin
 * demanded a filename instead -- so `cat < file` printed "missing file" and
 * the feature was syntax without a use.  At the prompt that means `cat`
 * alone echoes the console until end of input, which is what every other
 * cat does. */
static void cmd_cat(const char *path) {
    int fd = path ? open(path, O_RDONLY) : 0;
    if (fd < 0) {
        printf("cat: %s: no such file\n", path);
        return;
    }
    char buf[128];
    int64_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    if (path) close(fd);   /* never close fd 0 -- it is the shell's input */
}

static void cmd_echo(int argc, char **argv) {
    /* Do not mix buffered stdio (putchar) with direct write() calls here:
     * delayed buffered separators used to arrive after all arguments, turning
     * `echo one two` into `onetwo ` on the serial console.
     *
     * Assemble the whole line and emit it with a SINGLE write() so the kernel
     * writes it under one print_lock.  Writing each argument and each space as
     * its own write() released print_lock between syscalls, letting an SMP
     * kernel kprintf() (e.g. `[thread] reaped ...`) splice into the middle of
     * the serial line -- the recurring source of flaky console-marker
     * integration tests (shmake, sh7c).  A single write() of a short line maps
     * to one kputs_locked() and cannot be interleaved. */
    char buf[INPUT_MAX + 32];
    size_t n = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && n + 1 < sizeof(buf)) buf[n++] = ' ';
        size_t len = strlen(argv[i]);
        if (len > sizeof(buf) - 1 - n) len = sizeof(buf) - 1 - n;
        memcpy(buf + n, argv[i], len);
        n += len;
    }
    if (n < sizeof(buf)) buf[n++] = '\n';
    write(1, buf, n);
}

static void cmd_write_file(int argc, char **argv) {
    if (argc < 3) {
        puts("usage: write <file> <text>");
        fflush(stdout);
        return;
    }
    int fd = open(argv[1], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("write: cannot open/create %s\n", argv[1]);
        fflush(stdout);
        return;
    }
    for (int i = 2; i < argc; i++) {
        if (i > 2) write(fd, " ", 1);
        write(fd, argv[i], strlen(argv[i]));
    }
    write(fd, "\n", 1);
    close(fd);
    printf("wrote %s\n", argv[1]);
    fflush(stdout);
}

static void cmd_pwd(void) {
    puts("/");
    fflush(stdout);
}

static void cmd_uname(void) {
    puts("AuraLite OS 0.0.1 x86_64");
    fflush(stdout);
}

static void cmd_free(void) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        int64_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            write(1, buf, (size_t)n);
        }
        close(fd);
    } else {
        puts("              total        used        free");
        puts("Mem:          510MiB      ~32MiB      478MiB");
    }
}

static void cmd_help(void) {
    puts("AuraLite OS shell commands:");
    puts("  ls [path]   - list directory contents");
    puts("  cat <file>  - print file contents");
    puts("  echo <...>  - print arguments");
    puts("  write <file> <text> - create/overwrite writable file (/tmp, /disk, /fat)");
    puts("  run <prog>  - run a program in its own address space");
    puts("  pwd         - print working directory");
    puts("  uname       - print OS information");
    puts("  free        - print memory usage");
    puts("  nslookup    - resolve a hostname via DNS");
    puts("  dnscache    - show DNS cache and servers");
    puts("  dnsset <ip> [ip2] - override DNS servers (debug)");
    puts("  dnsflush    - clear the DNS cache");
    puts("  dnstc       - force the next DNS answer truncated (TCP fallback test)");
    puts("  ping <host> - ping a hostname via ICMP");
    puts("  ping6 <addr>- ping an IPv6 link-local neighbour (e.g. fe80::2)");
    puts("  ps          - list processes (stub)");
    puts("  mkdir <dir> - create a directory  (FAT32 / ext2)");
    puts("  rmdir <dir> - remove an empty directory");
    puts("  rm <file>   - delete a file");
    puts("  mv <a> <b>  - rename a file or directory");
    puts("  touch <file>- create an empty file");
    puts("  stat <path> - show file metadata");
    puts("  apm [cmd]   - AuraLite Package Manager");
    puts("  kbd [name]  - show/set keyboard layout (us, de)");
    puts("  help        - show this help");
    puts("  sh <file> [args] - run a script ($0..$9, $#, $?, $NAME)");
    puts("  set [NAME=VALUE] - assign a variable; no args lists them");
    puts("  unset NAME      - remove a variable");
    puts("  redirection: cmd > file, cmd >> file, cmd < file");
    puts("  pipes/lists: cmd | cmd ; cmd && cmd || cmd");
    puts("  if/then/elif/else/fi, while/do/done, for x in ...; break");
    puts("  true/false   - status 0 / 1");
    puts("  exit        - exit shell");
    puts("");
    /* Programs are named, not pathed: since F5 there is exactly one location
     * per program and it is found on the search path.  Printing paths here
     * would be printing something the user does not need to type, and would
     * be one more place to update the next time the layout moves. */
    puts("Applications (run <name>, or just <name>):");
    puts("  calc      - interactive calculator");
    puts("  sysinfo   - system information");
    puts("  editor    - text editor");
    puts("  clock     - clock display");
    puts("  guess     - number guessing game");
    puts("  snake     - snake game");
    puts("  glrunner  - Cube Runner 3D game (A/D steer, Space pause, R restart)");
    puts("  hello     - hello world");
    puts("  http      - HTTP client");
    puts("  trustinfo - show shipped TLS trust-store roots and their expiry");
    puts("  weather   - live weather report (wttr.in): weather [full|3] <city>");
    puts("  gweather  - GUI weather window (Riga, wttr.in)");
    puts("  browser   - web browser (fetch + render HTML)");
    puts("  gbrowser  - GUI browser (renders HTML, links, canvas)");
    puts("  gtaskmgr  - GUI Task Manager");
    puts("  play <song> - CLI audio player (starwars, ode)");
    puts("  gaudio    - GUI music player");
    puts("");
    puts("Searched in: /bin /apps /demos /tests /opt");
}

/* ---- Program search path (FSLAYOUT_PLAN phase F2) ----
 *
 * The list and the lookup live in libc (libc/src/progpath.c), not here.  The
 * shell is not the only thing that launches programs — the GUI launcher does
 * too — and two copies of a search list is two things to update in F3, of
 * which the second is the one that gets forgotten.
 */

/* Report a failed search naming what was looked at.  A bare "not found"
 * makes the user guess where the shell even looked. */
static void report_not_found(const char *name) {
    printf("%s: not found in", name);
    for (int d = 0; d < prog_path_count(); d++) {
        printf("%s%s", d ? ":" : " ", prog_path_entry(d));
    }
    printf("\n");
    fflush(stdout);
}

/* Run a program, optionally with arguments.
 *
 * @argv is NULL-terminated and conventionally starts with the program name,
 * or NULL for no arguments at all.  Before SDK_PLAN phase S3 there was no way
 * to pass any: the convention was to write them to a file the child agreed to
 * read (/tmp/apm.args and friends). */
static int  cmd_run_argv(const char *prog, char *const argv[]);

/* No-argument form, for the call sites that have nothing to pass. */
static int  cmd_run(const char *prog) { return cmd_run_argv(prog, 0); }

/* Does @path name a PE image that imports anything?  (WIN32_PLAN.md W32-8.)
 *
 * The kernel already recognises PE by magic and loads it (W32-3), so a
 * self-contained .exe runs when spawned directly.  But nothing binds its
 * import table on that path -- import resolution lives in user space by
 * design (decision D2) -- so an .exe that imports even one KERNEL32 function
 * loads and then faults on its first call through an unwritten IAT slot.
 *
 * Rather than leave that trap for the user, the shell detects the case and
 * routes such a binary through /apps/w32run, which maps it, binds the
 * imports and runs the CRT startup sequence.  A PE with NO imports still
 * goes straight to the kernel loader, because that path is the hardened one:
 * it applies per-section W^X, which w32run cannot.
 *
 * Only the headers are read.  Everything is bounds-checked against what was
 * actually read, since this runs on any file the user names.
 */
static int pe_needs_w32run(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    unsigned char h[1024];
    long n = read(fd, h, (long)sizeof h);
    close(fd);
    if (n < 512) return 0;

    if (h[0] != 'M' || h[1] != 'Z') return 0;

    unsigned long e_lfanew = (unsigned long)h[0x3C]
                           | ((unsigned long)h[0x3D] << 8)
                           | ((unsigned long)h[0x3E] << 16)
                           | ((unsigned long)h[0x3F] << 24);
    /* The optional header must fit in what we read, or this is not a file we
     * can classify from the headers alone. */
    if (e_lfanew + 24 + 240 > (unsigned long)n) return 0;

    const unsigned char *pe = h + e_lfanew;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) return 0;

    const unsigned char *opt = pe + 24;
    unsigned magic = (unsigned)opt[0] | ((unsigned)opt[1] << 8);
    if (magic != 0x20B) return 0;          /* PE32+ only */

    /* Data directory 1 is the import table; a non-zero size means the image
     * has imports that something must bind. */
    const unsigned char *imp = opt + 112 + 1 * 8;
    unsigned long imp_rva = (unsigned long)imp[0] | ((unsigned long)imp[1] << 8)
                          | ((unsigned long)imp[2] << 16)
                          | ((unsigned long)imp[3] << 24);
    unsigned long imp_size = (unsigned long)imp[4] | ((unsigned long)imp[5] << 8)
                           | ((unsigned long)imp[6] << 16)
                           | ((unsigned long)imp[7] << 24);
    return (imp_rva != 0 && imp_size != 0);
}

/* Returns the child's exit status (128+n if a signal killed it), or the
 * shell's own error codes when the program never ran: 2 usage, 127 not
 * found, 126 spawn failed, 1 waitpid failed.  SELFHOST SH6a: this used to
 * compute `status` from waitpid and discard it, so a failing program was
 * indistinguishable from a succeeding one. */
static int cmd_run_argv(const char *prog, char *const argv[]) {
    if (!prog) {
        puts("run: missing program name");
        return 2;
    }
    char resolved[128];
    if (!prog_resolve(prog, resolved, (int)sizeof(resolved))) {
        report_not_found(prog);
        return 127;
    }
    prog = resolved;

    /* A PE with imports needs the user-space binder; see pe_needs_w32run().
     *
     * The test is on the FILE, not on whether the caller supplied an argv:
     * `run x.exe` always passes one, so gating on argv skipped every case
     * this was written for. */
    char *w32_argv[3];
    char w32_path[128];
    if (pe_needs_w32run(prog)) {
        if (prog_resolve("w32run", w32_path, (int)sizeof(w32_path))) {
            printf("[shell] %s imports Win32 DLLs; running via %s\n",
                   prog, w32_path);
            w32_argv[0] = w32_path;
            w32_argv[1] = (char *)prog;
            w32_argv[2] = 0;
            argv = w32_argv;
            prog = w32_path;
        } else {
            /* Saying so beats spawning it anyway and letting it fault with
             * no explanation. */
            printf("[shell] %s needs w32run, which is not installed\n", prog);
            return 127;
        }
    }

    printf("running %s in isolated address space...\n", prog);
    fflush(stdout);
    pid_t pid = argv ? spawnv(prog, argv) : spawn(prog);
    if (pid < 0) {
        printf("run: failed to spawn %s\n", prog);
        fflush(stdout);
        return 126;
    }
    setpgid(pid, pid);
    tcsetpgrp(0, pid);
    printf("[shell] child PID %lld, waiting...\n", (long long)pid);
    fflush(stdout);

    int status = 0;
    pid_t got;
    do {
        got = waitpid(pid, &status, WUNTRACED);
    } while (got == 0);

    tcsetpgrp(0, getpid());

    int rc;
    if (got < 0) {
        printf("[shell] waitpid failed for %s\n", prog);
        rc = 1;
    } else if (WIFSTOPPED(status)) {
        add_job(pid, prog);
        job_list[next_job_id - 2].running = 0;
        printf("\n[%d] Stopped %s\n", job_list[next_job_id - 2].id, prog);
        rc = 0;   /* suspended, not failed; `fg` resumes it */
    } else {
        printf("[shell] child exited\n");
        rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    fflush(stdout);
    return rc;
}

static void cmd_ping(const char *host) {
    if (!host) {
        puts("ping: missing hostname");
        return;
    }
    printf("Resolving %s...\n", host);
    uint32_t ip = dns_resolve(host);
    if (ip == 0) {
        printf("ping: could not resolve %s\n", host);
        return;
    }
    char ipstr[20];
    /* Simple IP to string. */
    int pos = 0;
    unsigned o;
    for (int i = 3; i >= 0; i--) {
        o = (ip >> (i * 8)) & 0xFF;
        if (o >= 100) ipstr[pos++] = '0' + o / 100;
        if (o >= 10) ipstr[pos++] = '0' + (o / 10) % 10;
        ipstr[pos++] = '0' + o % 10;
        if (i > 0) ipstr[pos++] = '.';
    }
    ipstr[pos] = 0;
    printf("Pinging %s (%s)...\n", host, ipstr);
    if (net_ping(ip) == 0) {
        printf("Reply received from %s!\n", ipstr);
    } else {
        printf("No reply from %s\n", ipstr);
    }
}

static void cmd_nslookup(const char *hostname) {
    if (!hostname) {
        puts("nslookup: missing hostname");
        return;
    }
    uint32_t ip = dns_resolve(hostname);
    if (ip != 0) {
        printf("%s -> %u.%u.%u.%u\n", hostname,
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 8) & 0xFF, ip & 0xFF);
    } else {
        printf("nslookup: failed to resolve %s\n", hostname);
    }
}

/* X3: tiny dotted-quad parser (host-order result), avoids pulling in
 * arpa/inet.h for three debug commands. */
static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (!s || *s < '0' || *s > '9') return -1;
        int v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 255) return -1; s++; }
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3) { if (*s != '.') return -1; s++; }
    }
    if (*s != 0) return -1;
    *out = ip;
    return 0;
}

static void cmd_dnscache(void) {
    uint32_t servers[8];
    int ns = dnsctl(DNSCTL_GET_SERVERS, servers, sizeof(servers));
    printf("DNS servers (%d):\n", ns);
    for (int i = 0; i < ns; i++)
        printf("  #%d  %u.%u.%u.%u\n", i + 1,
               (servers[i] >> 24) & 0xFF, (servers[i] >> 16) & 0xFF,
               (servers[i] >> 8) & 0xFF, servers[i] & 0xFF);
    dnsctl_entry_t entries[16];
    int n = dnsctl(DNSCTL_LIST, entries, sizeof(entries));
    printf("DNS cache (%d entries):\n", n);
    for (int i = 0; i < n; i++) {
        if (entries[i].negative)
            printf("  %-40s NEGATIVE (ttl %us left)\n", entries[i].name, entries[i].ttl_left);
        else
            printf("  %-40s %u.%u.%u.%u (ttl %us left)\n", entries[i].name,
                   (entries[i].ip >> 24) & 0xFF, (entries[i].ip >> 16) & 0xFF,
                   (entries[i].ip >> 8) & 0xFF, entries[i].ip & 0xFF,
                   entries[i].ttl_left);
    }
}

static void cmd_dnsset(int argc, char **argv) {
    if (argc < 2) {
        puts("dnsset: usage: dnsset <primary-ip> [secondary-ip] ...");
        return;
    }
    uint32_t servers[8];
    int n = 0;
    for (int i = 1; i < argc && n < 8; i++) {
        if (parse_ipv4(argv[i], &servers[n]) != 0) {
            printf("dnsset: bad IPv4 address '%s'\n", argv[i]);
            return;
        }
        n++;
    }
    if (dnsctl(DNSCTL_SET_SERVERS, servers, (uint32_t)(n * 4)) < 0) {
        puts("dnsset: kernel refused the server list");
        return;
    }
    printf("dnsset: %d server(s) configured\n", n);
}

static void cmd_dnsflush(void) {
    dnsctl(DNSCTL_FLUSH, 0, 0);
    puts("dnsflush: cache cleared");
}

/* ---- X7: ping6 ---- */

/* Parse a text IPv6 address into 16 bytes (a small, dependency-free subset
 * supporting "::" and groups; the kernel validates the bytes it uses). */
static int parse_ipv6(const char *s, uint8_t out[16]) {
    int groups[8];
    int ng = 0, dc = -1;
    int val = -1;
    const char *p = s;
    memset(out, 0, 16);
    for (;;) {
        char c = *p;
        if (c == ':' || c == '\0') {
            if (val >= 0) {
                if (ng >= 8) return -1;
                groups[ng++] = val;
                val = -1;
            } else if (c == ':') {
                if (p[1] == ':') { if (dc >= 0) return -1; dc = ng; p += 2; continue; }
                return -1;
            }
            if (c == '\0') break;
            if (p[1] == ':') { if (dc >= 0) return -1; dc = ng; p += 2; continue; }
            p++;
            continue;
        }
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        val = (val < 0) ? v : (val << 4) | v;
        if (val > 0xFFFF) return -1;
        p++;
    }
    if (dc >= 0) {
        int fill = 8 - ng;
        if (fill < 1) return -1;
        int k = 0, i;
        for (i = 0; i < dc; i++) { out[k++] = (groups[i] >> 8) & 0xFF; out[k++] = groups[i] & 0xFF; }
        k += fill * 2;
        for (i = dc; i < ng; i++) { out[k++] = (groups[i] >> 8) & 0xFF; out[k++] = groups[i] & 0xFF; }
        return 0;
    }
    if (ng != 8) return -1;
    for (int i = 0; i < 8; i++) { out[i * 2] = (groups[i] >> 8) & 0xFF; out[i * 2 + 1] = groups[i] & 0xFF; }
    return 0;
}

static void cmd_ping6(const char *arg) {
    if (!arg) {
        puts("ping6: missing address (e.g. ping6 fe80::2)");
        return;
    }
    uint8_t addr[16];
    if (parse_ipv6(arg, addr) != 0) {
        printf("ping6: bad IPv6 address '%s'\n", arg);
        return;
    }
    printf("ping6 %s...\n", arg);
    if (net_ping6(addr) == 0) {
        printf("Reply received from %s!\n", arg);
    } else {
        printf("No reply from %s\n", arg);
    }
}

static void cmd_ps(void) {
    printf("  PID  NAME\n");
    int found = 0;
    for (int pid = 1; pid < 64; pid++) {
        char path[64];
        char name[64];
        /* Simple manual itoa for path */
        int p = pid, len = 0;
        char tmp[16];
        while (p > 0) { tmp[len++] = '0' + (p % 10); p /= 10; }
        strcpy(path, "/proc/");
        int off = 6;
        while (len > 0) path[off++] = tmp[--len];
        path[off] = '\0';
        strcat(path, "/cmdline");

        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            int64_t n = read(fd, name, sizeof(name) - 1);
            if (n > 0) {
                name[n] = '\0';
                /* chomp newline */
                if (n > 0 && name[n-1] == '\n') name[n-1] = '\0';
                printf("  %d    %s\n", pid, name);
                found++;
            }
            close(fd);
        }
    }
    if (!found) {
        printf("  1    init (shell)\n");
    }
}

static void cmd_mkdir(const char *path) {
    if (!path) { puts("mkdir: missing path"); fflush(stdout); return; }
    if (mkdir(path, 0755) == 0) printf("mkdir: created %s\n", path);
    else                        printf("mkdir: failed %s\n", path);
    fflush(stdout);
}

static void cmd_rmdir(const char *path) {
    if (!path) { puts("rmdir: missing path"); fflush(stdout); return; }
    if (rmdir(path) == 0) printf("rmdir: removed %s\n", path);
    else                  printf("rmdir: failed %s (must be empty)\n", path);
    fflush(stdout);
}

static void cmd_rm(const char *path) {
    if (!path) { puts("rm: missing path"); fflush(stdout); return; }
    if (unlink(path) == 0) printf("rm: removed %s\n", path);
    else                   printf("rm: failed %s\n", path);
    fflush(stdout);
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { puts("usage: mv <from> <to>"); fflush(stdout); return; }
    if (rename(argv[1], argv[2]) == 0) printf("mv: %s -> %s\n", argv[1], argv[2]);
    else                                printf("mv: failed\n");
    fflush(stdout);
}

static void cmd_stat(const char *path) {
    if (!path) { puts("stat: missing path"); fflush(stdout); return; }
    struct stat st;
    if (stat(path, &st) != 0) { printf("stat: %s: not found\n", path); fflush(stdout); return; }
    const char *type =
        st.st_type == ST_TYPE_DIR  ? "directory" :
        st.st_type == ST_TYPE_FILE ? "regular file" : "other";
    printf("Path:    %s\n", path);
    printf("Type:    %s\n", type);
    printf("Size:    %llu bytes\n", (unsigned long long)st.st_size);
    printf("Inode:   %llu\n",       (unsigned long long)st.st_inode);
    printf("Mode:    0%o\n",        (unsigned)st.st_mode);
    printf("Links:   %u\n",         (unsigned)st.st_nlink);
    printf("Blocks:  %u\n",         (unsigned)st.st_blocks);
    printf("MTime:   %llu\n",       (unsigned long long)st.st_mtime);
    printf("CTime:   %llu\n",       (unsigned long long)st.st_ctime);
    printf("ATime:   %llu\n",       (unsigned long long)st.st_atime);
    fflush(stdout);
}

static void cmd_touch(const char *path) {
    if (!path) { puts("touch: missing path"); return; }
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { printf("touch: cannot create %s\n", path); return; }
    close(fd);
    printf("touch: %s\n", path);
}

static void cmd_apm(int argc, char **argv) {
    /* Arguments are forwarded as arguments (SDK_PLAN phase S3).
     *
     * This used to write the command line to /tmp/apm.args for apm to read
     * and delete, because spawn() could not carry argv.  Two shells running
     * apm at once would have raced on that file. */
    char apm_path[128];
    if (!prog_resolve("apm", apm_path, (int)sizeof(apm_path))) {
        puts("apm: not found");
        return;
    }

    char *av[MAX_ARGS + 1];
    int n = 0;
    av[n++] = (char *)"apm";
    for (int i = 1; i < argc && n < MAX_ARGS; i++) av[n++] = argv[i];
    av[n] = 0;

    printf("[shell] starting apm...\n");
    fflush(stdout);
    pid_t pid = spawnv(apm_path, av);
    if (pid < 0) {
        printf("apm: failed to launch %s\n", apm_path);
        fflush(stdout);
        return;
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

/* ---- Shell main loop ---- */

/* FIX_R8: keyboard layout query/switch.  SYS_KBD_LAYOUT keeps the same
 * number/kind of ABI as the other non-standard syscalls in
 * kernel/arch/x86_64/syscall.c (SYS_MEMINFO sits at 600) and returns the
 * kernel value verbatim: 0 success, -ENOENT for an unknown layout name. */
#define SYS_KBD_LAYOUT   601
#define KBD_OP_GET       0
#define KBD_OP_SET       1
#define KBD_OP_ENUM      2
#define KBD_NAME_MAX     32

static void cmd_kbd(const char *arg) {
    if (!arg) {
        char cur[KBD_NAME_MAX];
        if (syscall(SYS_KBD_LAYOUT, KBD_OP_GET, (uint64_t)cur,
                    (uint64_t)sizeof(cur), 0, 0, 0) == 0) {
            printf("kbd: current layout '%s'\n", cur);
        }
        printf("kbd: available:");
        for (uint64_t i = 0; ; i++) {
            char nm[KBD_NAME_MAX];
            int64_t r = syscall(SYS_KBD_LAYOUT, KBD_OP_ENUM, i,
                                (uint64_t)nm, 0, 0, 0);
            if (r < 0) break;
            printf(" %s", nm);
        }
        putchar('\n');
        return;
    }
    int64_t r = syscall(SYS_KBD_LAYOUT, KBD_OP_SET, (uint64_t)arg, 0, 0, 0, 0);
    if (r == 0) {
        printf("kbd: layout set to '%s'\n", arg);
    } else {
        printf("kbd: unknown layout '%s' (see `kbd` for the list)\n", arg);
    }
}

static int process_command(char *line);

/* SELFHOST SH6a: `sh <file> [args...]` runs a file of shell commands in this
 * same shell.
 *
 *   - positional parameters follow the usual convention, so $0 is the script
 *     name and $1 the first argument (`sh build.sh kernel` sees $1 == kernel);
 *   - a failing line stops the script and is reported with its line number,
 *     because a build log that says "failed" without saying where is useless;
 *   - `exit N` inside the script stops the script, not the machine.
 *
 * Returns the status of the failing line, or 0 if every line succeeded. */
static int cmd_sh(int argc, char **argv) {
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        puts("sh: missing script name");
        return 2;
    }
    if (sh_depth >= SH_MAX_DEPTH) {
        printf("sh: nesting too deep (max %d)\n", SH_MAX_DEPTH);
        return 2;
    }

    size_t text_len = 0;
    char *text = read_whole_file(argv[1], &text_len);
    if (!text) {
        printf("sh: %s: cannot read script\n", argv[1]);
        return 127;
    }

    struct sh_frame *f = &sh_stack[sh_depth];
    f->line      = 0;
    f->text      = text;
    f->text_len  = text_len;
    f->cursor    = 0;
    f->exit_req  = 0;
    f->exit_code = 0;

    /* Copy the positional parameters, argv[1..] -> $0..: argv[1] is the
     * script name, so $0 is the script and $1 the first argument. */
    f->argc = 0;
    for (int i = 1; i < argc && f->argc < MAX_ARGS - 1; i++) {
        char *dst = sh_argbuf[sh_depth][f->argc];
        strncpy(dst, argv[i], SH_ARG_MAX - 1);
        dst[SH_ARG_MAX - 1] = '\0';
        f->argv[f->argc++] = dst;
    }
    f->argv[f->argc] = 0;

    /* Diagnostics name the script, so the path must outlive the caller's
     * line buffer.  $0 already holds a private copy of it -- point there
     * rather than aliasing argv[1], which lives in the parent frame's
     * expansion buffer when this `sh` was itself called from a script. */
    f->path = f->argv[0];
    sh_depth++;

    char *line;
    int status = 0;

    /* SH6b moved expansion into process_command, where it can run per token.
     * Expanding the whole line first would let a variable's value inject an
     * argument or a redirect operator; per-token expansion means a value can
     * only ever add text inside the argument it landed in. */
    {
        struct sh_src src;
        memset(&src, 0, sizeof src);
        src.f = f;
        while (sh_src_next(&src, &line)) {
            if (sh_line_is_blank(line)) continue;
            status = sh_exec_line(&src, line);
            if (f->exit_req) { status = f->exit_code; break; }
            if (status != 0) {
                printf("sh: %s:%d: command failed with status %d\n",
                       f->path, f->line, status);
                break;
            }
        }
    }

    sh_depth--;
    free(text);
    last_status = status;
    return status;
}

/* ---- SELFHOST SH6c: pipes and command lists ----
 *
 * A line is now a LIST of pipelines; a pipeline is stages joined by `|`.
 * Each list element is introduced by nothing (the first), `;`, `&`, `&&`
 * or `||` -- the last four decide whether the next element runs.
 *
 * How a pipeline executes.  The plan says "using SYS_PIPE/SYS_PIPE2 and a
 * process group per pipeline; foreground pipelines wait for the last stage;
 * the job table already tracks the rest."  Stages run SEQUENTIALLY in this
 * process, each one's stdout wired to the next one's stdin through a real
 * kernel pipe (SYS_PIPE).  That is a measured choice, not a shortcut:
 *
 *   - Per-stage fork() of a pipeline child was tried first.  The child
 *     resumed at the syscall stub and immediately took a user-mode page
 *     fault (error 0x6, write to a non-present page) -- every stage, every
 *     time.  The existing `&` path forks a subshell BEFORE dispatch, which
 *     is a different shape; cloning the shell from the middle of a
 *     multi-stage setup is the shape that faulted.  Ledger SH-40.
 *   - Sequential stages are correct for every pipeline a build script runs
 *     (`echo ... | cat > log`, a compiler's short stdout).  The pipe buffer
 *     is 4 KiB; a producer that writes more than that without a concurrent
 *     consumer would block.  SH6f's build.sh does not do that.
 *   - A backgrounded pipeline (`a | b &`) still uses the existing one-fork
 *     subshell path, so the job table tracks it as one job -- one process
 *     group per pipeline, as the plan asked.
 *
 * Pipeline status is the LAST stage's status, as in POSIX.  `set -o pipefail`
 * is not in the language; a build script that needs the first stage's failure
 * puts that stage last, or runs it as its own list element (which is how the
 * gate proves `&&` propagation).
 */
#define SH_MAX_STAGES 8
static struct sh_redir sh_stage_redir[SH_MAX_REDIR];

/* The builtin/spawn dispatch, with no redirect or background handling.
 * cmd_argv[0..argc) is the expanded command.  Sets and returns last_status. */
static int sh_run_command(int argc)
{
    const char *cmd = cmd_argv[0];
    last_status = 0;

    if (strcmp(cmd, "true") == 0) {
        last_status = 0;
        return 0;
    } else if (strcmp(cmd, "false") == 0) {
        last_status = 1;
        return 1;
    } else if (strcmp(cmd, "break") == 0) {
        if (sh_loop_depth <= 0) {
            puts("sh: break: only meaningful in a loop");
            last_status = 0;
            return 0;
        }
        sh_break_req = 1;
        last_status = 0;
        return 0;
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(argc > 1 ? cmd_argv[1] : "/");
    } else if (strcmp(cmd, "cat") == 0) {
        cmd_cat(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(argc, cmd_argv);
    } else if (strcmp(cmd, "write") == 0) {
        cmd_write_file(argc, cmd_argv);
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname();
    } else if (strcmp(cmd, "free") == 0) {
        cmd_free();
    } else if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "nslookup") == 0) {
        cmd_nslookup(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "dnscache") == 0) {
        cmd_dnscache();
    } else if (strcmp(cmd, "dnsset") == 0) {
        cmd_dnsset(argc, cmd_argv);
    } else if (strcmp(cmd, "dnsflush") == 0) {
        cmd_dnsflush();
    } else if (strcmp(cmd, "dnstc") == 0) {
        if (dnsctl(DNSCTL_FORCE_TC, 0, 0) == 0) {
            puts("dnstc: next DNS answer will be truncated (one shot)");
        } else {
            puts("dnstc: failed");
            last_status = 1;
        }
    } else if (strcmp(cmd, "ping") == 0) {
        cmd_ping(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "ping6") == 0) {
        cmd_ping6(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "run") == 0) {
        if (argc > 1) {
            last_status = cmd_run_argv(cmd_argv[1], &cmd_argv[1]);
        } else {
            last_status = cmd_run(0);
        }
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(cmd, "mkdir") == 0) {
        cmd_mkdir(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "rmdir") == 0) {
        cmd_rmdir(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "rm") == 0) {
        cmd_rm(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "mv") == 0) {
        cmd_mv(argc, cmd_argv);
    } else if (strcmp(cmd, "stat") == 0) {
        cmd_stat(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "touch") == 0) {
        cmd_touch(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "apm") == 0) {
        cmd_apm(argc, cmd_argv);
    } else if (strcmp(cmd, "gui") == 0) {
        {
            char launcher[128];
            if (prog_resolve("glaunch", launcher, (int)sizeof(launcher))) {
                spawn(launcher);
            } else {
                puts("[gui] launcher not found");
            }
        }
        puts("[gui] launcher spawned");
    } else if (strcmp(cmd, "exit") == 0) {
        if (sh_depth > 0) {
            int code = (argc > 1) ? atoi(cmd_argv[1]) : 0;
            struct sh_frame *f = &sh_stack[sh_depth - 1];
            f->exit_req  = 1;
            f->exit_code = code;
            last_status  = code;
        } else {
            puts("Goodbye!");
            _exit(0);
        }
    } else if (strcmp(cmd, "sh") == 0) {
        last_status = cmd_sh(argc, cmd_argv);
    } else if (strcmp(cmd, "set") == 0) {
        last_status = cmd_set(argc, cmd_argv);
    } else if (strcmp(cmd, "unset") == 0) {
        last_status = cmd_unset(argc, cmd_argv);
    } else if (strcmp(cmd, "jobs") == 0) {
        cmd_jobs();
    } else if (strcmp(cmd, "fg") == 0) {
        cmd_fg(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "bg") == 0) {
        cmd_bg(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "kbd") == 0) {
        cmd_kbd(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "sleep") == 0) {
        cmd_sleep(argc > 1 ? cmd_argv[1] : "0");
    } else if (cmd[0] == '/' || cmd[0] == '.') {
        last_status = cmd_run(cmd);
    } else {
        char resolved[128];
        if (prog_resolve(cmd, resolved, (int)sizeof(resolved))) {
            last_status = cmd_run_argv(cmd, cmd_argv);
        } else {
            printf("%s: command not found\n", cmd);
            last_status = 127;
        }
    }
    return last_status;
}

/* Expand one stage's tokens into the shared statics.  Returns 0, or 2 with
 * the diagnostic printed (and last_status set) when the stage is malformed. */
static int sh_expand_stage(const struct sh_tok *toks, int start, int end,
                           const struct sh_var *vars, int nvars,
                           char *const *pos, int npos, int prev_status,
                           int *out_argc, int *out_nredir)
{
    int argc = 0, nredir = 0;
    for (int i = start; i < end; i++) {
        if (toks[i].type == SH_TOK_WORD) {
            if (argc >= MAX_ARGS - 1) {
                printf("sh: too many arguments (max %d)\n", MAX_ARGS - 1);
                last_status = 2;
                return 2;
            }
            if (sh_expand_word(toks[i].text, toks[i].len, sh_expbuf[argc],
                               SH_ARG_MAX_EXP, vars, nvars, pos, npos,
                               prev_status) != SH_EXP_OK) {
                printf("sh: argument %d too long to expand (max %d)\n",
                       argc, SH_ARG_MAX_EXP - 1);
                last_status = 2;
                return 2;
            }
            cmd_argv[argc] = sh_expbuf[argc];
            argc++;
            continue;
        }
        if (toks[i].type == SH_TOK_GT || toks[i].type == SH_TOK_GGT ||
            toks[i].type == SH_TOK_LT) {
            if (i + 1 >= end || toks[i + 1].type != SH_TOK_WORD) {
                printf("sh: missing filename after %s\n",
                       sh_tok_name(toks[i].type));
                last_status = 2;
                return 2;
            }
            if (nredir >= SH_MAX_REDIR) {
                printf("sh: too many redirections (max %d)\n", SH_MAX_REDIR);
                last_status = 2;
                return 2;
            }
            if (sh_expand_word(toks[i + 1].text, toks[i + 1].len,
                               sh_redirbuf[nredir], SH_ARG_MAX_EXP,
                               vars, nvars, pos, npos,
                               prev_status) != SH_EXP_OK) {
                printf("sh: redirect target too long (max %d)\n",
                       SH_ARG_MAX_EXP - 1);
                last_status = 2;
                return 2;
            }
            sh_stage_redir[nredir].op   = toks[i].type;
            sh_stage_redir[nredir].path = sh_redirbuf[nredir];
            nredir++;
            i++;
            continue;
        }
        printf("sh: unexpected operator %s\n", sh_tok_name(toks[i].type));
        last_status = 2;
        return 2;
    }
    cmd_argv[argc] = 0;
    *out_argc = argc;
    *out_nredir = nredir;
    return 0;
}

/* Single-command path (SH6a/SH6b behaviour, unchanged): assignment, redir,
 * background, dispatch. */
static int sh_run_simple(int argc, int nredir, int bg)
{
    if (argc == 0) return 0;

    const char *cmd = cmd_argv[0];
    last_status = 0;

    if (strchr(cmd, '=')) {
        if (argc > 1) {
            puts("sh: per-command assignment is not supported; "
                 "use `set NAME=VALUE` on its own line");
            last_status = 2;
            return 2;
        }
        if (sh_try_assign(cmd)) return 0;
        printf("sh: %s: not a valid variable name\n", cmd);
        last_status = 2;
        return 2;
    }

    int saved_in = -1, saved_out = -1;
    if (sh_redir_apply(sh_stage_redir, nredir, &saved_in, &saved_out) < 0) {
        last_status = 1;
        return 1;
    }

    if (bg) {
        char resolved[128];
        if (strcmp(cmd, "run") == 0 && argc > 1) {
            if (!prog_resolve(cmd_argv[1], resolved, (int)sizeof(resolved))) {
                report_not_found(cmd_argv[1]);
                last_status = 127;
                goto n1out;
            }
            pid_t pid = spawn(resolved);
            if (pid > 0) { setpgid(pid, pid); add_job(pid, cmd_argv[1]); }
            last_status = 0;
            goto n1out;
        }
        if (cmd[0] == '/' || cmd[0] == '.') {
            pid_t pid = spawn(cmd);
            if (pid > 0) { setpgid(pid, pid); add_job(pid, cmd); }
            last_status = 0;
            goto n1out;
        }
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, 0);
            in_subshell = 1;
            last_status = sh_run_command(argc);
            fflush(stdout);
            _exit(last_status);
        } else if (pid > 0) {
            setpgid(pid, pid);
            add_job(pid, cmd);
            last_status = 0;
            goto n1out;
        }
    }

    last_status = sh_run_command(argc);
n1out:
    if (in_subshell) {
        fflush(stdout);
        _exit(last_status);
    }
    sh_redir_restore(saved_in, saved_out);
    return last_status;
}

static void sh_close_pipes(int pipes[][2], int n)
{
    for (int i = 0; i < n; i++) {
        if (pipes[i][0] >= 0) { close(pipes[i][0]); pipes[i][0] = -1; }
        if (pipes[i][1] >= 0) { close(pipes[i][1]); pipes[i][1] = -1; }
    }
}

/* Run one pipeline (token range [start, end)), possibly backgrounded. */
static int sh_run_pipeline(const struct sh_tok *toks, int start, int end,
                           int bg, const struct sh_var *vars, int nvars,
                           char *const *pos, int npos, int prev_status)
{
    int stage_start[SH_MAX_STAGES];
    int stage_end[SH_MAX_STAGES];
    int nstage = 0;
    int cur = start;

    for (int i = start; i <= end; i++) {
        if (i == end || (i < end && toks[i].type == SH_TOK_PIPE)) {
            if (nstage >= SH_MAX_STAGES) {
                printf("sh: pipeline too long (max %d stages)\n", SH_MAX_STAGES);
                last_status = 2;
                return 2;
            }
            stage_start[nstage] = cur;
            stage_end[nstage] = i;
            nstage++;
            cur = i + 1;
        }
    }

    if (nstage == 1) {
        int argc = 0, nredir = 0;
        if (sh_expand_stage(toks, start, end, vars, nvars, pos, npos,
                            prev_status, &argc, &nredir) < 0)
            return 2;
        return sh_run_simple(argc, nredir, bg);
    }

    for (int s = 0; s < nstage; s++) {
        int words = 0;
        for (int i = stage_start[s]; i < stage_end[s]; i++)
            if (toks[i].type == SH_TOK_WORD) words++;
        if (words == 0) {
            puts("sh: empty pipeline stage");
            last_status = 2;
            return 2;
        }
    }

    /* A backgrounded pipeline is one job: fork a subshell, run the pipeline
     * in the child without `&`, and let the job table track the child. */
    if (bg) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, 0);
            in_subshell = 1;
            last_status = sh_run_pipeline(toks, start, end, 0, vars, nvars,
                                          pos, npos, prev_status);
            fflush(stdout);
            _exit(last_status);
        } else if (pid > 0) {
            setpgid(pid, pid);
            add_job(pid, cmd_argv[0] ? cmd_argv[0] : "pipeline");
            last_status = 0;
            return 0;
        }
        /* fork failed: fall through and run in the foreground. */
    }

    int pipes[SH_MAX_STAGES - 1][2];
    int npipes = nstage - 1;
    for (int s = 0; s < npipes; s++) {
        pipes[s][0] = pipes[s][1] = -1;
        if (pipe(pipes[s]) != 0) {
            sh_close_pipes(pipes, s);
            puts("sh: pipeline: cannot create pipe");
            last_status = 1;
            return 1;
        }
    }

    int tty0 = dup(0);
    int tty1 = dup(1);
    if (tty0 < 0 || tty1 < 0) {
        sh_close_pipes(pipes, npipes);
        if (tty0 >= 0) close(tty0);
        if (tty1 >= 0) close(tty1);
        puts("sh: pipeline: cannot save stdio");
        last_status = 1;
        return 1;
    }

    int status = 0;
    for (int s = 0; s < nstage; s++) {
        int argc = 0, nredir = 0;
        if (sh_expand_stage(toks, stage_start[s], stage_end[s],
                            vars, nvars, pos, npos, prev_status,
                            &argc, &nredir) < 0) {
            status = 2;
            break;
        }
        if (argc > 0 && strchr(cmd_argv[0], '=')) {
            printf("sh: %s: an assignment cannot be a pipeline stage\n",
                   cmd_argv[0]);
            status = 2;
            break;
        }

        fflush(stdout);
        if (s > 0) dup2(pipes[s - 1][0], 0);
        else       dup2(tty0, 0);
        if (s < nstage - 1) dup2(pipes[s][1], 1);
        else                dup2(tty1, 1);

        int saved_in = -1, saved_out = -1;
        if (sh_redir_apply(sh_stage_redir, nredir, &saved_in, &saved_out) < 0) {
            status = 1;
        } else {
            status = sh_run_command(argc);
            sh_redir_restore(saved_in, saved_out);
        }

        /* Back to the console between stages so a diagnostic does not fall
         * into the next pipe, and so the next stage starts from a known fd
         * table. */
        dup2(tty0, 0);
        dup2(tty1, 1);

        /* Close this stage's write end so the next reader sees EOF after
         * draining the buffer; close the previous read end we just consumed. */
        if (s < nstage - 1) { close(pipes[s][1]); pipes[s][1] = -1; }
        if (s > 0)          { close(pipes[s - 1][0]); pipes[s - 1][0] = -1; }
    }

    sh_close_pipes(pipes, npipes);
    dup2(tty0, 0);
    dup2(tty1, 1);
    close(tty0);
    close(tty1);
    last_status = status;
    return status;
}

/* SELFHOST SH6a: returns the command's exit status, also stored in
 * last_status so $? can read it after the fact.
 * SELFHOST SH6c: the line is now a list of pipelines. */
static int process_command(char *line) {
    /* Defensive sanitising: VM serial ports can occasionally feed garbage when
     * no terminal is attached. Treat non-printable/non-ASCII bytes as spaces so
     * they never become bogus commands like "        : command not found". */
    for (char *p = line; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') *p = '\n';
        else if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) *p = ' ';
    }

    /* SELFHOST SH6b: tokenize with quotes and operators recognised in one
     * pass; SH6c adds the list operators to that same pass.  The old
     * strtok(line, " \t\n") could express none of it, so a `>` inside a
     * quoted argument was an operator and there was no way to write a
     * redirect -- or a pipe, or a command list -- at all. */
    int prev_status = last_status;

    struct sh_tok toks[SH_MAX_TOKS];
    int ntok = sh_tokenize(line, strlen(line), toks, SH_MAX_TOKS);
    if (ntok < 0) {
        switch (ntok) {
        case SH_PARSE_QUOTE:     puts("sh: unmatched quote"); break;
        case SH_PARSE_NOTARGET:  puts("sh: redirect with no filename"); break;
        case SH_PARSE_NOCOMMAND: puts("sh: expected a command"); break;
        default:                 puts("sh: command line too complex"); break;
        }
        last_status = 2;
        return 2;
    }
    if (ntok == 0) return 0;

    char *const *pos = 0;
    int npos = 0;
    if (sh_depth > 0) {
        pos  = sh_stack[sh_depth - 1].argv;
        npos = sh_stack[sh_depth - 1].argc;
    }
    const struct sh_var *vars = sh_var_table();

    /* SELFHOST SH6c: the line is a list of pipelines.  `;` and `&` run the
     * next element whatever happened before (a `&` also backgrounds its
     * element); `&&` runs it only when the previous status is 0, `||` only
     * when it is nonzero.  $? for the next element on the line is the status
     * of the last element that RAN -- a skipped element changes nothing. */
    int tok = 0;
    int line_status = 0;
    int have_status = 0;
    int pending_op = 0;
    while (tok <= ntok) {
        int el_start = tok;
        int el_end = tok;
        while (el_end < ntok &&
               toks[el_end].type != SH_TOK_SEMI &&
               toks[el_end].type != SH_TOK_AMP &&
               toks[el_end].type != SH_TOK_ANDAND &&
               toks[el_end].type != SH_TOK_OROR) {
            el_end++;
        }
        int bg = (el_end < ntok && toks[el_end].type == SH_TOK_AMP);
        int op = (el_end < ntok) ? toks[el_end].type : 0;

        /* An element with no tokens at all: the line started with an
         * operator (`&& echo`), or two separators sat back to back (`a;; b`).
         * POSIX calls both errors; so do we.  (A trailing `;` is different:
         * it ends the line, there is no element after it.) */
        if (el_end == el_start && op != 0) {
            printf("sh: expected a command after %s\n", sh_tok_name(op));
            last_status = 2;
            return 2;
        }

        int skip = 0;
        if (pending_op == SH_TOK_ANDAND && have_status && line_status != 0)
            skip = 1;
        if (pending_op == SH_TOK_OROR   && have_status && line_status == 0)
            skip = 1;

        if (!skip && el_end > el_start) {
            int st = sh_run_pipeline(toks, el_start, el_end, bg, vars,
                                     sh_nvars, pos, npos, prev_status);
            line_status = st;
            have_status = 1;
            prev_status = st;
            if (sh_break_req) break;
            if (sh_depth > 0 && sh_stack[sh_depth - 1].exit_req) break;
        }

        if (el_end >= ntok) break;
        pending_op = bg ? SH_TOK_SEMI : op;
        tok = el_end + 1;
    }

    last_status = have_status ? line_status : 0;
    return last_status;
}

/* ---- SELFHOST SH6d: if / while / for / break ----
 *
 * The subset build.sh needs, branching on the SH6a status spine.
 * `case`, functions, `trap` and arithmetic are out of scope.
 *
 * A compound is one command as far as cmd_sh is concerned: a failing
 * *condition* is consumed by the construct (so `if false; then` does not
 * abort the script), and the construct's status is the last command of
 * the taken branch, or 0 if no branch ran.  Body lines do not go through
 * cmd_sh's "stop on nonzero" loop -- that applies to the if/while/for as
 * a unit, which is POSIX without `set -e` plus SH6a's top-level stop.
 *
 * `true`/`false` are tiny status builtins so a condition does not have to
 * be `echo` (prints) or `run nosuch` (127 and a diagnostic).  `break` is
 * only meaningful inside a loop.
 *
 * Bodies of a collected compound are run through a sh_src that names
 * those lines, not the script frame.  A nested `if` that called
 * sh_next_line on the frame would consume the outer `done`.
 */

static int sh_toks_span(const struct sh_tok *toks, int start, int end,
                        char *dst, size_t cap)
{
    if (end <= start) { dst[0] = '\0'; return 0; }
    const char *a = toks[start].text;
    const char *b = toks[end - 1].text + toks[end - 1].len;
    size_t n = (size_t)(b - a);
    if (n >= cap) return -1;
    memcpy(dst, a, n);
    dst[n] = '\0';
    return 0;
}

static int sh_find_kw(const struct sh_tok *toks, int start, int ntok, int want)
{
    int depth = 0;
    for (int i = start; i < ntok; i++) {
        int k = sh_word_kw(&toks[i]);
        if (depth == 0 && k == want) return i;
        if (sh_kw_open(k) && i != start) depth++;
        if (sh_kw_close(k) && depth > 0) depth--;
    }
    return -1;
}

static int sh_line_delta(const struct sh_tok *toks, int ntok)
{
    int d = 0;
    for (int i = 0; i < ntok; i++) {
        int k = sh_word_kw(&toks[i]);
        if (sh_kw_open(k))  d++;
        if (sh_kw_close(k)) d--;
    }
    return d;
}

static int sh_wanted(int k, const int *want, int nwant)
{
    for (int i = 0; i < nwant; i++) if (k == want[i]) return 1;
    return 0;
}

static int sh_trim_semi(const struct sh_tok *toks, int end)
{
    if (end > 0 && toks[end - 1].type == SH_TOK_SEMI) return end - 1;
    return end;
}

static int sh_skip_semi_tok(const struct sh_tok *toks, int i, int ntok)
{
    if (i < ntok && toks[i].type == SH_TOK_SEMI) return i + 1;
    return i;
}

/* Collect body lines until a closer at depth 0.  The closer line is
 * pushed back, not included.  Returns 0, 2 on error, -1 on EOF. */
static int sh_collect_until(struct sh_src *src, const int *want, int nwant,
                            char **body, int max, int *nout)
{
    *nout = 0;
    if (!src) return -1;
    char *line;
    int depth = 0;
    while (sh_src_next(src, &line)) {
        if (sh_line_is_blank(line)) continue;
        struct sh_tok t[SH_MAX_TOKS];
        int n = sh_tokenize(line, strlen(line), t, SH_MAX_TOKS);
        if (n < 0) {
            puts("sh: syntax error in compound command");
            return 2;
        }
        int first = (n > 0) ? sh_word_kw(&t[0]) : SH_KW_NONE;
        if (depth == 0 && sh_wanted(first, want, nwant)) {
            sh_src_unread(src, line);
            return 0;
        }
        depth += sh_line_delta(t, n);
        if (depth < 0) depth = 0;
        if (*nout >= max) {
            puts("sh: compound command too large");
            return 2;
        }
        body[(*nout)++] = line;
    }
    return -1;
}

static int sh_run_lines(char **lines, int n)
{
    struct sh_src inner;
    memset(&inner, 0, sizeof inner);
    inner.lines  = lines;
    inner.nlines = n;
    int st = 0;
    char *line;
    while (sh_src_next(&inner, &line)) {
        if (sh_line_is_blank(line)) continue;
        st = sh_exec_line(&inner, line);
        if (sh_stopped()) break;
    }
    return st;
}

/* Prepend an optional first body fragment (the rest of a `then`/`do`
 * line) onto collected subsequent lines and run them as one source. */
static int sh_run_body(char *first, char **lines, int n)
{
    char *all[SH_MAX_BODY + 1];
    int nall = 0;
    if (first && first[0]) {
        if (nall >= SH_MAX_BODY + 1) {
            puts("sh: compound command too large");
            return 2;
        }
        all[nall++] = first;
    }
    for (int i = 0; i < n; i++) {
        if (nall >= SH_MAX_BODY + 1) {
            puts("sh: compound command too large");
            return 2;
        }
        all[nall++] = lines[i];
    }
    if (nall == 0) return 0;
    return sh_run_lines(all, nall);
}

static int sh_skip_until(struct sh_src *src, int closer)
{
    int want = closer;
    char *discard[SH_MAX_BODY];
    int n = 0;
    int r = sh_collect_until(src, &want, 1, discard, SH_MAX_BODY, &n);
    if (r == -1) {
        printf("sh: unexpected EOF looking for '%s'\n",
               closer == SH_KW_FI ? "fi" : "done");
        return 2;
    }
    if (r) return r;
    char *line;
    if (!sh_src_next(src, &line)) {
        printf("sh: unexpected EOF looking for '%s'\n",
               closer == SH_KW_FI ? "fi" : "done");
        return 2;
    }
    return 0;
}

/* Remainder after a keyword on a taken line.  Static until the next call;
 * callers who need it later copy. */
static char sh_restbuf[INPUT_MAX];

static int sh_take_kw_line(struct sh_src *src, int want, const char *name,
                           char **rest)
{
    *rest = 0;
    if (!src) {
        printf("sh: missing '%s'\n", name);
        return 2;
    }
    char *line;
    while (sh_src_next(src, &line)) {
        if (sh_line_is_blank(line)) continue;
        struct sh_tok t[SH_MAX_TOKS];
        int n = sh_tokenize(line, strlen(line), t, SH_MAX_TOKS);
        if (n < 0) return 2;
        if (n > 0 && sh_word_kw(&t[0]) == want) {
            int start = sh_skip_semi_tok(t, 1, n);
            if (start < n) {
                if (sh_toks_span(t, start, n, sh_restbuf, sizeof sh_restbuf) < 0) {
                    puts("sh: command too long");
                    return 2;
                }
            } else {
                sh_restbuf[0] = '\0';
            }
            *rest = sh_restbuf;
            return 0;
        }
        sh_src_unread(src, line);
        printf("sh: expected '%s'\n", name);
        return 2;
    }
    printf("sh: unexpected EOF looking for '%s'\n", name);
    return 2;
}

static int sh_eval_cond(const char *cond)
{
    if (!cond || !cond[0]) {
        puts("sh: empty condition");
        return 2;
    }
    return process_command((char *)cond);
}

/* After a one-line `fi`/`done`, optionally run the rest of the line
 * (`if c; then b; fi; echo next`).  The separator decides whether it
 * runs, matching SH6c's list rules. */
static int sh_after_compound(struct sh_src *src, const struct sh_tok *toks,
                             int closer, int ntok, int st)
{
    int i = closer + 1;
    if (i >= ntok) return st;
    int op = toks[i].type;
    int run = 0;
    if (op == SH_TOK_SEMI || op == SH_TOK_AMP) { run = 1; i++; }
    else if (op == SH_TOK_ANDAND) { run = (st == 0); i++; }
    else if (op == SH_TOK_OROR)   { run = (st != 0); i++; }
    else {
        puts("sh: expected a separator after compound command");
        return 2;
    }
    if (i >= ntok) return st;
    if (!run) return st;
    char rest[INPUT_MAX];
    if (sh_toks_span(toks, i, ntok, rest, sizeof rest) < 0) return 2;
    return sh_exec_line(src, rest);
}

static int sh_rewrite_if(const struct sh_tok *toks, int from, int ntok,
                         char *dst, size_t cap)
{
    if (cap < 4) return -1;
    dst[0] = 'i'; dst[1] = 'f'; dst[2] = ' ';
    if (from >= ntok) { dst[2] = '\0'; return 0; }
    const char *a = toks[from].text;
    const char *b = toks[ntok - 1].text + toks[ntok - 1].len;
    size_t n = (size_t)(b - a);
    if (n + 4 > cap) return -1;
    memcpy(dst + 3, a, n);
    dst[3 + n] = '\0';
    return 0;
}

static int sh_do_if(struct sh_src *src, struct sh_tok *toks, int ntok)
{
    char cond[INPUT_MAX];
    int then_at = sh_find_kw(toks, 1, ntok, SH_KW_THEN);
    int cond_end = (then_at >= 0) ? then_at : ntok;
    cond_end = sh_trim_semi(toks, cond_end);
    if (cond_end <= 1) { puts("sh: if: empty condition"); return 2; }
    if (sh_toks_span(toks, 1, cond_end, cond, sizeof cond) < 0) {
        puts("sh: if: condition too long");
        return 2;
    }

    char first_body[INPUT_MAX];
    first_body[0] = '\0';
    int have_first = 0;

    if (then_at >= 0) {
        int rest = sh_skip_semi_tok(toks, then_at + 1, ntok);
        int oneline_fi = sh_find_kw(toks, rest, ntok, SH_KW_FI);
        if (oneline_fi >= 0) {
            int elif_at = sh_find_kw(toks, rest, oneline_fi, SH_KW_ELIF);
            int else_at = sh_find_kw(toks, rest, oneline_fi, SH_KW_ELSE);
            int taken = (sh_eval_cond(cond) == 0);
            if (taken) {
                int body_end = oneline_fi;
                if (else_at >= 0 && else_at < body_end) body_end = else_at;
                if (elif_at >= 0 && elif_at < body_end) body_end = elif_at;
                body_end = sh_trim_semi(toks, body_end);
                int st = 0;
                if (body_end > rest) {
                    if (sh_toks_span(toks, rest, body_end, first_body,
                                     sizeof first_body) < 0) return 2;
                    st = process_command(first_body);
                }
                return sh_after_compound(src, toks, oneline_fi, ntok, st);
            }
            if (elif_at >= 0) {
                char rec[INPUT_MAX];
                if (sh_rewrite_if(toks, elif_at + 1, ntok, rec, sizeof rec) < 0)
                    return 2;
                return sh_exec_line(src, rec);
            }
            if (else_at >= 0) {
                int body_start = sh_skip_semi_tok(toks, else_at + 1, ntok);
                int body_end = sh_trim_semi(toks, oneline_fi);
                int st = 0;
                if (body_end > body_start) {
                    if (sh_toks_span(toks, body_start, body_end, first_body,
                                     sizeof first_body) < 0) return 2;
                    st = process_command(first_body);
                }
                return sh_after_compound(src, toks, oneline_fi, ntok, st);
            }
            return sh_after_compound(src, toks, oneline_fi, ntok, 0);
        }
        if (rest < ntok) {
            if (sh_toks_span(toks, rest, ntok, first_body, sizeof first_body) < 0)
                return 2;
            have_first = first_body[0] != '\0';
        }
    } else {
        char *rest = 0;
        int r = sh_take_kw_line(src, SH_KW_THEN, "then", &rest);
        if (r) return r;
        if (rest && rest[0]) {
            strncpy(first_body, rest, sizeof first_body - 1);
            first_body[sizeof first_body - 1] = '\0';
            have_first = 1;
        }
    }

    char *body[SH_MAX_BODY];
    int nbody = 0;
    static const int then_closers[] = { SH_KW_ELIF, SH_KW_ELSE, SH_KW_FI };
    int r = sh_collect_until(src, then_closers, 3, body, SH_MAX_BODY, &nbody);
    if (r == -1) { puts("sh: unexpected EOF looking for 'fi'"); return 2; }
    if (r) return r;

    int taken = (sh_eval_cond(cond) == 0);
    int result = 0;
    if (taken) {
        result = sh_run_body(have_first ? first_body : 0, body, nbody);
        return sh_skip_until(src, SH_KW_FI) ? 2 : result;
    }

    char *cline;
    if (!sh_src_next(src, &cline)) {
        puts("sh: unexpected EOF looking for 'fi'");
        return 2;
    }
    struct sh_tok ct[SH_MAX_TOKS];
    int cn = sh_tokenize(cline, strlen(cline), ct, SH_MAX_TOKS);
    int ckw = (cn > 0) ? sh_word_kw(&ct[0]) : SH_KW_NONE;
    if (ckw == SH_KW_FI) return 0;
    if (ckw == SH_KW_ELSE) {
        char else_first[INPUT_MAX];
        else_first[0] = '\0';
        int have_ef = 0;
        int s = sh_skip_semi_tok(ct, 1, cn);
        if (s < cn) {
            if (sh_toks_span(ct, s, cn, else_first, sizeof else_first) < 0)
                return 2;
            have_ef = else_first[0] != '\0';
        }
        char *ebody[SH_MAX_BODY];
        int ne = 0;
        static const int fi_only[] = { SH_KW_FI };
        r = sh_collect_until(src, fi_only, 1, ebody, SH_MAX_BODY, &ne);
        if (r == -1) { puts("sh: unexpected EOF looking for 'fi'"); return 2; }
        if (r) return r;
        result = sh_run_body(have_ef ? else_first : 0, ebody, ne);
        char *fi_line;
        if (!sh_src_next(src, &fi_line)) {
            puts("sh: unexpected EOF looking for 'fi'");
            return 2;
        }
        return result;
    }
    if (ckw == SH_KW_ELIF) {
        char rec[INPUT_MAX];
        if (sh_rewrite_if(ct, 1, cn, rec, sizeof rec) < 0) return 2;
        return sh_exec_line(src, rec);
    }
    puts("sh: expected 'fi', 'else' or 'elif'");
    return 2;
}

static int sh_do_while(struct sh_src *src, struct sh_tok *toks, int ntok)
{
    char cond[INPUT_MAX];
    int do_at = sh_find_kw(toks, 1, ntok, SH_KW_DO);
    int cond_end = (do_at >= 0) ? do_at : ntok;
    cond_end = sh_trim_semi(toks, cond_end);
    if (cond_end <= 1) { puts("sh: while: empty condition"); return 2; }
    if (sh_toks_span(toks, 1, cond_end, cond, sizeof cond) < 0) {
        puts("sh: while: condition too long");
        return 2;
    }

    char first_body[INPUT_MAX];
    first_body[0] = '\0';
    int have_first = 0;

    if (do_at >= 0) {
        int rest = sh_skip_semi_tok(toks, do_at + 1, ntok);
        int oneline_done = sh_find_kw(toks, rest, ntok, SH_KW_DONE);
        if (oneline_done >= 0) {
            int body_end = sh_trim_semi(toks, oneline_done);
            char body[INPUT_MAX];
            body[0] = '\0';
            if (body_end > rest) {
                if (sh_toks_span(toks, rest, body_end, body, sizeof body) < 0)
                    return 2;
            }
            sh_loop_depth++;
            int result = 0;
            int n = 0;
            struct sh_src empty;
            memset(&empty, 0, sizeof empty);
            for (;;) {
                if (++n > SH_MAX_LOOP) {
                    puts("sh: loop limit exceeded");
                    sh_loop_depth--;
                    last_status = 2;
                    return 2;
                }
                if (sh_eval_cond(cond) != 0) { result = 0; break; }
                if (body[0]) result = sh_exec_line(&empty, body);
                if (sh_break_req) { sh_break_req = 0; result = 0; break; }
                if (sh_depth > 0 && sh_stack[sh_depth - 1].exit_req) break;
            }
            sh_loop_depth--;
            return sh_after_compound(src, toks, oneline_done, ntok, result);
        }
        if (rest < ntok) {
            if (sh_toks_span(toks, rest, ntok, first_body, sizeof first_body) < 0)
                return 2;
            have_first = first_body[0] != '\0';
        }
    } else {
        char *rest = 0;
        int r = sh_take_kw_line(src, SH_KW_DO, "do", &rest);
        if (r) return r;
        if (rest && rest[0]) {
            strncpy(first_body, rest, sizeof first_body - 1);
            first_body[sizeof first_body - 1] = '\0';
            have_first = 1;
        }
    }

    char *body[SH_MAX_BODY];
    int nbody = 0;
    static const int done_only[] = { SH_KW_DONE };
    int r = sh_collect_until(src, done_only, 1, body, SH_MAX_BODY, &nbody);
    if (r == -1) { puts("sh: unexpected EOF looking for 'done'"); return 2; }
    if (r) return r;

    sh_loop_depth++;
    int result = 0;
    int n = 0;
    for (;;) {
        if (++n > SH_MAX_LOOP) {
            puts("sh: loop limit exceeded");
            sh_loop_depth--;
            last_status = 2;
            return 2;
        }
        if (sh_eval_cond(cond) != 0) { result = 0; break; }
        result = sh_run_body(have_first ? first_body : 0, body, nbody);
        if (sh_break_req) { sh_break_req = 0; result = 0; break; }
        if (sh_depth > 0 && sh_stack[sh_depth - 1].exit_req) break;
    }
    sh_loop_depth--;
    char *done_line;
    if (!sh_src_next(src, &done_line)) {
        puts("sh: unexpected EOF looking for 'done'");
        return 2;
    }
    return result;
}

static int sh_do_for(struct sh_src *src, struct sh_tok *toks, int ntok)
{
    if (ntok < 4 || toks[1].type != SH_TOK_WORD) {
        puts("sh: for: expected name");
        return 2;
    }
    if (sh_word_kw(&toks[2]) != SH_KW_IN) {
        puts("sh: for: expected 'in'");
        return 2;
    }

    char name[SH_VAR_NAME_MAX];
    if (toks[1].len >= sizeof name) { puts("sh: for: name too long"); return 2; }
    memcpy(name, toks[1].text, toks[1].len);
    name[toks[1].len] = '\0';
    if (!sh_is_name_start(name[0])) {
        printf("sh: for: '%s' is not a valid name\n", name);
        return 2;
    }

    int do_at = sh_find_kw(toks, 3, ntok, SH_KW_DO);
    int list_end = (do_at >= 0) ? do_at : ntok;
    list_end = sh_trim_semi(toks, list_end);

    char vals[MAX_ARGS][SH_ARG_MAX_EXP];
    int nvals = 0;
    char *const *pos = 0;
    int npos = 0;
    if (sh_depth > 0) {
        pos  = sh_stack[sh_depth - 1].argv;
        npos = sh_stack[sh_depth - 1].argc;
    }
    const struct sh_var *vars = sh_var_table();
    for (int i = 3; i < list_end; i++) {
        if (toks[i].type != SH_TOK_WORD) {
            printf("sh: for: unexpected %s in word list\n",
                   sh_tok_name(toks[i].type));
            return 2;
        }
        if (nvals >= MAX_ARGS) { puts("sh: for: too many words"); return 2; }
        if (sh_expand_word(toks[i].text, toks[i].len, vals[nvals],
                           SH_ARG_MAX_EXP, vars, sh_nvars, pos, npos,
                           last_status) != SH_EXP_OK) {
            puts("sh: for: word too long to expand");
            return 2;
        }
        nvals++;
    }

    char first_body[INPUT_MAX];
    first_body[0] = '\0';
    int have_first = 0;

    if (do_at >= 0) {
        int rest = sh_skip_semi_tok(toks, do_at + 1, ntok);
        int done_at = sh_find_kw(toks, rest, ntok, SH_KW_DONE);
        if (done_at >= 0) {
            int body_end = sh_trim_semi(toks, done_at);
            char body[INPUT_MAX];
            body[0] = '\0';
            if (body_end > rest) {
                if (sh_toks_span(toks, rest, body_end, body, sizeof body) < 0)
                    return 2;
            }
            sh_loop_depth++;
            int result = 0;
            struct sh_src empty;
            memset(&empty, 0, sizeof empty);
            for (int i = 0; i < nvals; i++) {
                if (sh_var_set(name, vals[i]) != 0) {
                    printf("sh: for: cannot assign %s\n", name);
                    sh_loop_depth--;
                    return 2;
                }
                if (body[0]) result = sh_exec_line(&empty, body);
                if (sh_break_req) { sh_break_req = 0; result = 0; break; }
                if (sh_depth > 0 && sh_stack[sh_depth - 1].exit_req) break;
            }
            sh_loop_depth--;
            return sh_after_compound(src, toks, done_at, ntok, result);
        }
        if (rest < ntok) {
            if (sh_toks_span(toks, rest, ntok, first_body, sizeof first_body) < 0)
                return 2;
            have_first = first_body[0] != '\0';
        }
    } else {
        char *rest = 0;
        int r = sh_take_kw_line(src, SH_KW_DO, "do", &rest);
        if (r) return r;
        if (rest && rest[0]) {
            strncpy(first_body, rest, sizeof first_body - 1);
            first_body[sizeof first_body - 1] = '\0';
            have_first = 1;
        }
    }

    char *body[SH_MAX_BODY];
    int nbody = 0;
    static const int done_only[] = { SH_KW_DONE };
    int r = sh_collect_until(src, done_only, 1, body, SH_MAX_BODY, &nbody);
    if (r == -1) { puts("sh: unexpected EOF looking for 'done'"); return 2; }
    if (r) return r;

    sh_loop_depth++;
    int result = 0;
    for (int i = 0; i < nvals; i++) {
        if (sh_var_set(name, vals[i]) != 0) {
            printf("sh: for: cannot assign %s\n", name);
            sh_loop_depth--;
            return 2;
        }
        result = sh_run_body(have_first ? first_body : 0, body, nbody);
        if (sh_break_req) { sh_break_req = 0; result = 0; break; }
        if (sh_depth > 0 && sh_stack[sh_depth - 1].exit_req) break;
    }
    sh_loop_depth--;
    char *done_line;
    if (!sh_src_next(src, &done_line)) {
        puts("sh: unexpected EOF looking for 'done'");
        return 2;
    }
    return result;
}

/* Dispatch one line: a compound opener, or a SH6c list. */
static int sh_exec_line(struct sh_src *src, char *line)
{
    for (char *p = line; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') *p = '\n';
        else if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) *p = ' ';
    }
    struct sh_tok toks[SH_MAX_TOKS];
    int ntok = sh_tokenize(line, strlen(line), toks, SH_MAX_TOKS);
    if (ntok < 0) return process_command(line);
    if (ntok == 0) return 0;
    int kw = sh_word_kw(&toks[0]);
    int st;
    if (kw == SH_KW_IF) {
        st = sh_do_if(src, toks, ntok);
    } else if (kw == SH_KW_WHILE) {
        st = sh_do_while(src, toks, ntok);
    } else if (kw == SH_KW_FOR) {
        st = sh_do_for(src, toks, ntok);
    } else if (kw == SH_KW_THEN || kw == SH_KW_ELIF || kw == SH_KW_ELSE ||
               kw == SH_KW_FI || kw == SH_KW_DO || kw == SH_KW_DONE) {
        printf("sh: unexpected '%.*s'\n", (int)toks[0].len, toks[0].text);
        st = 2;
    } else {
        st = process_command(line);
    }
    last_status = st;
    return st;
}

static void dummy_handler(int s) { (void)s; }

int main(void) {
    signal(SIGALRM, dummy_handler);

    /* An interactive shell must not be stoppable from its own keyboard:
     * between jobs the terminal's foreground group IS the shell, so a bare
     * ^Z would suspend the shell itself with nobody left to resume it.  The
     * kernel hands the default dispositions back at every exec, so spawned
     * children stay suspendible. */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);


    int fd = open("/dev/tty0", O_RDWR);
    if (fd >= 0) {
        if (fd != 0) { dup2(fd, 0); close(fd); }
        dup2(0, 1);
        dup2(0, 2);
    }

    printf("\n");
    printf("==============================================\n");
    printf("   AuraLite OS v0.0.1 — Interactive Shell     \n");
    printf("   Type 'help' for available commands         \n");
    printf("==============================================\n");
    printf("\n");

    for (;;) {
        int st = 0;
        pid_t reaped;
        while ((reaped = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0) {
            remove_job(reaped, st);
        }

        /* Print the prompt. */
        write(1, "auralite#\n", 10);

        /* Read a line from stdin (serial input). */
        int64_t n = read(0, input_line, INPUT_MAX - 1);
        if (n <= 0) {
            continue;
        }
        input_line[n] = '\0';

        /* Process the command.  Compounds (if/while/for) start a line;
         * a one-liner at the prompt has no further source. */
        {
            struct sh_src prompt;
            memset(&prompt, 0, sizeof prompt);
            (void)sh_exec_line(&prompt, input_line);
        }
    }

    return 0;
}
