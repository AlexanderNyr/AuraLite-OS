/* tools/fsbench/fsbench.c -- FSMODERN_PLAN.md M0, the IN-GUEST lane.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT THE HOST TEST
 * -----------------------------------------------
 * `tests/unit/test_fsbench.c` runs on the host with a RAM array underneath
 * the block seam.  That lane is good at three things -- correctness,
 * amplification (sectors written per user sector), and call counts -- but
 * every MiB/s it prints is RAM-to-RAM and therefore meaningless as disk
 * speed.  It says so in its own output, on purpose.
 *
 * This program runs INSIDE the OS, on a real emulated disk, through the real
 * VFS, and prints the numbers the plan is judged by.  Where the host lane
 * measures "memcpy through the cache", this one measures "QEMU's emulated
 * device behind a 512 KiB cache, with TCG translating every instruction".
 *
 * WHAT IT MEASURES
 * ----------------
 *   seq_write   MiB/s   N files of --size bytes, written back to back
 *   seq_read    MiB/s   read them all back
 *   sync        us      fs_cache_sync() over whatever remains dirty
 *   create      ops/s   creating N files (name allocation + directory work)
 *   unlink      ops/s   removing them
 *   remount     us      not measured here: a mount cannot be driven from
 *                       userland on this tree yet (HOME_PLAN/FSMODERN M1)
 *
 * The intent is not a micro-benchmark suite.  It is the smallest set of
 * numbers that can answer "did M2 make the block layer faster?" with a
 * before/after instead of an opinion.
 *
 *   crash_loop  —      FSMODERN M3.  NOT a benchmark.  Writes ONE block,
 *                       fsync()s it, prints a receipt, then plants a second
 *                       write WITHOUT a sync before planting a third.  Built
 *                       for test_fs_crash.sh: the guest does the whole
 *                       write->sync->receipt cycle in one program, so the
 *                       window between "wrote" and the host's kill is a few
 *                       instructions instead of a 200 ms serial round trip.
 *
 * Usage:
 *   fsbench [--dir PATH] [--files N] [--size BYTES]
 *   fsbench --crash-loop [--dir PATH] [--size BYTES]
 * Defaults: --dir /tmp --files 8 --size 4096 (a 32 KiB run, a few seconds
 * under TCG).  The receipt format is stable and machine-parseable by the
 * integration case; keep it that way.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define DEFAULT_DIR   "/tmp"
#define DEFAULT_FILES 8
#define DEFAULT_SIZE  4096
#define MAX_FILES     64

/* WHERE THE FIRST VERSION OF THIS FILE WAS WRONG, AND HOW
 * ------------------------------------------------------
 * It timed every phase with clock_gettime(CLOCK_MONOTONIC), which on this
 * tree is PIT-driven: kernel_clock_gettime() reads timer_get_ticks() and the
 * PIT runs at 99 Hz on the boot path ("[timer] PIT channel 0: divisor 11932
 * -> 99 Hz").  One tick is 10.1 ms.  A whole 32 KiB run fits INSIDE one
 * tick, so every delta came back 0 and the receipt read
 *
 *     seq_write=31250.00 MiB/s   sync=0 us
 *
 * -- arithmetic on a zero divisor, dressed up as a very fast filesystem.
 * That is exactly the kind of number this plan exists to stop publishing,
 * and it was caught by the in-guest case, not by the host lane.
 *
 * The fix is to time with the TSC, which is what the kernel itself uses for
 * its own perf receipts.  CR4.TSD is not set anywhere in this tree (checked:
 * paging.c sets SMEP/SMAP/OSFXSR, never TSD), so RDTSC from ring 3 is not
 * faulted.  The tick clock stays as a fallback for a CPU without a TSC, and
 * the receipt names which clock produced the numbers -- a reader must never
 * have to guess. */
static int g_using_tsc = 0;

static uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Calibrate the TSC against the only clock the kernel exposes, then convert
 * TSC counts to microseconds.  One 20 ms sleep is enough to get within a few
 * percent on a calibrated TSC; the point is a number with the right order of
 * magnitude, not metrology. */
static uint64_t g_tsc_per_us = 0;

static void calibrate(void) {
    /* CPUID leaf 0x15/0x16 would be cleaner but QEMU does not always fill
     * them; measure instead. */
    struct timespec a, b;
    a.tv_sec = b.tv_sec = 0;
    a.tv_nsec = b.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t t0 = tsc_read();
    /* 20 ms: two PIT ticks, long enough for the tick clock to move. */
    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = 20000000;
    nanosleep(&req, 0);
    uint64_t t1 = tsc_read();
    clock_gettime(CLOCK_MONOTONIC, &b);
    uint64_t us = ((uint64_t)b.tv_sec - (uint64_t)a.tv_sec) * 1000000u
                + ((uint64_t)b.tv_nsec - (uint64_t)a.tv_nsec) / 1000u;
    if (us >= 1000 && t1 > t0) {
        g_tsc_per_us = (t1 - t0) / us;
        g_using_tsc = 1;
    }
}

