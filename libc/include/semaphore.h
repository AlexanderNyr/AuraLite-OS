#ifndef AURALITE_LIBC_SEMAPHORE_H
#define AURALITE_LIBC_SEMAPHORE_H

#include <stdint.h>

typedef struct {
    volatile int value;
} sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *sval);

#endif /* AURALITE_LIBC_SEMAPHORE_H */
