#ifndef _SYS_MSG_H
#define _SYS_MSG_H

#include <sys/ipc.h>
#include <sys/types.h>
#include <time.h>

struct msqid_ds {
    struct ipc_perm msg_perm;
    time_t          msg_stime;
    time_t          msg_rtime;
    time_t          msg_ctime;
    unsigned long   msg_cbytes;
    unsigned long   msg_qnum;
    unsigned long   msg_qbytes;
    pid_t           msg_lspid;
    pid_t           msg_lrpid;
};

struct msgbuf {
    long mtype;
    char mtext[1];
};

#define MSG_NOERROR 0x1000

int    msgget(key_t key, int msgflg);
int    msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg);
int    msgctl(int msqid, int cmd, struct msqid_ds *buf);

#endif /* _SYS_MSG_H */
