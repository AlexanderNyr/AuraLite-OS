/* tests/unit/pathstub/unistd.h — host stand-in for AuraLite's <unistd.h>.
 *
 * See fcntl.h in this directory for why these stubs exist. In short:
 * progpath.c is AuraLite code, so its #include "unistd.h" must mean
 * AuraLite's header, not glibc's — the test was previously compiled against
 * the host's libc and only broke once a machine enabled _FORTIFY_SOURCE.
 *
 * Only what progpath.c actually calls is declared. The signatures match
 * libc/include/unistd.h exactly, so a drift between the two would be a
 * compile error here rather than a silent difference.
 */
#ifndef AURALITE_TEST_UNISTD_STUB_H
#define AURALITE_TEST_UNISTD_STUB_H

int open(const char *path, int flags, ...);
int close(int fd);

#endif /* AURALITE_TEST_UNISTD_STUB_H */
