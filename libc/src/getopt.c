/* libc/src/getopt.c — getopt (P10) */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

int getopt(int argc, char * const argv[], const char *optstring) {
    static int optpos = 1;
    if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] == '\0')
        return -1;

    const char *p = strchr(optstring, argv[optind][optpos]);
    if (!p) {
        if (opterr) fprintf(stderr, "%s: illegal option -- %c\n", argv[0], argv[optind][optpos]);
        optopt = argv[optind][optpos];
        return '?';
    }

    if (p[1] == ':') {
        if (argv[optind][optpos + 1]) {
            optarg = argv[optind] + optpos + 1;
        } else if (++optind < argc) {
            optarg = argv[optind];
        } else {
            if (opterr) fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], *p);
            return '?';
        }
        optind++;
        optpos = 1;
    } else {
        if (argv[optind][++optpos] == '\0') {
            optind++;
            optpos = 1;
        }
    }
    return *p;
}