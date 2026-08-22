/* userspace/system/initrv/initrv.c -- the rv64 init (RISCV_PLAN V5).
 *
 * init32.c's shape at the third width: the first C program compiled
 * with the rv64 toolchain to run in U-mode on AuraLite.  Bring-up
 * scope: prove the whole stack -- crt0_rv, the ecall wrapper, the D4
 * register convention, the kernel's pointer validation -- with output
 * the smoke test asserts exactly.
 */

#include "lib/libcrv/libcrv.h"

int main(void)
{
    puts_rv("\n");
    puts_rv("AuraLite rv64 init: userspace is alive\n");

    /* P8: the shared mini-printf, live on every port -- the
     * output is byte-identical to the old tiny itoa. */
    aura_printf("initrv: pid=%d\n", (int)getpid());

    /* Round-trip a yield to prove a second syscall shape works. */
    sched_yield();
    puts_rv("initrv: sched_yield returned\n");

    /* A negative control from C: write() with a kernel pointer must
     * be refused (-EFAULT path), not serviced.  The address is the
     * HHDM base -- unquestionably kernel, unquestionably mapped, so
     * the refusal is the bounds check's doing, not an accident of an
     * unmapped page. */
    long r = write(1, (const void *)0xFFFFFFC000000000UL, 16);
    if (r < 0)
        puts_rv("initrv: kernel-pointer write refused (EFAULT) -- good\n");
    else
        puts_rv("initrv: BUG: kernel-pointer write was serviced\n");

    puts_rv("initrv: exiting 7\n");
    return 7;
}