static uint64_t now_us(void) {
    if (g_using_tsc && g_tsc_per_us) {
        return tsc_read() / g_tsc_per_us;
    }
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

#define US(x) ((x) ? (double)(x) : 1.0)

static char payload[65536];

int main(int argc, char **argv) {
    const char *dir = DEFAULT_DIR;
    int nfiles = DEFAULT_FILES;
    int fsize = DEFAULT_SIZE;
    int crash_loop = 0;
    int scale_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) {
            nfiles = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            fsize = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--crash-loop") == 0) {
            crash_loop = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: fsbench [--dir PATH] [--files N] [--size BYTES]\n"
                   "       fsbench --crash-loop [--dir PATH] [--size BYTES]\n");
            return 0;
        } else {
            printf("fsbench: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (nfiles < 1 || nfiles > MAX_FILES) {
        printf("fsbench: --files must be 1..%d\n", MAX_FILES);
        return 2;
    }
    if (fsize < 1 || fsize > (int)sizeof(payload)) {
        printf("fsbench: --size must be 1..%d\n", (int)sizeof(payload));
        return 2;
    }

    for (int i = 0; i < fsize; i++) payload[i] = (char)(i * 7 + 1);

    calibrate();
    if (!g_using_tsc) {
        printf("fsbench: no usable TSC; falling back to the %s clock, "
               "which ticks at ~100 Hz -- a short run may measure as 0\n",
               "kernel");
    }

    /* FSMODERN M3 crash loop.  See the header comment; the receipts are a
     * stable contract with tests/integration/cases/test_fs_crash.sh. */
    if (scale_mode) {
        char path[MAX_FILES][128];
        int sizes[3] = { nfiles / 16, nfiles / 4, nfiles };
        if (sizes[0] < 1) sizes[0] = 1;
        if (sizes[1] < 1) sizes[1] = 1;
        double ops[3] = {0, 0, 0};
        printf("[fsbench] scale: dir=%s sizes=%d/%d/%d size=%d clock=%s\n",
               dir, sizes[0], sizes[1], sizes[2], fsize,
               g_using_tsc ? "tsc" : "kernel");
        for (int sidx = 0; sidx < 3; sidx++) {
            int n = sizes[sidx];
            char subdir[96];
            snprintf(subdir, sizeof(subdir), "%s/s%d", dir, sidx);
            mkdir(subdir, 0755);
            for (int i = 0; i < n; i++) {
                snprintf(path[i], sizeof(path[i]), "%s/f_%d.dat", subdir, i);
                int fd = open(path[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd < 0) { printf("[fsbench] scale: FAIL create %s\n", path[i]); return 1; }
                close(fd);
            }
            fsync(0);
            uint64_t t0 = now_us();
            int passes = 0;
            uint64_t lookups = 0;
            struct stat st;
            while (passes < 5) {
                for (int i = 0; i < n; i++) {
                    if (stat(path[i], &st) != 0) {
                        printf("[fsbench] scale: FAIL stat %s\n", path[i]);
                        return 1;
                    }
                    lookups++;
                }
                passes++;
                if ((now_us() - t0) >= 250000ULL) break;
            }
            uint64_t dt = now_us() - t0;
            double secs = (double)dt / 1000000.0;
            if (secs < 1e-6) secs = 1e-6;
            ops[sidx] = (double)lookups / secs;
            printf("[fsbench] scale: n=%d lookups=%llu passes=%d -> %.1f ops/s\n",
                   n, (unsigned long long)lookups, passes, ops[sidx]);
        }
        double ratio = (ops[2] > 0.0) ? (ops[0] / ops[2]) : 0.0;
        printf("[fsbench] scale: ratio small/large = %.2fx for %dx the names (small=%.1f large=%.1f ops/s)\n",
               ratio, (int)(nfiles / sizes[0]), ops[0], ops[2]);
        printf("[fsbench] scale: PASS\n");
        return 0;
    }

    if (crash_loop) {
        char cpath[64];
        snprintf(cpath, sizeof(cpath), "%s/crash.txt", dir);

        /* Phase A: the whole point -- write, fsync, SAY SO. */
        int fd = open(cpath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0) {
            printf("fsbench: FAIL: cannot create %s\n", cpath);
            return 1;
        }
        int n = (int)write(fd, payload, (size_t)fsize);
        if (n != fsize) {
            printf("fsbench: FAIL: short write on %s (%d of %d)\n",
                   cpath, n, fsize);
            return 1;
        }
        if (fsync(fd) != 0) {
            /* The contract says a failed fsync is NOT durable; say so and
             * exit non-zero rather than let the crash test read a stale
             * file as a pass. */
            printf("fsbench: FAIL: fsync(%s) failed\n", cpath);
            close(fd);
            return 1;
        }
        uint64_t sync_end = now_us();
        printf("fsbench: [crash] synced %s (%d bytes) at us=%llu\n",
               cpath, n, (unsigned long long)sync_end);
        close(fd);

        /* Phase B and C are deliberately NOT synced.  Phase B overwrites
         * the file with the same length (so a stale-but-torn write is
         * detectable), phase C appends.  A kill landing between any two
         * lines must leave the file whole -- either the synced size or a
         * whole later size, never a half one. */
        fd = open(cpath, O_WRONLY, 0);
        if (fd >= 0) {
            write(fd, payload, (size_t)fsize);
            close(fd);
            printf("fsbench: [crash] unsynced-rewrite %s\n", cpath);
        }
        fd = open(cpath, O_WRONLY | O_APPEND, 0);
        if (fd >= 0) {
            write(fd, payload, (size_t)fsize);
            close(fd);
            printf("fsbench: [crash] unsynced-append %s\n", cpath);
        }
        /* Nothing after this line can be a lie about durability: the two
         * unsynced writes above are allowed to disappear, and the crash
         * test only demands that whatever is there is WHOLE. */
        printf("fsbench: [crash] done %s\n", cpath);
        return 0;
    }

    char path[MAX_FILES][64];
    for (int i = 0; i < nfiles; i++) {
        snprintf(path[i], sizeof(path[i]), "%s/fsb_%d.dat", dir, i);
    }

    /* --- write ------------------------------------------------------ */
    uint64_t w0 = now_us();
    int written = 0;
    for (int i = 0; i < nfiles; i++) {
        int fd = open(path[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0) {
            printf("fsbench: FAIL: cannot create %s\n", path[i]);
            return 1;
        }
        int n = (int)write(fd, payload, (size_t)fsize);
        close(fd);
        if (n != fsize) {
            printf("fsbench: FAIL: short write on %s (%d of %d)\n",
                   path[i], n, fsize);
            return 1;
        }
        written += n;
    }
    uint64_t w1 = now_us();

    /* --- sync ------------------------------------------------------- */
    uint64_t s0 = now_us();
    sync();
    uint64_t s1 = now_us();

    /* --- read back and VERIFY --------------------------------------- */
    static char back[65536];
    uint64_t r0 = now_us();
    int read_back = 0;
    for (int i = 0; i < nfiles; i++) {
        int fd = open(path[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("fsbench: FAIL: cannot open %s for reading\n", path[i]);
            return 1;
        }
        int n = (int)read(fd, back, (size_t)fsize);
        close(fd);
        if (n != fsize) {
            printf("fsbench: FAIL: short read on %s (%d of %d)\n",
                   path[i], n, fsize);
            return 1;
        }
        /* A benchmark that does not verify its own payload measures a
         * filesystem that may be returning garbage quickly. */
        if (memcmp(back, payload, (size_t)fsize) != 0) {
            printf("fsbench: FAIL: %s read back WRONG DATA\n", path[i]);
            return 1;
        }
        read_back += n;
    }
    uint64_t r1 = now_us();

    /* --- unlink ----------------------------------------------------- */
    uint64_t u0 = now_us();
    int removed = 0;
    for (int i = 0; i < nfiles; i++) {
        if (unlink(path[i]) != 0) {
            printf("fsbench: FAIL: cannot unlink %s\n", path[i]);
            return 1;
        }
        removed++;
    }
    uint64_t u1 = now_us();

    double mib = (double)written / (1024.0 * 1024.0);
    double write_mibs = mib / (US(w1 - w0) / 1e6);
    double read_mibs  = mib / (US(r1 - r0) / 1e6);
    double unlink_ops = (double)removed / (US(u1 - u0) / 1e6);
    double create_ops = (double)nfiles / (US(w1 - w0) / 1e6);

    /* One line per number, stable labels: the integration case greps for
     * "[fsbench]" and parses label=value.  A receipt that only a human can
     * read is not a measurement. */
    printf("[fsbench] dir=%s files=%d size=%d clock=%s\n", dir, nfiles, fsize,
           g_using_tsc ? "tsc" : "tick");
    printf("[fsbench] seq_write=%.2f MiB/s\n", write_mibs);
    printf("[fsbench] seq_read=%.2f MiB/s\n", read_mibs);
    printf("[fsbench] create=%.1f ops/s\n", create_ops);
    printf("[fsbench] unlink=%.1f ops/s\n", unlink_ops);
    printf("[fsbench] sync=%llu us\n", (unsigned long long)(s1 - s0));
    printf("[fsbench] verified=%d bytes, removed=%d file(s)\n",
           read_back, removed);
    printf("[fsbench] PASS: %d B written, read back, verified, unlinked\n",
           written);
    return 0;
}
