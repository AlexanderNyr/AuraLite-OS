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
    puts("  run <prog>  - run a program in its own address space");
    puts("  pwd         - print working directory");
    puts("  uname       - print OS information");
    puts("  free        - print memory usage");
    puts("  ps          - list processes (stub)");
    puts("  help        - show this help");
    puts("  exit        - exit shell");
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
    printf("[shell] child PID %d, waiting...\n", (int)pid);
    wait(NULL);
    printf("[shell] child exited\n");
}

static void cmd_ps(void) {
    printf("  PID  NAME\n");
    printf("  1    init (shell)\n");
}

/* ---- Shell main loop ---- */

static void process_command(char *line) {
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
    } else if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(cmd, "uname") == 0) {
        cmd_uname();
    } else if (strcmp(cmd, "free") == 0) {
        cmd_free();
    } else if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "run") == 0) {
        cmd_run(argc > 1 ? cmd_argv[1] : 0);
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
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
