#ifndef AURALITE_LIBC_GETOPT_H
#define AURALITE_LIBC_GETOPT_H

extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;

int getopt(int argc, char *const argv[], const char *optstring);

/* getopt_long support. */
struct option {
    const char *name;
    int         has_arg;   /* no_argument / required_argument / optional_argument */
    int        *flag;
    int         val;
};

#define no_argument        0
#define required_argument  1
#define optional_argument  2

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#endif /* AURALITE_LIBC_GETOPT_H */
