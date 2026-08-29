/* tools/selfhost/sh6e_stamp.c -- SH6e recipe stand-in.
 *
 * AuraLite has no /bin/sh, so a Makefile recipe cannot be `echo x > t`.
 * This tiny program is the "compiler": it writes `target` and prints a
 * greppable rebuild line tagged with argv[1], so the host can tell a
 * first run (GEN=1) from an incremental run (GEN=2).
 *
 *   sh6e_stamp <tag> <target> [prereqs...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int make_abs(const char *in, char *out, int outsz) {
    char cwd[512];
    int n;
    size_t cl;
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

int main(int argc, char **argv) {
    FILE *fp;
    char abs[512];
    if (argc < 3) {
        fprintf(stderr, "usage: sh6e_stamp <tag> <target> [prereqs...]\n");
        return 2;
    }
    if (make_abs(argv[2], abs, (int)sizeof abs) != 0) {
        perror("getcwd");
        return 1;
    }
    fp = fopen(abs, "w");
    if (!fp) {
        perror(abs);
        return 1;
    }
    fprintf(fp, "stamp %s\n", argv[2]);
    fclose(fp);
    printf("[selfhost] sh6e: g%s rebuilt %s\n", argv[1], argv[2]);
    fflush(stdout);
    return 0;
}
