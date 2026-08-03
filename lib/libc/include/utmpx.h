#ifndef _UTMPX_H
#define _UTMPX_H

#include <sys/types.h>
#include <time.h>

#define EMPTY      0
#define RUN_LVL    1
#define BOOT_TIME  2
#define NEW_TIME   3
#define OLD_TIME   4
#define INIT_PROCESS 5
#define LOGIN_PROCESS 6
#define USER_PROCESS  7
#define DEAD_PROCESS  8

struct utmpx {
    char    ut_user[32];
    char    ut_id[4];
    char    ut_line[32];
    pid_t   ut_pid;
    short   ut_type;
    struct timeval ut_tv;
};

void setutxent(void);
void endutxent(void);
struct utmpx *getutxent(void);

#endif /* _UTMPX_H */
