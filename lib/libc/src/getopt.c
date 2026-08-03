/* libc/src/getopt.c — getopt / getopt_long (P10) */

#include <getopt.h>
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
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    /* Long option: argv[optind] starts with "--". */
    if (optind < argc && argv[optind][0] == '-' && argv[optind][1] == '-'
        && argv[optind][2] != '\0') {
        const char *arg = argv[optind] + 2;
        size_t namelen = strlen(arg);
        const char *eq = strchr(arg, '=');
        if (eq) namelen = (size_t)(eq - arg);

        for (int i = 0; longopts && longopts[i].name; i++) {
            if (strncmp(arg, longopts[i].name, namelen) == 0
                && longopts[i].name[namelen] == '\0') {
                if (longindex) *longindex = i;
                optind++;
                if (longopts[i].has_arg == required_argument) {
                    if (eq) optarg = (char *)eq + 1;
                    else if (optind < argc) optarg = argv[optind++];
                    else return '?';
                } else if (longopts[i].has_arg == optional_argument) {
                    optarg = eq ? (char *)eq + 1 : NULL;
                } else {
                    optarg = NULL;
                }
                if (longopts[i].flag) { *longopts[i].flag = longopts[i].val; return 0; }
                return longopts[i].val;
            }
        }
        if (opterr) fprintf(stderr, "%s: unrecognized option '--%s'\n", argv[0], arg);
        optind++;
        return '?';
    }

    /* Fall back to short-option parsing. */
    return getopt(argc, argv, optstring);
}
