/* apm — AuraLite Package Manager. */
#include "unistd.h"
#include "fcntl.h"
#include "string.h"
#include "stdio.h"

#include "apkg.h"
#include "sys/stat.h"

/* The repository is the FILESYSTEM (SDK_PLAN phase S5).
 *
 * It used to be a compile-time array of three entries.  apm installed
 * correctly, into the right place, with the kernel enforcing the
 * destination -- and it could only ever offer those three packages.  A
 * third-party application could not be added without rebuilding the OS,
 * which defeats the point of having a package manager at all.
 *
 * Now apm scans /pkg for *.apkg and reads each one's header, so dropping a
 * package in makes it installable.  `apm install /path/to/x.apkg` takes one
 * from anywhere, which is what makes a package obtained from outside the
 * system usable.
 *
 * No network and no signatures.  A CRC-32 detects corruption, not tampering;
 * real signing needs a key story this OS does not have, and pretending
 * otherwise would be worse than saying so.
 */
#define PKG_DIR      "/pkg"
#define INSTALL_DIR  "/opt"
#define MAX_PKG      32

struct package {
    char name[APKG_NAME_MAX];
    char version[APKG_VERSION_MAX];
    char desc[APKG_DESC_MAX];
    char pkg_path[160];
    char install_path[160];
    uint64_t size;
};

static struct package repo[MAX_PKG];
static int repo_count;

static void join_path(char *out, int cap, const char *dir, const char *file) {
    int p = 0;
    for (const char *c = dir; *c && p < cap - 1; c++) out[p++] = *c;
    if (p > 0 && out[p - 1] != '/' && p < cap - 1) out[p++] = '/';
    for (const char *c = file; *c && p < cap - 1; c++) out[p++] = *c;
    out[p] = '\0';
}

static int ends_with_apkg(const char *name) {
    int n = (int)strlen(name);
    return n > 5 && strcmp(name + n - 5, ".apkg") == 0;
}

/* Read a package's header.  Only the header is read: the payload can be a
 * megabyte and listing the repository must not cost that per entry. */
static int read_header(const char *path, struct apkg_header *h, int quiet) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[APKG_HEADER_MAX];
    int64_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return 0;

    int r = apkg_parse(buf, (size_t)n, h);
    /* A short read is expected here: apkg_parse() checks the declared size
     * against what it was given, and we deliberately gave it only the header.
     * That specific complaint is therefore not an error at this stage. */
    if (r == APKG_ERR_SIZE) r = APKG_OK;
    if (r != APKG_OK) {
        if (!quiet) printf("[apm] %s: %s\n", path, apkg_strerror(r));
        return 0;
    }
    return 1;
}

/* Scan /pkg.  Returns the number of packages found. */
static int repo_scan(void) {
    repo_count = 0;

    static struct aura_dirent ents[64];
    int n = aura_readdir(PKG_DIR, ents, 64);
    if (n <= 0) return 0;

    for (int i = 0; i < n && repo_count < MAX_PKG; i++) {
        if (!ends_with_apkg(ents[i].name)) continue;

        char path[160];
        join_path(path, sizeof(path), PKG_DIR, ents[i].name);

        struct apkg_header h;
        if (!read_header(path, &h, 0)) continue;   /* reason already printed */

        struct package *p = &repo[repo_count++];
        strncpy(p->name, h.name, sizeof(p->name) - 1);
        strncpy(p->version, h.version, sizeof(p->version) - 1);
        strncpy(p->desc, h.desc, sizeof(p->desc) - 1);
        strncpy(p->pkg_path, path, sizeof(p->pkg_path) - 1);
        join_path(p->install_path, sizeof(p->install_path), INSTALL_DIR, h.name);
        p->size = h.size;
    }
    return repo_count;
}

static int is_installed(const struct package *p) {
    struct stat st;
    return (stat(p->install_path, &st) == 0);
}

static void apm_update(void) {
    /* There is no network and no remote repository.  This used to print
     * "Fetching repository index from AuraLite upstream..." and a package
     * count, which was theatre: nothing was fetched.  Printing a lie about
     * network activity is worse than printing nothing, so it now says what it
     * actually does -- rescan the local directory. */
    int n = repo_scan();
    printf("[apm] Scanned %s: %d package%s.\n", PKG_DIR, n, n == 1 ? "" : "s");
    puts("[apm] (there is no remote repository; packages are local files)");
}

static void apm_list(void) {
    if (repo_count == 0) {
        printf("No packages in %s.\n", PKG_DIR);
        return;
    }
    puts("Available packages:");
    printf("  %-12s %-8s %-12s %s\n", "NAME", "VERSION", "STATUS", "DESCRIPTION");
    printf("  ----------------------------------------------------------------\n");
    for (int i = 0; i < repo_count; i++) {
        const char *status = is_installed(&repo[i]) ? "[installed]" : "[available]";
        printf("  %-12s %-8s %-12s %s\n",
               repo[i].name, repo[i].version, status, repo[i].desc);
    }
}

static struct package *find_pkg(const char *name) {
    for (int i = 0; i < repo_count; i++) {
        if (strcmp(repo[i].name, name) == 0) return &repo[i];
    }
    return 0;
}

