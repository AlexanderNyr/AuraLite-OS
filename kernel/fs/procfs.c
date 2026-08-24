/* procfs.c — /proc virtual filesystem. */

#include <stdint.h>
#include "kernel/fs/procfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/net/netdev.h"
#include "kernel/fs/blkdev.h"
#include "kernel/time.h"      /* R6: the time seam */
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/diagnostics.h"
#include "kernel/lib/perfstat.h"

#define PROCFS_MAX_VNODES 64
static struct vnode procfs_vnodes[PROCFS_MAX_VNODES];
static int vnode_rr = 0;

static struct vnode *get_procfs_vnode(void) {
    struct vnode *vn = &procfs_vnodes[vnode_rr];
    vnode_rr = (vnode_rr + 1) % PROCFS_MAX_VNODES;
    memset(vn, 0, sizeof(*vn));
    vn->ops = &procfs_ops;
    return vn;
}

/* Helper to parse positive int. */
static uint64_t parse_pid(const char *s, const char **endptr) {
    uint64_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    if (endptr) *endptr = s;
    return val;
}

static struct vnode *procfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    if (path[0] == '\0') {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "proc", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_DIR;
        vn->mode = 0755;
        vn->size = 0;
        vn->inode_id = 0;
        return vn;
    }
    if (strcmp(path, "uptime") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "uptime", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 64;
        vn->inode_id = 1;
        return vn;
    }
    if (strcmp(path, "meminfo") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "meminfo", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 512;
        vn->inode_id = 2;
        return vn;
    }
    if (strcmp(path, "cpuinfo") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "cpuinfo", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 512;
        vn->inode_id = 3;
        return vn;
    }
    if (strcmp(path, "version") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "version", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 128;
        vn->inode_id = 4;
        return vn;
    }
    if (strcmp(path, "stat") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "stat", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 256;
        vn->inode_id = 5;
        return vn;
    }
    if (strcmp(path, "loadavg") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "loadavg", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 64;
        vn->inode_id = 6;
        return vn;
    }
    if (strcmp(path, "netdev") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "netdev", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 256;
        vn->inode_id = 7;
        return vn;
    }
    if (strcmp(path, "diskstats") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "diskstats", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 256;
        vn->inode_id = 8;
        return vn;
    }
    if (strcmp(path, "perf") == 0) {
        /* OPT_PLAN.md O0: the perfstat counters, one "name value" line
         * each.  The names are perfstat.c's table; tests parse this file,
         * so the format is an interface. */
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "perf", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0644;
        vn->size = 512;
        vn->inode_id = 15;
        return vn;
    }
    if (strcmp(path, "sysrq-trigger") == 0) {
        /* FIX_R0 test gate: Linux-style /proc/sysrq-trigger.  Writing 'c'
         * requests a deliberate kernel fault so the fatal diagnostics can
         * be integration-tested.  Write-only, mirroring Linux. */
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "sysrq-trigger", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_FILE;
        vn->mode = 0200;
        vn->size = 0;
        vn->inode_id = 9;
        return vn;
    }

    /* Q13: /proc/self/fd/<N> — resolve an open file descriptor to its real
     * vnode.  This is what fdopendir() (via SYS_LISTDIR on the synthetic
     * path) and fexecve() (via execveat's AT_EMPTY_PATH) rely on; returning
     * the fd's actual vnode means open/read/stat/readdir all behave like
     * the original.  The "self" node is a directory standing for the
     * calling process. */
    if (strcmp(path, "self") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "self", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_DIR;
        vn->mode = 0755;
        vn->inode_id = 0x7FFFFFF0ull;
        return vn;
    }
    if (strcmp(path, "self/fd") == 0) {
        struct vnode *vn = get_procfs_vnode();
        strncpy(vn->name, "fd", VFS_PATH_MAX - 1);
        vn->type = VFS_TYPE_DIR;
        vn->mode = 0755;
        vn->inode_id = 0x7FFFFFF1ull;
        return vn;
    }
    if (strncmp(path, "self/fd/", 8) == 0) {
        int fd = 0;
        for (const char *q = path + 8; *q; q++) {
            if (*q < '0' || *q > '9') return NULL;
            fd = fd * 10 + (*q - '0');
            if (fd > VFS_MAX_FDS) return NULL;
        }
        return vfs_get_vnode(fd);
    }

    /* Check if it's a PID directory or PID file (e.g. "1", "1/status", "1/cmdline"). */
    const char *p = path;
    if (*p >= '0' && *p <= '9') {
        const char *end;
        uint64_t pid = parse_pid(p, &end);
        if (!thread_get_by_pid(pid)) {
            return NULL; /* Process not found */
        }
        if (*end == '\0') {
            struct vnode *vn = get_procfs_vnode();
            ksnprintf(vn->name, sizeof(vn->name), "%llu", (unsigned long long)pid);
            vn->type = VFS_TYPE_DIR;
            vn->mode = 0755;
            vn->size = 0;
            vn->inode_id = (pid << 16) | 10;
            return vn;
        }
        if (*end == '/') {
            end++;
            if (strcmp(end, "status") == 0) {
                struct vnode *vn = get_procfs_vnode();
                strncpy(vn->name, "status", VFS_PATH_MAX - 1);
                vn->type = VFS_TYPE_FILE;
                vn->mode = 0644;
                vn->size = 512;
                vn->inode_id = (pid << 16) | 11;
                return vn;
            }
            if (strcmp(end, "cmdline") == 0) {
                struct vnode *vn = get_procfs_vnode();
                strncpy(vn->name, "cmdline", VFS_PATH_MAX - 1);
                vn->type = VFS_TYPE_FILE;
                vn->mode = 0644;
                vn->size = 128;
                vn->inode_id = (pid << 16) | 12;
                return vn;
            }
            if (strcmp(end, "stat") == 0) {
                struct vnode *vn = get_procfs_vnode();
                strncpy(vn->name, "stat", VFS_PATH_MAX - 1);
                vn->type = VFS_TYPE_FILE;
                vn->mode = 0644;
                vn->size = 256;
                vn->inode_id = (pid << 16) | 13;
                return vn;
            }
        }
    }
    return NULL;
}

