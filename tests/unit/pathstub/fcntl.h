/* tests/unit/pathstub/fcntl.h — host stand-in for AuraLite's <fcntl.h>.
 *
 * WHY THIS EXISTS
 *
 * libc/src/progpath.c is shipping AuraLite code: its #include "fcntl.h" means
 * AuraLite's header.  Compiled on the host with only -I ., that resolved to
 * GLIBC's /usr/include/fcntl.h instead, and the test got away with it right
 * up until a machine with _FORTIFY_SOURCE enabled by default:
 *
 *   /usr/include/x86_64-linux-gnu/bits/fcntl2.h:41: error: redefinition of 'open'
 *
 * Under fortification glibc defines open() as an inline function, which
 * collides with the test's own open() stub.  The real fault was not the
 * collision — it was that a test of AuraLite code was being compiled against
 * the host's libc headers at all.  Whether that happens to work depends on
 * the distribution's default flags, which is not a property a test should
 * have.
 *
 * These stubs put the include path back under the test's control: progpath.c
 * sees exactly the declarations AuraLite provides, and nothing from glibc.
 */
#ifndef AURALITE_TEST_FCNTL_STUB_H
#define AURALITE_TEST_FCNTL_STUB_H

/* AuraLite's value (libc/include/fcntl.h); it is also Linux's. */
#define O_RDONLY 0x0000

#endif /* AURALITE_TEST_FCNTL_STUB_H */
