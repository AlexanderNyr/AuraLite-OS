/* lx/tests/hello.c — the LX_COMPAT L1 gate program.
 *
 * Built on the HOST with the host's own gcc, -static, against the
 * host's glibc — an unmodified, ordinary Linux x86-64 binary, exactly
 * what the ladder's first rung promises.  It must survive:
 *   - glibc's static startup (set_tid_address, brk, arch_prctl, and
 *     every probed call we answer -ENOSYS to),
 *   - the stack-canary init from AT_RANDOM,
 *   - write(2) to fd 1 through the lx number map,
 *   - exit_group through the lx-only arm.
 *
 * The exit status is 0 so the case can also assert clean propagation.
 */

#include <unistd.h>

int main(void) {
    static const char msg[] = "[lx] hello from an unmodified Linux binary\n";
    ssize_t n = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    if (n != (ssize_t)(sizeof(msg) - 1)) {
        static const char err[] = "[lx] short write\n";
        (void)write(STDERR_FILENO, err, sizeof(err) - 1);
        return 1;
    }
    return 0;
}
