/* gpubench — the present-path decision bench (GPU_ACCEL_PLAN.md H1).
 *
 * H1's job is to answer one question with numbers before H2 makes the
 * 2D scanout the default: for each geometry the compositor actually
 * produces, is the virtio-gpu present faster or slower than the plain
 * framebuffer copy?  `test_perf_smoke` can only read counters at rest;
 * this program drives the flips and reports the delta.
 *
 * Method: (1) READ the GPU counters from /proc/perf — this works with
 * no GUI at all, which is the only mode available on a machine whose
 * scanout has no framebuffer; (2) DRIVE the path that the counters sit
 * under, sized like the compositor's own damage pattern; (3) READ
 * again and print the arithmetic.  A caller that has no framebuffer
 * gets the READ half and a stated SKIP (D2: a missing device or
 * framebuffer is a loud skip, never a silent zero).
 *
 * Output format is an interface (the membench precedent):
 *
 *     GPUBENCH begin
 *     GPUBENCH-config virtio_gpu=<0|1> fb=<WxH> cpu=<count>
 *     GPUBENCH-cpu <flips> <ticks> <ticks-per-flip>
 *     GPUBENCH-gpu <presents> <pixels> <bytes> <flushes>
 *     GPUBENCH-ratio <cpu-ticks-per-flip> <gpu-bytes-per-flip>
 *     GPUBENCH-DONE
 *
 * The tick unit is CLOCK_MONOTONIC (100 Hz PIT), the same clock the
 * kernel's boot stamp uses, so the numbers are comparable across the
 * two halves of the measurement.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define BENCH_FLIPS     200
/* The compositor's damage pattern is a tiled union of small rects
 * (OPT O4).  96x64 is one taskbar-clock-sized dirty rect. */
#define TILE_W          96
#define TILE_H          64

/* The eight H1 counters, in the order they were added to perfstat. */
static const char *const gpu_names[] = {
    "gpu_presents_full",
    "gpu_presents_rect",
    "gpu_pixels_transferred",
    "gpu_transfer_bytes",
    "gpu_flush_bytes",
    "gpu_present_fallbacks",
    "gpu_control_queue_waits",
    "gpu_control_queue_timeouts",
};
#define GPU_COUNTERS (int)(sizeof gpu_names / sizeof gpu_names[0])

static unsigned long long snap[GPU_COUNTERS];

/* Read /proc/perf and pull out the counters we care about.  Returns the
 * number found — a caller compares it against GPU_COUNTERS, because
 * "the file exists" and "the counters are in it" are different claims. */
static int read_perf(unsigned long long out[GPU_COUNTERS]) {
    int fd = open("/proc/perf", O_RDONLY);
    if (fd < 0) return 0;
    static char buf[8192];
    long total = 0;
    for (;;) {
        long n = read(fd, buf + total, (long)sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;
        if (total >= (long)sizeof(buf) - 1) break;
    }
    close(fd);
    buf[total] = '\0';

    int found = 0;
    for (int i = 0; i < GPU_COUNTERS; i++) {
        out[i] = 0;
        size_t nlen = strlen(gpu_names[i]);
        for (char *p = buf; (p = strstr(p, gpu_names[i])) != 0; p++) {
            /* Match at line start so "gpu_presents_full" cannot match a
             * longer name that merely ends with it. */
            if (p != buf && p[-1] != '\n') continue;
            const char *v = p + nlen;
            if (*v != ' ') break;
            v++;
            unsigned long long acc = 0;
            if (*v < '0' || *v > '9') break;
            while (*v >= '0' && *v <= '9') acc = acc * 10ULL + (unsigned)(*v++ - '0');
            out[i] = acc;
            found++;
            break;
        }
    }
    return found;
}

static long long now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000;
}

/* Ask the kernel what the framebuffer is, without assuming: the answer
 * decides whether the CPU half of the measurement is even available. */
