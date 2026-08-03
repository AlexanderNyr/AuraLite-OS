#ifndef AURALITE_LIBC_SYS_MMAN_H
#define AURALITE_LIBC_SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

int    shm_open(const char *name, int oflag, mode_t mode);
int    shm_unlink(const char *name);

#endif /* AURALITE_LIBC_SYS_MMAN_H */
