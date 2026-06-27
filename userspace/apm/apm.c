/* apm — AuraLite Package Manager. */
#include "unistd.h"
#include "fcntl.h"
#include "string.h"
#include "stdio.h"

#define MAX_PKG 3

struct package {
    const char *name;
    const char *version;
    const char *desc;
    const char *pkg_path;
    const char *install_path;
};

static struct package repo[MAX_PKG] = {
    { "matrix", "1.0", "Matrix digital rain screen simulation", "/matrix.pkg", "/tmp/matrix" },
    { "life",   "1.2", "Conway's Game of Life simulation",      "/life.pkg",   "/tmp/life"   },
    { "fetch",  "2.1", "System information fetch utility",      "/fetch.pkg",  "/tmp/fetch"  },
};

static int is_installed(const struct package *p) {
    struct stat st;
    return (stat(p->install_path, &st) == 0);
}

static void apm_update(void) {
    puts("[apm] Fetching repository index from AuraLite upstream...");
    puts("[apm] Read 3 package manifests.");
    puts("[apm] Repository index successfully updated.");
}

static void apm_list(void) {
    puts("Available packages in AuraLite repository:");
    printf("  %-10s %-8s %-12s %s\n", "NAME", "VERSION", "STATUS", "DESCRIPTION");
    printf("  --------------------------------------------------------------\n");
    for (int i = 0; i < MAX_PKG; i++) {
        const char *status = is_installed(&repo[i]) ? "[installed]" : "[available]";
        printf("  %-10s %-8s %-12s %s\n", repo[i].name, repo[i].version, status, repo[i].desc);
    }
}

static void apm_info(const char *name) {
    if (!name) { puts("apm info: missing package name"); return; }
    for (int i = 0; i < MAX_PKG; i++) {
        if (strcmp(repo[i].name, name) == 0) {
            printf("Package:      %s\n", repo[i].name);
            printf("Version:      %s\n", repo[i].version);
            printf("Description:  %s\n", repo[i].desc);
            printf("Source URL:   aurapkg://pkg/%s.pkg\n", repo[i].name);
            printf("Install Path: %s\n", repo[i].install_path);
            printf("Status:       %s\n", is_installed(&repo[i]) ? "Installed" : "Not installed");
            return;
        }
    }
    printf("apm: package '%s' not found in repository\n", name);
}

static void apm_install(const char *name) {
    if (!name) { puts("apm install: missing package name"); return; }
    for (int i = 0; i < MAX_PKG; i++) {
        if (strcmp(repo[i].name, name) == 0) {
            if (is_installed(&repo[i])) {
                printf("[apm] Package '%s' is already installed.\n", name);
                return;
            }
            printf("[apm] Installing %s (%s)...\n", repo[i].name, repo[i].version);
            int fd_src = open(repo[i].pkg_path, O_RDONLY);
            if (fd_src < 0) {
                printf("[apm] Error: source archive '%s' not found.\n", repo[i].pkg_path);
                return;
            }
            int fd_dst = open(repo[i].install_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd_dst < 0) {
                printf("[apm] Error: cannot create target '%s'.\n", repo[i].install_path);
                close(fd_src);
                return;
            }
            char buf[512];
            int64_t n, total = 0;
            while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
                write(fd_dst, buf, (size_t)n);
                total += n;
            }
            close(fd_src);
            close(fd_dst);
            printf("[apm] Unpacked %lld bytes to %s.\n", (long long)total, repo[i].install_path);
            printf("[apm] Successfully installed %s! Run with 'run %s'\n", repo[i].name, repo[i].install_path);
            return;
        }
    }
    printf("apm: package '%s' not found in repository\n", name);
}

static void apm_remove(const char *name) {
    if (!name) { puts("apm remove: missing package name"); return; }
    for (int i = 0; i < MAX_PKG; i++) {
        if (strcmp(repo[i].name, name) == 0) {
            if (!is_installed(&repo[i])) {
                printf("[apm] Package '%s' is not installed.\n", name);
                return;
            }
            if (unlink(repo[i].install_path) == 0) {
                printf("[apm] Removed %s successfully.\n", repo[i].install_path);
            } else {
                printf("[apm] Error removing %s.\n", repo[i].install_path);
            }
            return;
        }
    }
    printf("apm: package '%s' not found in repository\n", name);
}

static void process_cmd(char *line) {
    char *argv[8];
    int argc = 0;
    char *tok = strtok(line, " \t\n");
    while (tok && argc < 7) {
        argv[argc++] = tok;
        tok = strtok(0, " \t\n");
    }
    argv[argc] = 0;
    if (argc == 0) return;

    const char *cmd = argv[0];
    if (strcmp(cmd, "update") == 0)      apm_update();
    else if (strcmp(cmd, "list") == 0)   apm_list();
    else if (strcmp(cmd, "info") == 0)   apm_info(argc > 1 ? argv[1] : 0);
    else if (strcmp(cmd, "install") == 0)apm_install(argc > 1 ? argv[1] : 0);
    else if (strcmp(cmd, "remove") == 0) apm_remove(argc > 1 ? argv[1] : 0);
    else if (strcmp(cmd, "help") == 0) {
        puts("apm commands:");
        puts("  update          - update package repository index");
        puts("  list            - list available and installed packages");
        puts("  info <pkg>      - show details about a package");
        puts("  install <pkg>   - install a package");
        puts("  remove <pkg>    - remove an installed package");
        puts("  exit            - exit apm");
    }
    else if (strcmp(cmd, "exit") == 0)   _exit(0);
    else printf("apm: unknown command '%s'. Type 'help' for usage.\n", cmd);
}

int main(void) {
    /* Check if we were passed arguments via /tmp/apm.args */
    int fd_args = open("/tmp/apm.args", O_RDONLY);
    if (fd_args >= 0) {
        char argbuf[256];
        int64_t n = read(fd_args, argbuf, sizeof(argbuf) - 1);
        close(fd_args);
        unlink("/tmp/apm.args");
        if (n > 0) {
            argbuf[n] = '\0';
            process_cmd(argbuf);
            return 0;
        }
    }

    /* Interactive mode */
    puts("\n==============================================");
    puts("   AuraLite Package Manager (apm) v1.0        ");
    puts("   Type 'help' for available commands         ");
    puts("==============================================\n");

    char line[256];
    for (;;) {
        write(1, "apm> ", 5);
        int64_t n = read(0, line, sizeof(line) - 1);
        if (n <= 0) continue;
        line[n] = '\0';
        process_cmd(line);
    }
    return 0;
}