static void apm_info(const char *name) {
    if (!name) { puts("apm info: missing package name"); return; }
    struct package *p = find_pkg(name);
    if (!p) { printf("apm: package '%s' not found in %s\n", name, PKG_DIR); return; }

    printf("Package:      %s\n", p->name);
    printf("Version:      %s\n", p->version);
    printf("Description:  %s\n", p->desc);
    printf("Source:       %s\n", p->pkg_path);
    printf("Payload:      %llu bytes\n", (unsigned long long)p->size);
    printf("Install Path: %s\n", p->install_path);
    printf("Status:       %s\n", is_installed(p) ? "Installed" : "Not installed");
}

/* Install from a package file.
 *
 * The whole file is read and verified BEFORE anything is written, so a
 * corrupt package leaves nothing behind.  Verifying while copying would
 * produce a half-written executable in /opt and a error message, which is a
 * worse outcome than refusing.
 */
static int install_file(const char *pkg_path, const char *want_name) {
    int fd = open(pkg_path, O_RDONLY);
    if (fd < 0) {
        printf("[apm] cannot open %s\n", pkg_path);
        return 0;
    }

    /* One static buffer: apm is single-threaded and this avoids a malloc
     * whose failure mode would be a partially-read package. */
    static char filebuf[APKG_HEADER_MAX + APKG_PAYLOAD_MAX];
    int64_t total = 0;
    for (;;) {
        int64_t n = read(fd, filebuf + total, (int64_t)sizeof(filebuf) - total);
        if (n <= 0) break;
        total += n;
        if (total >= (int64_t)sizeof(filebuf)) break;
    }
    close(fd);
    if (total <= 0) { printf("[apm] %s is empty\n", pkg_path); return 0; }

    struct apkg_header h;
    int r = apkg_parse(filebuf, (size_t)total, &h);
    if (r != APKG_OK) {
        printf("[apm] %s: %s\n", pkg_path, apkg_strerror(r));
        return 0;
    }
    r = apkg_verify(filebuf, (size_t)total, &h);
    if (r != APKG_OK) {
        printf("[apm] %s: %s -- refusing to install\n", pkg_path, apkg_strerror(r));
        return 0;
    }

    if (want_name && strcmp(want_name, h.name) != 0) {
        printf("[apm] %s contains '%s', not '%s'\n", pkg_path, h.name, want_name);
        return 0;
    }

    char dest[160];
    join_path(dest, sizeof(dest), INSTALL_DIR, h.name);

    printf("[apm] Installing %s (%s)...\n", h.name, h.version);

    /* 0755: an installed program is created executable.  The kernel permits
     * that only under /opt or /tmp (FSLAYOUT_PLAN F1), so a build of apm
     * pointing anywhere else fails here with EPERM rather than succeeding
     * quietly. */
    int out = open(dest, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (out < 0) {
        printf("[apm] cannot create %s\n", dest);
        puts("[apm] programs may only be installed under /opt");
        return 0;
    }
    int64_t written = write(out, filebuf + h.payload_off, (size_t)h.size);
    close(out);

    if (written != (int64_t)h.size) {
        printf("[apm] short write to %s (%lld of %llu bytes)\n",
               dest, (long long)written, (unsigned long long)h.size);
        unlink(dest);
        return 0;
    }

    printf("[apm] Unpacked %llu bytes to %s.\n",
           (unsigned long long)h.size, dest);
    printf("[apm] Successfully installed %s! Run with 'run %s'\n", h.name, h.name);
    return 1;
}

static void apm_install(const char *name) {
    if (!name) { puts("apm install: missing package name"); return; }

    /* A path installs that file directly -- this is what makes a package
     * obtained from outside the system usable at all. */
    if (strchr(name, '/')) {
        install_file(name, 0);
        return;
    }

    struct package *p = find_pkg(name);
    if (!p) { printf("apm: package '%s' not found in %s\n", name, PKG_DIR); return; }
    if (is_installed(p)) {
        printf("[apm] Package '%s' is already installed.\n", name);
        return;
    }
    install_file(p->pkg_path, p->name);
}

static void apm_remove(const char *name) {
    if (!name) { puts("apm remove: missing package name"); return; }
    struct package *p = find_pkg(name);
    if (!p) { printf("apm: package '%s' not found in %s\n", name, PKG_DIR); return; }
    if (!is_installed(p)) {
        printf("[apm] Package '%s' is not installed.\n", name);
        return;
    }
    if (unlink(p->install_path) == 0) printf("[apm] Removed %s successfully.\n", p->install_path);
    else                              printf("[apm] Error removing %s.\n", p->install_path);
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

int main(int argc, char **argv) {
    repo_scan();

    /* Arguments arrive as arguments (SDK_PLAN phase S3).
     *
     * They used to arrive through /tmp/apm.args: the shell wrote the command
     * line to a file and apm read and deleted it, because spawn() could not
     * forward argv.  That worked, and it was a workaround for a missing
     * syscall feature rather than a design -- it broke if two shells ran apm
     * at once, and it was not something to teach a third-party program. */
    if (argc > 1) {
        char line[256];
        int p = 0;
        for (int i = 1; i < argc && p < (int)sizeof(line) - 1; i++) {
            if (i > 1 && p < (int)sizeof(line) - 1) line[p++] = ' ';
            for (const char *c = argv[i]; *c && p < (int)sizeof(line) - 1; c++) {
                line[p++] = *c;
            }
        }
        line[p] = '\0';
        process_cmd(line);
        return 0;
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
