/*
 * hello.c — Phase 9 gate test: a compiled user program.
 *
 * Calls write(1, "hello\n", 6) via the libc syscall wrapper, then exits.
 * This proves the full chain works: libc → SYSCALL → kernel dispatch.
 */

#include "unistd.h"

int main(void) {
    write(1, "hello\n", 6);
    return 0;
}
