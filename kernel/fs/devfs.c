/* devfs.c — /dev filesystem with null and zero character devices. */

#include <stdint.h>
#include "kernel/fs/devfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/mm/kheap.h"
#include "kernel/audio/audio.h"
#include "kernel/tty/tty.h"

#define DEVFS_MAX_DEVICES 8

enum dev_type { DEV_NULL, DEV_ZERO, DEV_AUDIO, DEV_TTY };

struct devfs_device {
    char     name[32];
    uint32_t type;
};

struct devfs_state {
    struct devfs_device devices[DEVFS_MAX_DEVICES];
    struct vnode        vnodes[DEVFS_MAX_DEVICES];
    int count;
};

static struct devfs_state state;

/* Device identifiers (stable pointers for vnode.fs_data). */
static struct devfs_device dev_null  = { "/dev/null",  DEV_NULL  };
static struct devfs_device dev_zero  = { "/dev/zero",  DEV_ZERO  };
static struct devfs_device dev_audio = { "/dev/audio", DEV_AUDIO };
static struct devfs_device dev_tty0  = { "/dev/tty0",  DEV_TTY   };

void devfs_init(void) {
    state.count = 0;
    memset(state.vnodes, 0, sizeof(state.vnodes));

    /* /dev/null */
    strncpy(state.vnodes[state.count].name, "null", 31);
    state.vnodes[state.count].type    = VFS_TYPE_CHARDEV;
    state.vnodes[state.count].size    = 0;
    state.vnodes[state.count].ops     = &devfs_ops;
    state.vnodes[state.count].fs_data = &dev_null;
    state.count++;

    /* /dev/zero */
    strncpy(state.vnodes[state.count].name, "zero", 31);
    state.vnodes[state.count].type    = VFS_TYPE_CHARDEV;
    state.vnodes[state.count].size    = 0;
    state.vnodes[state.count].ops     = &devfs_ops;
    state.vnodes[state.count].fs_data = &dev_zero;
    state.count++;

    /* /dev/audio */
    strncpy(state.vnodes[state.count].name, "audio", 31);
    state.vnodes[state.count].type    = VFS_TYPE_CHARDEV;
    state.vnodes[state.count].size    = 0;
    state.vnodes[state.count].ops     = &devfs_ops;
    state.vnodes[state.count].fs_data = &dev_audio;
    state.count++;

    /* /dev/tty0 — the system console terminal. */
    strncpy(state.vnodes[state.count].name, "tty0", 31);
    state.vnodes[state.count].type    = VFS_TYPE_CHARDEV;
    state.vnodes[state.count].size    = 0;
    state.vnodes[state.count].ops     = &devfs_ops;
    state.vnodes[state.count].fs_data = &dev_tty0;
    state.count++;

    kprintf("[devfs] registered %d device(s): null, zero, audio, tty0\n",
            state.count);
}

static struct vnode *devfs_lookup(void *fs_data, const char *path) {
    (void)fs_data;
    /* Empty path = root of /dev → return any vnode marker (none means dir). */
    if (path[0] == '\0') {
        /* Synthesize a tiny stack-static dir vnode. */
        static struct vnode root_vn;
        root_vn.type = VFS_TYPE_DIR;
        root_vn.size = 0;
        root_vn.ops = &devfs_ops;
        return &root_vn;
    }
    for (int i = 0; i < state.count; i++) {
        if (strcmp(path, state.vnodes[i].name) == 0) {
            return &state.vnodes[i];
        }
    }
    return NULL;
}

static int devfs_readdir(struct vnode *vn, struct vfs_dirent *out, int max) {
    (void)vn;
    int n = 0;
    for (int i = 0; i < state.count && n < max; i++) {
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].name, state.vnodes[i].name, VFS_PATH_MAX - 1);
        out[n].type = state.vnodes[i].type;
        out[n].size = 0;
        out[n].inode = (uint64_t)(uintptr_t)&state.vnodes[i];
        n++;
    }
    return n;
}

static int64_t devfs_read(struct vnode *vn, uint64_t pos,
                          void *buf, uint64_t count) {
    (void)pos;
    struct devfs_device *d = (struct devfs_device *)vn->fs_data;
    switch (d->type) {
    case DEV_NULL:
        return 0;   /* EOF */
    case DEV_ZERO:
        memset(buf, 0, count);
        return (int64_t)count;
    case DEV_TTY:
        /* Drain whatever the line discipline has committed (may be 0; the
         * syscall layer's blocking loop re-polls + yields). */
        return tty_read_available(tty_console(), (char *)buf, (int)count);
    default:
        break;
    }
    return 0;
}

static int64_t devfs_write(struct vnode *vn, uint64_t pos,
                           const void *buf, uint64_t count) {
    (void)pos;
    struct devfs_device *d = (struct devfs_device *)vn->fs_data;
    if (d->type == DEV_TTY) {
        return tty_write(tty_console(), (const char *)buf, (int)count);
    }
    if (d->type == DEV_AUDIO) {
        const char *s = (const char *)buf;
        if (count > 5 && memcmp(s, "BEEP ", 5) == 0) {
            uint64_t freq = 0, dur = 0;
            const char *p = s + 5;
            while (*p >= '0' && *p <= '9') { freq = freq * 10 + (*p - '0'); p++; }
            while (*p == ' ' || *p == '\t') p++;
            while (*p >= '0' && *p <= '9') { dur = dur * 10 + (*p - '0'); p++; }
            audio_play_tone((uint32_t)freq, (uint32_t)dur);
        } else {
            audio_write_buffer((const uint8_t *)buf, (uint32_t)count);
        }
        return (int64_t)count;
    }
    return (int64_t)count;   /* discard data, report success */
}

static int devfs_ioctl(struct vnode *vn, unsigned long cmd, void *arg) {
    struct devfs_device *d = (struct devfs_device *)vn->fs_data;
    if (d->type == DEV_TTY) {
        return tty_ioctl(tty_console(), cmd, arg);
    }
    return -ENOTTY;   /* not a terminal */
}

const struct vfs_ops devfs_ops = {
    .lookup  = devfs_lookup,
    .read    = devfs_read,
    .write   = devfs_write,
    .readdir = devfs_readdir,
    .ioctl   = devfs_ioctl,
};