static int procfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    int n = 0;
    if (vn->inode_id == 0x7FFFFFF0ull) { /* /proc/self */
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, "fd", VFS_PATH_MAX - 1);
        out[n].type = VFS_TYPE_DIR;
        n++;
        return n;
    }
    if (vn->inode_id == 0x7FFFFFF1ull) { /* /proc/self/fd — the open FDs */
        struct ofd **ft = sched_current() ? sched_current()->fd_table : NULL;
        for (int i = 0; i < VFS_MAX_FDS && n < max; i++) {
            if (ft && ft[i]) {
                memset(&out[n], 0, sizeof(out[n]));
                ksnprintf(out[n].name, VFS_PATH_MAX, "%d", i);
                out[n].type = VFS_TYPE_FILE;
                out[n].inode = (uint64_t)i;
                n++;
            }
        }
        return n;
    }
    if (vn->inode_id == 0) { /* Root of /proc */
        const char *static_files[] = {"uptime", "meminfo", "cpuinfo", "version", "stat",
                                       "loadavg", "netdev", "diskstats", "sysrq-trigger",
                                       "perf"};
        const uint64_t static_inodes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 15};
        for (int i = 0; i < 10 && n < max; i++) {
            memset(&out[n], 0, sizeof(out[n]));
            strncpy(out[n].name, static_files[i], VFS_PATH_MAX - 1);
            out[n].type = VFS_TYPE_FILE;
            out[n].size = 512;
            out[n].inode = static_inodes[i];
            n++;
        }
        /* Add active PIDs */
        tcb_t *list[64];
        int count = thread_get_all(list, 64);
        for (int i = 0; i < count && n < max; i++) {
            memset(&out[n], 0, sizeof(out[n]));
            ksnprintf(out[n].name, VFS_PATH_MAX, "%llu", (unsigned long long)list[i]->id);
            out[n].type = VFS_TYPE_DIR;
            out[n].size = 0;
            out[n].inode = (list[i]->id << 16) | 10;
            n++;
        }
        return n;
    }
    if ((vn->inode_id & 0xFFFF) == 10) { /* /proc/<pid> directory */
        uint64_t pid = vn->inode_id >> 16;
        const char *pid_files[] = {"status", "cmdline", "stat"};
        for (int i = 0; i < 3 && n < max; i++) {
            memset(&out[n], 0, sizeof(out[n]));
            strncpy(out[n].name, pid_files[i], VFS_PATH_MAX - 1);
            out[n].type = VFS_TYPE_FILE;
            out[n].size = 512;
            out[n].inode = (pid << 16) | (11 + i);
            n++;
        }
        return n;
    }
    return -ENOTDIR;   /* readdir on a non-directory procfs node */
}

