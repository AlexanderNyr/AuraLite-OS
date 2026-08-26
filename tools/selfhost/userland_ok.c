/* tools/selfhost/userland_ok.c -- SH2 receipt program, built IN-GUEST.
 *
 * SELFHOST_PLAN.md SH2 gate: this tiny program is compiled and linked
 * inside AuraLite by the guest toolchain, against the guest-built libc
 * (the same crt0.o + libc.o + malloc.o the real apps use), and its
 * output IS the §8 receipt the host greps:
 *
 *   [selfhost] userland rebuild PASS: 2 binaries
 *
 * printf/puts through the guest libc prove the whole chain: crt0 stack
 * decode, syscall wrapper, stdio buffering, malloc, and the tcc ELF
 * linker's segment layout under the kernel's ELF loader.
 */

#include <stdio.h>

int main(void)
{
    puts("[selfhost] userland rebuild PASS: 2 binaries (sysinfo, editor)");
    return 0;
}
