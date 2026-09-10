/* P9 Clone declarations */
#ifndef CLONE_DECLS_H
#define CLONE_DECLS_H

#include <stdint.h>

int64_t do_clone(uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls);
int64_t do_arch_prctl(int code, uint64_t addr);
int64_t do_futex(uint64_t uaddr, int op, uint32_t val, uint64_t timeout, uint32_t *uaddr2, uint32_t val3);
int64_t do_tkill(int64_t tid, int sig);

/* Linux-compatible clone flags (subset used by the pthread runtime and by
 * the lx personality's clone arm).  Values are Linux's, so an lx process
 * can pass them through verbatim. */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

#endif