static int64_t procfs_read(struct vnode *vn, uint64_t pos, void *buf, uint64_t count) {
    char text[1024];
    memset(text, 0, sizeof(text));
    int len = 0;

    if (vn->inode_id == 1) {
        uint64_t ticks = ktime_ticks();
        uint32_t freq  = ktime_hz();
        if (freq == 0) freq = 100;
        len = ksnprintf(text, sizeof(text), "%llu.%02llu\n",
                        (unsigned long long)(ticks / freq),
                        (unsigned long long)((ticks % freq) * 100 / freq));
    } else if (vn->inode_id == 2) {
        uint64_t usable = pmm_get_usable_frames() * PMM_PAGE_SIZE;
        uint64_t free_mem = pmm_get_free_frames() * PMM_PAGE_SIZE;
        uint64_t used = usable - free_mem;
        len = ksnprintf(text, sizeof(text),
                        "MemTotal:       %llu KiB\n"
                        "MemFree:        %llu KiB\n"
                        "MemUsed:        %llu KiB\n"
                        "total usable:   %llu MiB, free: %llu MiB\n",
                        (unsigned long long)(usable / 1024),
                        (unsigned long long)(free_mem / 1024),
                        (unsigned long long)(used / 1024),
                        (unsigned long long)(usable / (1024 * 1024)),
                        (unsigned long long)(free_mem / (1024 * 1024)));
    } else if (vn->inode_id == 3) {
        len = ksnprintf(text, sizeof(text),
                        "processor\t: 0\n"
                        "vendor_id\t: GenuineIntel\n"
                        "cpu family\t: 6\n"
                        "model name\t: AuraLite x86_64 CPU\n"
                        "cores\t\t: %u\n",
                        (unsigned)smp_get_cpu_count());
    } else if (vn->inode_id == 4) {
        len = ksnprintf(text, sizeof(text), "AuraLite OS v0.0.1 (x86_64) #1 SMP\n");
    } else if (vn->inode_id == 5) {
        /* Linux-style jiffie counters: user/nice/system/idle, in PIT ticks.
         * AuraLite doesn't distinguish user/nice/system time yet, so all
         * non-idle time is reported as "system" (3rd field) -- but idle
         * (4th field) is real, sourced from sched_get_idle_ticks(), so
         * `top`-style tools computing %busy = 1 - idle_delta/total_delta
         * get an accurate answer. */
        uint64_t total = sched_get_total_ticks();
        uint64_t idle  = sched_get_idle_ticks();
        uint64_t busy  = (total >= idle) ? (total - idle) : 0;
        len = ksnprintf(text, sizeof(text),
                        "cpu  0 0 %llu %llu\nintr 0\nctxt 0\n",
                        (unsigned long long)busy, (unsigned long long)idle);
    } else if (vn->inode_id == 6) {
        /* /proc/loadavg: real instantaneous CPU busy% (over all ticks since
         * boot) in the first field, formatted like a 1-minute load average
         * so existing "load average" style tooling/parsers keep working;
         * fields 2-3 (5/15 min) are not tracked separately and mirror
         * field 1. Last two fields (runnable/total threads, last PID) use
         * real data from thread_get_all()/tid allocation. */
        /* 1-second window from sched_tick, not lifetime busy/total
         * (that number is boot self-tests and never comes down). */
        uint64_t busy_pct_x100 = sched_get_busy_pct_x100();
        tcb_t *list[64];
        int nthreads = thread_get_all(list, 64);
        int runnable = 0;
        for (int i = 0; i < nthreads; i++) {
            if (list[i]->state == THREAD_RUNNING || list[i]->state == THREAD_READY) runnable++;
        }
        len = ksnprintf(text, sizeof(text), "%llu.%02llu %llu.%02llu %llu.%02llu %d/%d 0\n",
                        (unsigned long long)(busy_pct_x100 / 100), (unsigned long long)(busy_pct_x100 % 100),
                        (unsigned long long)(busy_pct_x100 / 100), (unsigned long long)(busy_pct_x100 % 100),
                        (unsigned long long)(busy_pct_x100 / 100), (unsigned long long)(busy_pct_x100 % 100),
                        runnable, nthreads);
    } else if (vn->inode_id == 7) {
        /* /proc/netdev: cumulative bytes/packets for the active NIC. */
        uint64_t rxb = 0, txb = 0, rxp = 0, txp = 0;
        netdev_get_stats(&rxb, &txb, &rxp, &txp);
        len = ksnprintf(text, sizeof(text),
                        "Inter-|   Receive                | Transmit\n"
                        " face |bytes    packets|bytes    packets\n"
                        "%6s: %8llu %8llu %8llu %8llu\n",
                        netdev_name(),
                        (unsigned long long)rxb, (unsigned long long)rxp,
                        (unsigned long long)txb, (unsigned long long)txp);
    } else if (vn->inode_id == 8) {
        /* /proc/diskstats: cumulative sectors read/written through the
         * blkdev seam, all devices aggregated (see blkdev_get_stats()).
         * P1 semantic shift, named: driver-internal traffic (the AHCI
         * self-test's probe sectors) no longer counts -- these are
         * filesystem-layer counters now, and the row is labelled blk0
         * accordingly. */
        uint64_t sread = 0, swritten = 0;
        blkdev_get_stats(&sread, &swritten);
        len = ksnprintf(text, sizeof(text),
                        "   1    0 blk0 0 0 %llu 0 0 0 %llu 0 0 0 0\n",
                        (unsigned long long)sread, (unsigned long long)swritten);
    } else if (vn->inode_id == 9) {
        /* /proc/sysrq-trigger read: list the supported commands. */
        len = ksnprintf(text, sizeof(text),
                        "AuraLite sysrq trigger commands:\n"
                        "  c - crash: deliberate kernel page fault "
                        "(FIX_R0 diagnostics test gate; "
                        "STOP 0x0000000E PAGE_FAULT)\n"
                        "  o - overflow: deliberate kernel stack overflow "
                        "-> #DF (FIX_R1 IST test gate; "
                        "STOP 0x00000008 DOUBLE_FAULT)\n");
    } else if (vn->inode_id == 15) {
        /* /proc/perf: the OPT_PLAN O0 counters, "name value" per line. */
        for (int i = 0; i < perfstat_counter_count(); i++) {
            len += ksnprintf(text + len, sizeof(text) - (size_t)len,
                             "%s %llu\n", perfstat_name(i),
                             (unsigned long long)perfstat_get(i));
            if (len >= (int)sizeof(text) - 1) break;
        }
    } else if ((vn->inode_id >> 16) != 0) {
        uint64_t pid = vn->inode_id >> 16;
        uint64_t file_type = vn->inode_id & 0xFFFF;
        tcb_t *t = thread_get_by_pid(pid);
        if (!t) return 0;
        if (file_type == 11) {
            const char *st = (t->state == THREAD_RUNNING) ? "Running" :
                             (t->state == THREAD_READY)   ? "Ready"   :
                             (t->state == THREAD_BLOCKED) ? "Blocked" : "Dead";
            len = ksnprintf(text, sizeof(text),
                            "Name:\t%s\nState:\t%s\nPID:\t%llu\nPPID:\t%llu\nPML4:\t0x%llx\n",
                            t->name, st, (unsigned long long)t->id,
                            (unsigned long long)(t->parent ? t->parent->id : 0),
                            (unsigned long long)t->pml4_phys);
        } else if (file_type == 12) {
            len = ksnprintf(text, sizeof(text), "%s\n", t->name);
        } else if (file_type == 13) {
            len = ksnprintf(text, sizeof(text), "%llu (%s) %c %llu %llu\n",
                            (unsigned long long)t->id, t->name,
                            (t->state == THREAD_RUNNING) ? 'R' : (t->state == THREAD_READY) ? 'r' : (t->state == THREAD_BLOCKED) ? 'S' : 'Z',
                            (unsigned long long)(t->parent ? t->parent->id : 0),
                            (unsigned long long)t->quantum);
        }
    }

    if (pos >= (uint64_t)len) return 0;
    uint64_t copy_len = len - pos;
    if (copy_len > count) copy_len = count;
    memcpy(buf, text + pos, (size_t)copy_len);
    return (int64_t)copy_len;
}

