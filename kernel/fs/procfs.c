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
#include "drivers/timer/pit.h"

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
    if (vn->inode_id == 0) { /* Root of /proc */
        const char *static_files[] = {"uptime", "meminfo", "cpuinfo", "version", "stat"};
        for (int i = 0; i < 5 && n < max; i++) {
            memset(&out[n], 0, sizeof(out[n]));
            strncpy(out[n].name, static_files[i], VFS_PATH_MAX - 1);
            out[n].type = VFS_TYPE_FILE;
            out[n].size = 512;
            out[n].inode = i + 1;
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
        uint64_t ticks = timer_get_ticks();
        uint32_t freq  = timer_get_frequency();
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
                        "cores\t\t: 4\n");
    } else if (vn->inode_id == 4) {
        len = ksnprintf(text, sizeof(text), "AuraLite OS v1.0.0 (x86_64) #1 SMP\n");
    } else if (vn->inode_id == 5) {
        len = ksnprintf(text, sizeof(text), "cpu  1234 0 5678 91011\nintr 1450\nctxt 2450\n");
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
    memcpy(buf, text + pos, copy_len);
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
    out->blocks = (vn->size + 4095) / 4096;
    return 0;
}

const struct vfs_ops procfs_ops = {
    .lookup  = procfs_lookup,
    .read    = procfs_read,
    .write   = NULL,
    .readdir = procfs_readdir,
    .stat    = procfs_stat,
};

void procfs_init(void) {
    kprintf("[procfs] initialising /proc virtual filesystem...\n");
    vfs_mount("/proc", &procfs_ops, NULL);
}
