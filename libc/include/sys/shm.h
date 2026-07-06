#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/ipc.h>
#include <sys/types.h>
#include <time.h>

#define SHM_RDONLY 0x1000
#define SHM_RND    0x2000

#define SHMMAX 0x2000000

struct shmid_ds {
    struct ipc_perm shm_perm;
    size_t          shm_segsz;
    time_t          shm_atime;
    time_t          shm_dtime;
    time_t          shm_ctime;
    pid_t           shm_cpid;
    pid_t           shm_lpid;
    unsigned short  shm_nattch;
};

int   shmget(key_t key, size_t size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int   shmdt(const void *shmaddr);
int   shmctl(int shmid, int cmd, struct shmid_ds *buf);

#endif /* _SYS_SHM_H */