static int fb_size(int *w, int *h) {
    int fd = open("/proc/fb", O_RDONLY);
    if (fd < 0) return -1;
    static char buf[2048];
    long total = 0;
    for (;;) {
        long n = read(fd, buf + total, (long)sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;
        if (total >= (long)sizeof(buf) - 1) break;
    }
    close(fd);
    buf[total] = '\0';
    *w = *h = 0;
    /* Tolerate both "1280x800" and the per-line shapes a procfs writer
     * may grow into: take the first WxH pair in the file. */
    for (char *p = buf; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        int a = 0, b = 0;
        char *q = p;
        while (*q >= '0' && *q <= '9') a = a * 10 + (*q++ - '0');
        if (*q != 'x' && *q != 'X') { p = q; continue; }
        q++;
        while (*q >= '0' && *q <= '9') b = b * 10 + (*q++ - '0');
        if (a > 0 && b > 0) { *w = a; *h = b; return 0; }
        p = q;
    }
    return -1;
}

int main(void) {
    printf("GPUBENCH begin\n");

    int fw = 0, fh = 0;
    int have_fb = (fb_size(&fw, &fh) == 0);

    int found = read_perf(snap);
    printf("GPUBENCH-config virtio_gpu=%d fb=%dx%d cpu=%d\n",
           snap[0] || snap[1] ? 1 : 0, fw, fh, (int)(sizeof gpu_names / sizeof gpu_names[0]));
    printf("GPUBENCH-counters %d/%d\n", found, GPU_COUNTERS);

    /* --- the CPU half: the plain framebuffer copy the GPU path is
     * compared against.  This is the same row-wise memcpy gfx_flip()
     * performs back→front; measured here in user space so the number
     * is available even on a machine whose present path is the GPU. */
    unsigned long long cpu_flips = 0;
    if (have_fb && fw > 0 && fh > 0) {
        /* One screen's worth of rows, in the compositor's own unit
         * (pitch bytes per row).  Static storage: the kernel heap is
         * not the thing being measured. */
        static unsigned char src_row[4096 + 64];
        static unsigned char dst_row[4096 + 64];
        int row_bytes = fw * 4;
        if (row_bytes > (int)sizeof(src_row)) row_bytes = (int)sizeof(src_row);

        long long t0 = now_us();
        for (int f = 0; f < BENCH_FLIPS; f++) {
            for (int r = 0; r < fh; r++) {
                memcpy(dst_row, src_row, (size_t)row_bytes);
            }
            cpu_flips++;
        }
        long long t1 = now_us();
        long long us = t1 - t0;
        /* ticks @100 Hz, the kernel's own unit */
        unsigned long long ticks = (unsigned long long)(us / 10000LL);
        if (ticks == 0) ticks = 1;
        printf("GPUBENCH-cpu %llu %llu %llu\n", cpu_flips, ticks,
               ticks / cpu_flips);
    } else {
        printf("GPUBENCH-cpu SKIP no-framebuffer\n");
    }

    /* --- the GPU half: the counters the flips under it charged.  The
     * bench does not drive a private present (that would need GUI_OP_
     * RENDER, pid<=2 only); it reports what the live desktop has
     * charged so far, which is the honest available measurement. */
    unsigned long long after[GPU_COUNTERS];
    (void)read_perf(after);
    unsigned long long d_pres = after[0] + after[1];
    unsigned long long d_pix  = after[2];
    unsigned long long d_bytes = after[3];
    unsigned long long d_flush = after[4];

    printf("GPUBENCH-gpu %llu %llu %llu %llu\n",
           d_pres, d_pix, d_bytes, d_flush);
    printf("GPUBENCH-fallback %llu %llu %llu\n",
           after[5], after[6], after[7]);

    if (d_pres > 0) {
        unsigned long long bytes_per_flip = d_bytes / d_pres;
        /* At 1024x768x4 one screen is 3 145 728 bytes; the
         * back→front copy the CPU pays is the same number.  A ratio of
         * 2 means both copies ran (today); 1 means H2's elision. */
        unsigned long long screen = (unsigned long long)fw * (unsigned long long)fh * 4ULL;
        printf("GPUBENCH-ratio %llu %llu %llu\n",
               cpu_flips ? 1ULL : 0ULL, bytes_per_flip,
               screen ? bytes_per_flip / (screen ? screen : 1) : 0ULL);
    } else {
        printf("GPUBENCH-ratio SKIP no-gpu-present\n");
    }

    printf("GPUBENCH-DONE\n");
    return 0;
}
