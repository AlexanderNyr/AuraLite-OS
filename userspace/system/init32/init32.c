/* userspace/system/init32/init32.c -- the i386 init (I386_PLAN I5).
 *
 * The first C program compiled with the 32-bit toolchain to run in
 * Ring 3 on AuraLite.  Bring-up scope: prove the whole stack -- crt0,
 * the int 0x80 wrapper, the D4 register convention, the kernel's
 * pointer validation -- with output the smoke test asserts exactly.
 * The interactive shell arrives when the keyboard driver does (I7);
 * an init that pretended to read input from a driver that does not
 * exist would be theatre.
 */

#include "lib/libc32/libc32.h"

int main(void)
{
    puts32("\n");
    puts32("AuraLite i386 init: userspace is alive\n");

    long pid = getpid();
    /* Tiny itoa: pid is a tid from an 8-slot table, one digit. */
    char msg[] = "init32: pid=0\n";
    msg[13 - 1] = (char)('0' + (pid % 10));
    puts32(msg);

    /* Round-trip a yield to prove a second syscall shape works. */
    sched_yield();
    puts32("init32: sched_yield returned\n");

    /* A negative control from C: write() with a kernel pointer must
     * be refused (-EFAULT path), not serviced. */
    long r = write(1, (const void *)0xC0100000u, 16);
    if (r < 0)
        puts32("init32: kernel-pointer write refused (EFAULT) -- good\n");
    else
        puts32("init32: BUG: kernel-pointer write was serviced\n");

    puts32("init32: exiting 7\n");
    return 7;
}
