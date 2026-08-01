/* kernel/gpu/gpu_cmdcheck.c — VirGL command-stream validator.
 *
 * Phase K1 of GL_PLAN.md.
 *
 * Split into its own translation unit for one reason: this is the function
 * standing between a hostile user process and the host GPU, and it is worth
 * testing directly with deliberately malformed input rather than only through
 * the syscall.  It has no kernel dependencies — no allocation, no user copies,
 * no driver calls — so tests/unit/test_gpu_syscall.c links THIS FILE and
 * therefore exercises the shipping code rather than a copy of it.
 */

#include <stdint.h>

#include "kernel/gpu/gpu_syscalls.h"

/* ============================================================================
 * Command-stream validation
 *
 * This function is what stands between a hostile process and the host GPU, so
 * it is deliberately simple enough to audit by reading.
 *
 * A VirGL stream is a sequence of packets.  Each starts with a header dword
 * whose top 16 bits are the opcode/object and whose low 16 bits are the
 * payload length IN DWORDS, not counting the header itself.  Walking the
 * stream means repeatedly reading a length and stepping over it — and the only
 * thing that can go wrong is a length that runs past the end of the buffer,
 * which is exactly what an attacker would supply.
 *
 * A stream whose header lies is rejected WHOLE rather than truncated: partial
 * execution of an attacker-shaped stream is not a safe state.
 * ==========================================================================*/

int gpu_validate_cmd_stream(const uint32_t *cmd, uint32_t dwords) {
    if (!cmd) return -1;
    if (dwords == 0) return -1;

    uint32_t i = 0;
    uint32_t packets = 0;

    while (i < dwords) {
        uint32_t header = cmd[i];
        uint32_t len = header & 0xFFFFu;      /* payload length in dwords */

        /* The header itself plus its payload must fit in what remains.  The
         * addition is done in 64-bit to make overflow impossible even if len
         * is 0xFFFF and i is near the end. */
        uint64_t need = (uint64_t)i + 1ull + (uint64_t)len;
        if (need > (uint64_t)dwords) return -1;

        i = (uint32_t)need;

        /* A stream of empty packets would otherwise let a caller burn kernel
         * time in this loop.  The bound is generous: real streams are tens to
         * hundreds of packets. */
        if (++packets > 65536u) return -1;
    }

    /* The walk must land exactly on the end.  Landing short would mean a
     * trailing partial packet, which the loop above already rejects, but
     * asserting it here documents the intended post-condition. */
    return (i == dwords) ? 0 : -1;
}

