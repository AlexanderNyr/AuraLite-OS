#ifndef AURALITE_KERNEL_SYNC_FUTEX_H
#define AURALITE_KERNEL_SYNC_FUTEX_H

#include <stdint.h>

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_PRIVATE_FLAG  128

int futex_wait(uint32_t *uaddr, uint32_t val);
int futex_wake(uint32_t *uaddr, int n);

#endif /* AURALITE_KERNEL_SYNC_FUTEX_H */