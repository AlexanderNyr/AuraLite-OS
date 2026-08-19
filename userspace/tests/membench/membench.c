/* membench — the copy-engine microbenchmark (OPT_PLAN.md O0).
 *
 * The gate tooling for phase O1: measures libc memcpy/memset throughput
 * at the sizes that matter (small = call overhead, page = COW unit,
 * screen-row-ish = compositor unit, 1 MiB = spawn's bounce buffer) and
 * prints a fixed-format table the integration case and the plan's §6
 * table read.  The format is an interface:
 *
 *     MEMBENCH begin
 *     MEMBENCH <name> <bytes> <MB/s>
 *     ...
 *     MEMBENCH-DONE
 *
 * Method: repeat the operation over a static buffer until at least
 * MIN_MS of CLOCK_MONOTONIC time has elapsed (the PIT gives 10 ms
 * resolution, so short runs would quantise to garbage), then report
 * bytes/elapsed.  Misaligned variants offset src by 1 and dst by 3 —
 * the worst case for word-at-a-time loops, free for rep movsb.
 *
 * D2 (OPT_PLAN) applies: these numbers are recorded and human-read;
 * nothing gates hard on them under TCG.  The only hard assertion the
 * integration case makes is that this program runs to MEMBENCH-DONE.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MIN_MS      200
#define BUF_MAX     (1024 * 1024)

/* +64 so the misaligned variants stay in bounds. */
static unsigned char src[BUF_MAX + 64];
static unsigned char dst[BUF_MAX + 64];

static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

typedef void (*op_fn)(unsigned char *d, unsigned char *s, size_t n);

static void op_memcpy(unsigned char *d, unsigned char *s, size_t n)  { memcpy(d, s, n); }
static void op_memset(unsigned char *d, unsigned char *s, size_t n)  { (void)s; memset(d, 0x5a, n); }
static void op_memmove(unsigned char *d, unsigned char *s, size_t n) { memmove(d, s, n); }

static void bench(const char *name, op_fn op, size_t n,
                  unsigned d_off, unsigned s_off) {
    long t0 = now_ms();
    if (t0 < 0) { printf("MEMBENCH %s %u SKIP-noclock\n", name, (unsigned)n); return; }

    unsigned long long bytes = 0;
    long t1 = t0;
    /* At least one iteration, then run until MIN_MS has really elapsed. */
    do {
        op(dst + d_off, src + s_off, n);
        bytes += n;
        t1 = now_ms();
    } while (t1 - t0 < MIN_MS);

    long ms = t1 - t0;
    if (ms <= 0) ms = 1;
    /* MB/s with no floats: bytes * 1000 / ms / 1e6. */
    unsigned long long mbps = bytes * 1000ULL / (unsigned long long)ms / 1000000ULL;
    printf("MEMBENCH %s %u %llu\n", name, (unsigned)n, mbps);
}

int main(void) {
    /* Touch both buffers so BSS pages are resident before timing. */
    memset(src, 0xa5, sizeof(src));
    memset(dst, 0x00, sizeof(dst));

    printf("MEMBENCH begin\n");

    static const size_t sizes[] = { 64, 4096, 65536, BUF_MAX };
    for (int i = 0; i < 4; i++) {
        bench("memcpy-a",  op_memcpy,  sizes[i], 0, 0);
        bench("memcpy-u",  op_memcpy,  sizes[i], 3, 1);
    }
    bench("memset-a",  op_memset,  BUF_MAX, 0, 0);
    bench("memmove-o", op_memmove, 65536,   0, 8);   /* overlapping-ish */

    printf("MEMBENCH-DONE\n");
    return 0;
}
