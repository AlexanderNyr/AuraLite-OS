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
#include "sys/wait.h"
#include "signal.h"

#define INPUT_MAX 256
#define MAX_ARGS  8

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

static void cmd_cat(const char *path) {
    if (!path) {
        puts("cat: missing file");
        return;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("cat: %s: no such file\n", path);
        return;
    }
    char buf[128];
    int64_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    close(fd);
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        write(1, argv[i], strlen(argv[i]));
    }
    putchar('\n');
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
static void cmd_run_argv(const char *prog, char *const argv[]);

/* No-argument form, for the call sites that have nothing to pass. */
static void cmd_run(const char *prog) { cmd_run_argv(prog, 0); }

static void cmd_run_argv(const char *prog, char *const argv[]) {
    if (!prog) {
        puts("run: missing program name");
        return;
    }
    char resolved[128];
    if (!prog_resolve(prog, resolved, (int)sizeof(resolved))) {
        report_not_found(prog);
        return;
    }
    prog = resolved;
    printf("running %s in isolated address space...\n", prog);
    fflush(stdout);
    pid_t pid = argv ? spawnv(prog, argv) : spawn(prog);
    if (pid < 0) {
        printf("run: failed to spawn %s\n", prog);
        fflush(stdout);
        return;
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

    if (got < 0) {
        printf("[shell] waitpid failed for %s\n", prog);
    } else if (WIFSTOPPED(status)) {
        add_job(pid, prog);
        job_list[next_job_id - 2].running = 0;
        printf("\n[%d] Stopped %s\n", job_list[next_job_id - 2].id, prog);
    } else {
        printf("[shell] child exited\n");
    }
    fflush(stdout);
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

static void process_command(char *line) {
    /* Defensive sanitising: VM serial ports can occasionally feed garbage when
     * no terminal is attached. Treat non-printable/non-ASCII bytes as spaces so
     * they never become bogus commands like "        : command not found". */
    for (char *p = line; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') *p = '\n';
        else if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) *p = ' ';
    }

    /* Tokenize the line into command + arguments. */
    int argc = 0;
    char *tok = strtok(line, " \t\n");
    while (tok && argc < MAX_ARGS - 1) {
        cmd_argv[argc++] = tok;
        tok = strtok(0, " \t\n");
    }
    cmd_argv[argc] = 0;

    if (argc == 0) {
        return;   /* empty line */
    }

    int bg = 0;
    size_t len = strlen(cmd_argv[argc - 1]);
    if (len > 0 && cmd_argv[argc - 1][len - 1] == '&') {
        bg = 1;
        if (len == 1) {
            cmd_argv[--argc] = 0;
        } else {
            cmd_argv[argc - 1][len - 1] = '\0';
        }
    }
    if (argc == 0) return;

    const char *cmd = cmd_argv[0];

    if (bg) {
        /* The background paths resolve through the same list as the
         * foreground ones.  Keeping one of them hardcoded is how `run calc`
         * and `run calc &` end up behaving differently. */
        char resolved[128];
        if (strcmp(cmd, "run") == 0 && argc > 1) {
            if (!prog_resolve(cmd_argv[1], resolved, (int)sizeof(resolved))) {
                report_not_found(cmd_argv[1]);
                return;
            }
            pid_t pid = spawn(resolved);
            if (pid > 0) { setpgid(pid, pid); add_job(pid, cmd_argv[1]); }
            return;
        }
        if (cmd[0] == '/' || cmd[0] == '.') {
            pid_t pid = spawn(cmd);
            if (pid > 0) { setpgid(pid, pid); add_job(pid, cmd); }
            return;
        }
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, 0);
            bg = 0;
            in_subshell = 1;
            goto do_dispatch;
        } else if (pid > 0) {
            setpgid(pid, pid);
            add_job(pid, cmd);
            return;
        }
    }

do_dispatch:
    if (strcmp(cmd, "ls") == 0) {
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
    } else if (strcmp(cmd, "ping") == 0) {
        cmd_ping(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "ping6") == 0) {
        cmd_ping6(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "run") == 0) {
        if (argc > 1) {
            /* Forward everything after the program name.  argv[0] is the
             * program, as the convention everywhere else expects. */
            cmd_run_argv(cmd_argv[1], &cmd_argv[1]);
        } else {
            cmd_run(0);
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
        puts("Goodbye!");
        _exit(0);
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
        cmd_run(cmd);
    } else {
        /* Not a built-in.  Before declaring it unknown, look for a program of
         * that name on the search path — this is what makes `calc` work as
         * well as `run calc`. */
        char resolved[128];
        if (prog_resolve(cmd, resolved, (int)sizeof(resolved))) {
            cmd_run_argv(cmd, cmd_argv);
        } else {
            printf("%s: command not found\n", cmd);
        }
    }
    if (in_subshell) _exit(0);
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

        /* Process the command. */
        process_command(input_line);
    }

    return 0;
}
