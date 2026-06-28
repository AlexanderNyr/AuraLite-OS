#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/string.h"
int do_getcwd(char *buf, size_t size) {
    tcb_t *cur = sched_current();
    if (!cur) return -1;
    strncpy(buf, cur->cwd[0] ? cur->cwd : "/", size);
    return 0;
}
int do_chdir(const char *p) { (void)p; return 0; }
