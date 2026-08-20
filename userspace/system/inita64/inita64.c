/* userspace/system/inita64/inita64.c -- the aarch64 init (ARM64_PLAN
 * A5b).
 *
 * initrv.c's shape at the fourth trap mechanism: the first C program
 * compiled with the aarch64 toolchain to run at EL0 on AuraLite.
 * Bring-up scope: prove the whole stack -- crt0_a64, the svc wrapper,
 * the D4 register convention, the kernel's pointer validation -- with
 * output the smoke test asserts exactly.  (A5b builds and audits this
 * binary; A5c is where the kernel learns to load and run it.)
 */

#include "lib/libca64/libca64.h"

int main(void)
{
    puts_a64("\n");
    puts_a64("AuraLite a64 init: userspace is alive\n");

    long pid = getpid();
    /* Tiny itoa: pid is a tid from a small table, one digit. */
    char msg[] = "inita64: pid=0\n";
    msg[14 - 1] = (char)('0' + (pid % 10));
    puts_a64(msg);

    /* Round-trip a yield to prove a second syscall shape works. */
    sched_yield();
    puts_a64("inita64: sched_yield returned\n");

    /* A negative control from C: write() with a kernel pointer must
     * be refused (-EFAULT path), not serviced.  The address is the
     * HHDM base -- the SAME constant as rv64 by TTBR1 arithmetic
     * (plan D3), unquestionably kernel either way. */
    long r = write(1, (const void *)0xFFFFFFC000000000UL, 16);
    if (r < 0)
        puts_a64("inita64: kernel-pointer write refused (EFAULT) -- good\n");
    else
        puts_a64("inita64: BUG: kernel-pointer write was serviced\n");

    puts_a64("inita64: exiting 7\n");
    return 7;
}