static int procfs_stat(struct vnode *vn, struct vfs_stat *out) {
    memset(out, 0, sizeof(*out));
    out->type  = vn->type;
    out->mode  = vn->mode;
    out->uid   = vn->uid;
    out->gid   = vn->gid;
    out->size  = vn->size;
    out->inode = vn->inode_id;
    out->nlink = 1;
    out->blocks = (uint32_t)((vn->size + 4095) / 4096);
    return 0;
}

/* /proc/sysrq-trigger write.  Only command at the moment: 'c' (crash) —
 * a deliberate kernel page fault, so the FIX_R0 fatal-diagnostics path can
 * be exercised end-to-end from the shell.  Mirroring Linux, the write that
 * triggers the crash does not return a result anyone could observe. */
static int64_t procfs_write(struct vnode *vn, uint64_t pos,
                            const void *buf, uint64_t count) {
    (void)pos;
    if (!vn || (!buf && count != 0)) {
        return -EINVAL;
    }
    if (vn->inode_id == 9) {
        const char *s = (const char *)buf;
        for (uint64_t i = 0; i < count; i++) {
            if (s[i] == 'c') {
                kprintf("[sysrq] trigger 'c': deliberate kernel fault "
                        "requested (cpu%u)\n", diag_cpu_id());
                diag_trigger_kernel_fault();
                /* Reaching this would mean address 0 turned out mapped. */
                kprintf("[sysrq] trigger 'c': the deliberate fault did NOT "
                        "happen — this should never print\n");
            } else if (s[i] == 'o') {
                kprintf("[sysrq] trigger 'o': deliberate kernel stack "
                        "overflow requested (cpu%u)\n", diag_cpu_id());
                diag_trigger_kernel_stack_overflow();
                /* Reaching this would mean the stack did not overflow. */
                kprintf("[sysrq] trigger 'o': recursion RETURNED — the "
                        "overflow did not happen, this should never print\n");
            }
        }
        return (int64_t)count;
    }
    return -EINVAL;
}

const struct vfs_ops procfs_ops = {
    .lookup  = procfs_lookup,
    .read    = procfs_read,
    .write   = procfs_write,
    .readdir = procfs_readdir,
    .stat    = procfs_stat,
};

void procfs_init(void) {
    kprintf("[procfs] initialising /proc virtual filesystem...\n");
    vfs_mount("/proc", &procfs_ops, NULL);
}
