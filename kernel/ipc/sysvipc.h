#ifndef AURALITE_KERNEL_SYSVIPC_H
#define AURALITE_KERNEL_SYSVIPC_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/proc/thread.h"   /* tcb_t for the teardown hook */

/*
 * System V IPC — POSIX2024 phase Q14.
 *
 * Replaces the twelve ENOSYS stubs from Q10 with real kernel objects:
 * semaphores (semget/semop/semctl), shared memory (shmget/shmat/shmdt/
 * shmctl) and message queues (msgget/msgsnd/msgrcv/msgctl).
 *
 * Non-goals (deliberate, per POSIX2024_PLAN.md Q14): MSG_COPY, SHM_LOCK,
 * namespace juggling, /proc/sysvipc.
 */

/* ipc.h constants (must match lib/libc/include/sys/ipc.h). */
#define SYSV_IPC_PRIVATE 0
#define SYSV_IPC_CREAT   01000
#define SYSV_IPC_EXCL    02000
#define SYSV_IPC_NOWAIT  04000
#define SYSV_IPC_RMID    0
#define SYSV_IPC_SET     1
#define SYSV_IPC_STAT    2

/* semctl commands (must match lib/libc/include/sys/sem.h). */
#define SYSV_GETPID  11
#define SYSV_GETVAL  12
#define SYSV_GETALL  13
#define SYSV_SETVAL  16
#define SYSV_SETALL  17

#define SYSV_SEM_UNDO 0x1000

/* shmat flags (must match lib/libc/include/sys/shm.h). */
#define SYSV_SHM_RDONLY 0x1000
#define SYSV_SHM_RND    0x2000

/* msgrcv flags (must match lib/libc/include/sys/msg.h). */
#define SYSV_MSG_NOERROR 0x1000

#define SYSV_MAX_OBJS 32

/* Kernel ABI structs.  The libc side declares byte-identical layouts. */
struct sysv_sembuf {   /* == struct sembuf (userspace) */
    unsigned short sem_num;
    short          sem_op;
    short          sem_flg;
};

struct sysv_ipc_perm { /* == struct ipc_perm (userspace) */
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    unsigned short seq;
    int64_t  key;
};

struct sysv_semid_ds { /* == struct semid_ds (userspace) */
    struct sysv_ipc_perm sem_perm;
    unsigned short       sem_nsems;
    int64_t              sem_otime;
    int64_t              sem_ctime;
};

struct sysv_shmid_ds { /* == struct shmid_ds (userspace) */
    struct sysv_ipc_perm shm_perm;
    uint64_t             shm_segsz;
    int64_t              shm_atime;
    int64_t              shm_dtime;
    int64_t              shm_ctime;
    int32_t              shm_cpid;
    int32_t              shm_lpid;
    unsigned short       shm_nattch;
};

struct sysv_msqid_ds { /* == struct msqid_ds (userspace) */
    struct sysv_ipc_perm msg_perm;
    int64_t              msg_stime;
    int64_t              msg_rtime;
    int64_t              msg_ctime;
    uint64_t             msg_cbytes;
    uint64_t             msg_qnum;
    uint64_t             msg_qbytes;
    int32_t              msg_lspid;
    int32_t              msg_lrpid;
};

/* Syscall entry points.  All pointer arguments are USER pointers. */
int64_t sysv_semget(int64_t key, int nsems, int semflg);
int64_t sysv_semop(int semid, const void *sops_user, uint64_t nsops);
int64_t sysv_semctl(int semid, int semnum, int cmd, uint64_t arg);
int64_t sysv_shmget(int64_t key, uint64_t size, int shmflg);
uint64_t sysv_shmat(int shmid, uint64_t shmaddr, int shmflg);
int64_t sysv_shmdt(uint64_t shmaddr);
int64_t sysv_shmctl(int shmid, int cmd, uint64_t buf_user);
int64_t sysv_msgget(int64_t key, int msgflg);
int64_t sysv_msgsnd(int msqid, const void *msgp_user, uint64_t msgsz, int msgflg);
int64_t sysv_msgrcv(int msqid, void *msgp_user, uint64_t msgsz,
                    int64_t msgtyp, int msgflg);
int64_t sysv_msgctl(int msqid, int cmd, uint64_t buf_user);

/* Called from thread exit: apply SEM_UNDO records and detach all shm
 * segments attached by the dying process. */
void sysvipc_cleanup_process(tcb_t *t);

/* Called from execve: detach all shm segments (POSIX/Linux semantics —
 * attached segments are inherited across fork but detached at exec).
 * SEM_UNDO records are NOT applied here (they survive exec). */
void sysvipc_shm_detach_all(tcb_t *t);

#endif /* AURALITE_KERNEL_SYSVIPC_H */
