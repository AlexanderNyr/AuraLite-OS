/* tests/unit/glstub/unistd.h — host stand-in for AuraLite's <unistd.h>.
 *
 * Only glvirgl.c needs anything from it: syscall(), which it uses to reach
 * SYS_GPU_CALL.  The host's real unistd.h declares a syscall() with a
 * different signature, so this shadows it — the alternative is an #ifdef in
 * shipping code to accommodate a test build, which is the wrong way round.
 *
 * The stub always reports "no GPU", which is what makes the backend tests
 * meaningful: they assert that the VirGL candidate DECLINES and the registry
 * falls through to software.
 */
#ifndef AURALITE_TEST_UNISTD_STUB_H
#define AURALITE_TEST_UNISTD_STUB_H

#include <stdint.h>
#include <stddef.h>

int64_t syscall(int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* AURALITE_TEST_UNISTD_STUB_H */
