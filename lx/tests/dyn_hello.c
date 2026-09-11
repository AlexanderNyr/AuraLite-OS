/* lx/tests/dyn_hello.c — the LX_COMPAT L4 gate program.
 *
 * Built on the HOST with the host's own gcc and the host's DEFAULT flags
 * (Debian gcc default: a PIE, dynamically linked against glibc) — an
 * unmodified, ordinary Linux x86-64 binary, exactly what the ladder's
 * fourth rung promises.  It must survive the whole dynamic startup:
 *   - the kernel reading PT_INTERP (/lib64/ld-linux-x86-64.so.2),
 *   - ld.so mapping itself + libc via mmap/MAP_FIXED + mprotect,
 *   - futex (WAIT/WAKE/BITSET) on the loader's internal locks,
 *   - TLS via arch_prctl(ARCH_SET_FS), set_tid_address, set_robust_list,
 *   - rseq answered -ENOSYS (glibc's documented fallback),
 *   - AT_PHDR/AT_ENTRY/AT_BASE/AT_RANDOM on the initial stack,
 * and then print one greppable line and exit 0, so the case can also
 * assert clean exit-status propagation.
 */

#include <stdio.h>

int main(void) {
    printf("LX4-HELLO-OK\n");
    return 0;
}
