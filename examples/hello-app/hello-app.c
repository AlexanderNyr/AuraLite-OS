/*
 * hello-app.c — the smallest useful AuraLite OS application.
 *
 * This is a worked example, built by `make sdk-check` against the STAGED SDK
 * rather than against the OS source tree. If the SDK stops being sufficient
 * to build this, CI fails — which is the only reason the SDK can be trusted.
 */

#include "stdio.h"
#include "string.h"
#include "unistd.h"

int main(int argc, char **argv) {
    printf("HELLOAPP: hello from a third-party application\n");

    /* argv works when the program is started by execve, and — since
     * SDK_PLAN phase S3 — by `run hello-app a b c` too. */
    printf("HELLOAPP: argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("HELLOAPP: argv[%d]=%s\n", i, argv[i]);
    }

    /* Ordinary libc works: this is the same libc the shipped programs use. */
    char buf[64];
    snprintf(buf, sizeof(buf), "%d + %d = %d", 2, 3, 2 + 3);
    printf("HELLOAPP: %s\n", buf);

    /* Output is buffered. A program that exits without flushing can lose its
     * last line, which is a confusing way to discover buffering exists. */
    fflush(stdout);
    return 0;
}
