#ifndef _SYS_SEM_H
#define _SYS_SEM_H

#include <sys/ipc.h>
#include <sys/types.h>
#include <time.h>

#define SEM_UNDO 0x1000

#define GETPID  11
#define GETVAL  12
#define SETVAL  16
#define GETALL  13
#define SETALL  17
#define IPC_INFO 14

struct sembuf {
    unsigned short sem_num;
    short          sem_op;
    short          sem_flg;
};

struct semid_ds {
    struct ipc_perm sem_perm;
    unsigned short  sem_nsems;
    time_t          sem_otime;
    time_t          sem_ctime;
};

int semget(key_t key, int nsems, int semflg);
int semop(int semid, struct sembuf *sops, size_t nsops);
int semctl(int semid, int semnum, int cmd, ...);

#endif /* _SYS_SEM_H */
