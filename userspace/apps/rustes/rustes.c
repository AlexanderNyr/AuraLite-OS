#include "unistd.h"

extern void rust_main(int, char **, char **);

int main(int argc, char **argv, char **envp) {
    rust_main(argc, argv, envp);
    return 0;
}