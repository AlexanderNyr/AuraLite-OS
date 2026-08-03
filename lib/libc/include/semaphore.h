#ifndef AURALITE_LIBC_SEMAPHORE_H
#define AURALITE_LIBC_SEMAPHORE_H

#include <stdint.h>
#include <time.h>

typedef struct {
    volatile int value;
} sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *sval);

/* Named semaphores (Q7) */
#define SEM_FAILED ((sem_t *)(void *)-1)

sem_t  *sem_open(const char *name, int oflag, ...);
int     sem_close(sem_t *sem);
int     sem_unlink(const char *name);
int     sem_timedwait(sem_t *sem, const struct timespec *abs_timeout);

#endif /* AURALITE_LIBC_SEMAPHORE_H */
