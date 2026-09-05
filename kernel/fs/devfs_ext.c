/* devfs_ext.c — the x86_64-only half of /dev (RESIDUE2 T3, RES-07).
 *
 * RES-07's closing shape splits devfs into a portable core (devfs.c:
 * null + zero + the registration hook, linked on all four archs) and
 * this module, which registers the devices that only exist on the
 * x86_64 tenant: the system console tty and the audio/PC-speaker sink.
 * The ports never link this file, so their /dev stays null + zero —
 * exactly the reduced-but-honest surface PARITY P2 asked for.
 */

#include <stdint.h>
#include "kernel/fs/devfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/audio/audio.h"
#include "kernel/tty/tty.h"

/* ---- /dev/tty0: the console terminal ---- */

static int64_t tty0_read(struct vnode *vn, uint64_t pos, void *buf,
                         uint64_t count) {
    (void)vn;
    (void)pos;
    /* Drain whatever the line discipline has committed (may be 0; the
     * syscall layer's blocking loop re-polls + yields). */
    return tty_read_available(tty_console(), (char *)buf, (int)count);
}

static int64_t tty0_write(struct vnode *vn, uint64_t pos, const void *buf,
                          uint64_t count) {
    (void)vn;
    (void)pos;
    return tty_write(tty_console(), (const char *)buf, (int)count);
}

static int tty0_ioctl(struct vnode *vn, unsigned long cmd, void *arg) {
    (void)vn;
    return tty_ioctl(tty_console(), cmd, arg);
}

/* ---- /dev/audio: the PC speaker / audio buffer sink ---- */

static int64_t audio_write(struct vnode *vn, uint64_t pos, const void *buf,
                           uint64_t count) {
    (void)vn;
    (void)pos;
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

/* Called from kernel.c right after devfs_init(), before /dev mounts. */
void devfs_ext_init(void) {
    devfs_register_ext("tty0", tty0_read, tty0_write, tty0_ioctl);
    devfs_register_ext("audio", 0, audio_write, 0);
    kprintf("[devfs] registered x86_64 device(s): tty0, audio\n");
}
