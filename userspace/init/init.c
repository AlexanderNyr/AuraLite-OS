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
 *   help       — list commands
 *   exit       — halt
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

#define INPUT_MAX 256
#define MAX_ARGS  8

/* Line buffer and token storage (in BSS, zero-filled by the ELF loader). */
static char input_line[INPUT_MAX];
static char *cmd_argv[MAX_ARGS];

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
    int fd = open(path);
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
        return;
    }
    int fd = open(argv[1]);
    if (fd < 0) {
        printf("write: cannot open/create %s\n", argv[1]);
        return;
    }
    for (int i = 2; i < argc; i++) {
        if (i > 2) write(fd, " ", 1);
        write(fd, argv[i], strlen(argv[i]));
    }
    write(fd, "\n", 1);
    close(fd);
    printf("wrote %s\n", argv[1]);
}

static void cmd_pwd(void) {
    puts("/");
}

static void cmd_uname(void) {
    puts("AuraLite OS 1.0.0 x86_64");
}

static void cmd_free(void) {
    /* Memory stats would come from a syscall; for now print a stub. */
    puts("              total        used        free");
    puts("Mem:          510MiB      ~32MiB      478MiB");
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
    puts("  ping <host> - ping a hostname via ICMP");
    puts("  ps          - list processes (stub)");
    puts("  mkdir <dir> - create a directory  (FAT32 / ext2)");
    puts("  rmdir <dir> - remove an empty directory");
    puts("  rm <file>   - delete a file");
    puts("  mv <a> <b>  - rename a file or directory");
    puts("  touch <file>- create an empty file");
    puts("  stat <path> - show file metadata");
    puts("  help        - show this help");
    puts("  exit        - exit shell");
    puts("");
    puts("Applications:");
    puts("  run /calc     - interactive calculator");
    puts("  run /sysinfo  - system information");
    puts("  run /editor   - text editor");
    puts("  run /clock    - clock display");
    puts("  run /guess    - number guessing game");
    puts("  run /snake    - snake game");
    puts("  run /hello    - hello world");
    puts("  run /http     - HTTP client");
    puts("  run /browser  - web browser (fetch + render HTML)");
}

static void cmd_run(const char *prog) {
    if (!prog) {
        puts("run: missing program name");
        return;
    }
    printf("running %s in isolated address space...\n", prog);
    pid_t pid = spawn(prog);
    if (pid < 0) {
        printf("run: failed to spawn %s\n", prog);
        return;
    }
    printf("[shell] child PID %lld, waiting...\n", (long long)pid);
    wait(NULL);
    printf("[shell] child exited\n");
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

static void cmd_ps(void) {
    printf("  PID  NAME\n");
    printf("  1    init (shell)\n");
}

static void cmd_mkdir(const char *path) {
    if (!path) { puts("mkdir: missing path"); return; }
    if (mkdir(path) == 0) printf("mkdir: created %s\n", path);
    else                  printf("mkdir: failed %s\n", path);
}

static void cmd_rmdir(const char *path) {
    if (!path) { puts("rmdir: missing path"); return; }
    if (rmdir(path) == 0) printf("rmdir: removed %s\n", path);
    else                  printf("rmdir: failed %s (must be empty)\n", path);
}

static void cmd_rm(const char *path) {
    if (!path) { puts("rm: missing path"); return; }
    if (unlink(path) == 0) printf("rm: removed %s\n", path);
    else                   printf("rm: failed %s\n", path);
}

static void cmd_mv(int argc, char **argv) {
    if (argc < 3) { puts("usage: mv <from> <to>"); return; }
    if (rename(argv[1], argv[2]) == 0) printf("mv: %s -> %s\n", argv[1], argv[2]);
    else                                printf("mv: failed\n");
}

static void cmd_stat(const char *path) {
    if (!path) { puts("stat: missing path"); return; }
    struct stat st;
    if (stat(path, &st) != 0) { printf("stat: %s: not found\n", path); return; }
    const char *type =
        st.st_type == ST_TYPE_DIR  ? "directory" :
        st.st_type == ST_TYPE_FILE ? "regular file" : "other";
    printf("  Path:    %s\n", path);
    printf("  Type:    %s\n", type);
    printf("  Size:    %llu bytes\n", (unsigned long long)st.st_size);
    printf("  Inode:   %llu\n",       (unsigned long long)st.st_inode);
    printf("  Mode:    0%o\n",        (unsigned)st.st_mode);
    printf("  Links:   %u\n",         (unsigned)st.st_nlink);
    printf("  Blocks:  %u\n",         (unsigned)st.st_blocks);
}

static void cmd_touch(const char *path) {
    if (!path) { puts("touch: missing path"); return; }
    int fd = open(path);
    if (fd < 0) { printf("touch: cannot create %s\n", path); return; }
    close(fd);
    printf("touch: %s\n", path);
}

/* ---- Shell main loop ---- */

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

    const char *cmd = cmd_argv[0];

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
    } else if (strcmp(cmd, "ping") == 0) {
        cmd_ping(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "run") == 0) {
        cmd_run(argc > 1 ? cmd_argv[1] : 0);
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
    } else if (strcmp(cmd, "gui") == 0) {
        spawn("/glaunch");
        puts("[gui] launcher spawned");
    } else if (strcmp(cmd, "exit") == 0) {
        puts("Goodbye!");
        _exit(0);
    } else {
        printf("%s: command not found\n", cmd);
    }
}

int main(void) {
    printf("\n");
    printf("==============================================\n");
    printf("   AuraLite OS v1.0.0 — Interactive Shell     \n");
    printf("   Type 'help' for available commands         \n");
    printf("==============================================\n");
    printf("\n");

    for (;;) {
        /* Print the prompt. */
        write(1, "auralite# ", 9);

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
