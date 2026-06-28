#ifndef _TIME_H
#define _TIME_H

#include <sys/types.h>

typedef long          time_t;
typedef long          clock_t;
typedef int           clockid_t;

#define CLOCKS_PER_SEC 100

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

int      clock_gettime(clockid_t clockid, struct timespec *tp);
int      clock_getres(clockid_t clockid, struct timespec *res);
int      nanosleep(const struct timespec *req, struct timespec *rem);
int      gettimeofday(struct timeval *tv, void *tz);
time_t   time(time_t *t);
int      getitimer(int which, struct itimerval *curr_value);
int      setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value);

double   difftime(time_t time1, time_t time0);
time_t   mktime(struct tm *tm);
struct tm *gmtime(const time_t *timer);
struct tm *localtime(const time_t *timer);
char    *asctime(const struct tm *tm);
char    *ctime(const time_t *timer);
size_t   strftime(char *s, size_t max, const char *format, const struct tm *tm);

unsigned int sleep(unsigned int seconds);
int usleep(unsigned long usec);

#endif /* _TIME_H */