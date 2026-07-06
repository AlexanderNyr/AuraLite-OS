#ifndef _MQUEUE_H
#define _MQUEUE_H

#include <fcntl.h>
#include <sys/types.h>
#include <time.h>

typedef int mqd_t;
#define MQD_INVALID ((mqd_t)-1)

struct mq_attr {
    long mq_flags;
    long mq_maxmsg;
    long mq_msgsize;
    long mq_curmsgs;
};

mqd_t   mq_open(const char *name, int oflag, ...);
int     mq_close(mqd_t mqdes);
int     mq_unlink(const char *name);
int     mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned msg_prio);
ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned *msg_prio);
int     mq_timedsend(mqd_t mqdes, const char *msg_ptr, size_t msg_len,
                     unsigned msg_prio, const struct timespec *abs_timeout);
ssize_t mq_timedreceive(mqd_t mqdes, char *msg_ptr, size_t msg_len,
                        unsigned *msg_prio, const struct timespec *abs_timeout);
int     mq_getattr(mqd_t mqdes, struct mq_attr *mqstat);
int     mq_setattr(mqd_t mqdes, const struct mq_attr *restrict mqstat,
                   struct mq_attr *restrict omqstat);
int     mq_notify(mqd_t mqdes, const struct sigevent *notification);

#endif /* _MQUEUE_H */
