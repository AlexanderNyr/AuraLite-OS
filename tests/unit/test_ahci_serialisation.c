/*
 * test_ahci_serialisation.c — A2-R1 regression test (host-side).
 *
 * A2-R1: `cat /disk/persist.txt` intermittently returned the raw bytes
 * "AURALOG TXT ..." -- a FAT32 *directory* sector -- instead of the file.
 * Roughly one run in three.
 *
 * The cause was not in FAT32.  drivers/ahci/ahci.c issues every transfer
 * through ONE command slot and ONE DMA bounce buffer per port, and had no
 * lock at all.  Eight subsystems call into it (fat32, diskfs, ext2, ext4,
 * btrfs, f2fs, buffer_cache, /disk).  fat32_lock serialises FAT32 against
 * FAT32, but nothing stopped the BSP reading /disk through diskfs while an
 * AP flushed the kernel log to /fat/AURALOG.TXT.  Both land in ahci_exec()
 * on the same port: the second overwrites the first's command header and
 * PRDT while the first is still polling PxCI, and one request's data is
 * delivered into the other's buffer.
 *
 * This test models that shared state and asserts the invariant the driver
 * must uphold: a transfer's bounce buffer still holds ITS OWN data when the
 * transfer completes.  Run unlocked, it fails; run locked, it passes.
 *
 * It is deliberately a host-side model rather than a QEMU case: the real
 * race needs two CPUs hitting one port at the right instant, which is why
 * A2-R1 only showed up in one run out of three.  A model reproduces it
 * every time.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#define PORTS        1
#define ITERATIONS   2000
#define THREADS      4

/* The shared per-port state, mirroring struct ahci_port. */
struct fake_port {
    unsigned char dma_buf[64];   /* the single bounce buffer */
    int           cmd_slot_busy; /* the single command slot */
};

static struct fake_port ports[PORTS];
static pthread_mutex_t  port_lock[PORTS];
static int              use_lock = 1;
static volatile int     corruption_seen = 0;

/*
 * One transfer: stamp the buffer with this caller's id, "wait" for the
 * device, then read the buffer back.  If another thread was allowed in
 * between, the bytes belong to somebody else.
 */
static void *do_transfer(void *arg) {
    long id = (long)arg;
    unsigned char mine = (unsigned char)('A' + id);

    for (int i = 0; i < ITERATIONS; i++) {
        if (use_lock) pthread_mutex_lock(&port_lock[0]);

        /* Fill the bounce buffer a byte at a time, yielding in the middle.
         * The real driver's window is the whole PxCI poll -- microseconds
         * of DMA -- but a tight memset() here is so fast that an unlocked
         * run can finish before any other thread is scheduled, which made
         * the first version of this control pass by luck and prove
         * nothing. */
        for (unsigned k = 0; k < sizeof(ports[0].dma_buf); k++) {
            ports[0].dma_buf[k] = mine;
            if (k == sizeof(ports[0].dma_buf) / 2) sched_yield();
        }
        ports[0].cmd_slot_busy = 1;

        sched_yield();

        for (unsigned k = 0; k < sizeof(ports[0].dma_buf); k++) {
            if (ports[0].dma_buf[k] != mine) {
                corruption_seen = 1;
                break;
            }
        }
        ports[0].cmd_slot_busy = 0;

        if (use_lock) pthread_mutex_unlock(&port_lock[0]);
    }
    return NULL;
}

static int run_case(const char *label, int locked, int expect_corruption) {
    pthread_t t[THREADS];

    use_lock = locked;
    corruption_seen = 0;
    memset(ports, 0, sizeof(ports));
    pthread_mutex_init(&port_lock[0], NULL);

    for (long i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, do_transfer, (void *)i);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);

    int ok = expect_corruption ? corruption_seen : !corruption_seen;
    printf("  %s %s (corruption %sobserved)\n",
           ok ? "PASS:" : "FAIL:", label, corruption_seen ? "" : "not ");
    return ok;
}

int main(void) {
    int ok = 1;
    printf("test_ahci_serialisation (A2-R1)\n");

    /* The control: without the lock the race must actually happen, or this
     * test is not exercising anything and its PASS below means nothing. */
    ok &= run_case("unlocked shared DMA buffer is corrupted (control)",
                   0, 1);

    /* The property the fix must provide. */
    ok &= run_case("per-port lock keeps each transfer's buffer intact",
                   1, 0);

    printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
