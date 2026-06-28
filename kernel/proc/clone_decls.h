/* P9 Clone declarations */
#ifndef CLONE_DECLS_H
#define CLONE_DECLS_H

#include <stdint.h>

int64_t do_clone(uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls);
int64_t do_arch_prctl(int code, uint64_t addr);
int64_t do_futex(uint64_t uaddr, int op, uint32_t val, uint64_t timeout, uint32_t *uaddr2, uint32_t val3);
int64_t do_tkill(int64_t tid, int sig);

#endif
