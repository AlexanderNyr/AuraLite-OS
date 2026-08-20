/* kernel/rng.c — kernel CSPRNG (INTERNET_PLAN.md phase N0).
 *
 * Replaces the Q16 seeded xorshift128+ pool.  The generation core is the
 * ChaCha20 DRBG in kernel/rng_core.h; this file is responsible for the
 * ENTROPY feeding it, which is the part N0 exists for (decision D1: no
 * cryptographic code may stand on a guessable seed).
 *
 * Sources, in preference order:
 *
 *   1. RDSEED  — CPUID leaf 7 EBX bit 18.  A wide hardware entropy source;
 *                384 bits (48 bytes) are collected with a bounded retry,
 *                since RDSEED is allowed to return CF=0 when its pool is
 *                drained.
 *   2. RDRAND  — CPUID leaf 1 ECX bit 30.  Conditioned hardware source;
 *                used when RDSEED is absent or starved.
 *   3. Interrupt-timing jitter pool — the fallback.  irq_dispatch() calls
 *                rng_jitter_event() on every IRQ; the low bits of the TSC
 *                DELTA between consecutive interrupts are accumulated into
 *                a pool and an estimate of observed variation is counted.
 *                The DRBG is seeded only once that estimate reaches
 *                RNG_POOL_BITS, and until then rng_try_fill() returns
 *                -ENOSYS.  The estimate is logged, never silently assumed.
 *
 * Reseeding: every RNG_RESEED_BYTES of output the state is re-seeded from
 * the best source currently available.  The DRBG additionally re-keys from
 * its own stream after every served request (backtracking resistance),
 * which rng_core.h implements.
 *
 * SMP/IRQ notes: the jitter pool is written lock-free from IRQ context on
 * any CPU — a torn or lost mix only ever loses entropy, never creates a
 * correctness dependency.  DRBG state is guarded by an irqsave spinlock.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/rng.h"
#include "kernel/rng_core.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/selftest.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/lib/string.h"
#include "kernel/lib/errno.h"
#include "drivers/timer/pit.h"

#define RNG_RESEED_BYTES (1u << 20)   /* re-seed every 1 MiB of output */
#define RNG_HW_RETRIES   4096         /* bounded retry per RDSEED/RDRAND word */
#define RNG_SELFTEST_LEN 16384        /* boot self-test size in bytes */

static struct rngc_drbg drbg;
static spinlock_t rng_lock = SPINLOCK_UNLOCKED;

static volatile int rng_module_up = 0;  /* rng_init() has run */

/* OPT_PLAN.md O7: getrandom() blocks here until seeding completes
 * (woken from every rng_ready=1 site, IRQ context included — the O4
 * wait_queue irqsave fix is what makes that legal). */
struct wait_queue rng_ready_wq;
static volatile int rng_ready     = 0;  /* DRBG seeded and serving */
static int have_rdrand = 0;
static int have_rdseed = 0;
static uint64_t bytes_since_reseed = 0;

/* ---- interrupt-timing jitter pool ---- */

#define JPOOL_WORDS 8
static volatile uint64_t jpool[JPOOL_WORDS];
static volatile uint32_t jsamples = 0;
static volatile uint32_t jbits    = 0;   /* observed-variation estimate */
static volatile uint64_t last_irq_tsc   = 0;
static volatile uint64_t last_irq_delta = 0;

static inline uint64_t rotl64(uint64_t v, int n) {
    return (v << n) | (v >> (64 - n));
}

static inline int popcnt16(uint32_t v) {
    int c = 0;
    v &= 0xFFFFu;
    while (v) { c += (int)(v & 1u); v >>= 1; }
    return c;
}

/* ---- hardware sources ---- */

static int hw_rdseed64(uint64_t *out) {
    unsigned char ok = 0;
    __asm__ volatile ("rdseed %0; setc %1" : "=r"(*out), "=qm"(ok));
    return ok ? 1 : 0;
}

static int hw_rdrand64(uint64_t *out) {
    unsigned char ok = 0;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));
    return ok ? 1 : 0;
}

/* Collect 48 bytes from a hardware source.  Returns 1 on success. */
static int collect_hw(int (*src)(uint64_t *), uint8_t material[RNGC_SEED_LEN]) {
    size_t filled = 0;
    while (filled < RNGC_SEED_LEN) {
        uint64_t w = 0;
        int ok = 0;
        for (int attempt = 0; attempt < RNG_HW_RETRIES; attempt++) {
            if (src(&w)) { ok = 1; break; }
        }
        if (!ok) return 0;               /* source starved: bail out */
        for (int b = 0; b < 8 && filled < RNGC_SEED_LEN; b++) {
            material[filled++] = (uint8_t)(w >> (b * 8));
        }
    }
    return 1;
}

/* Snapshot of the jitter pool + cheap context noise, used when no hardware
 * source is available (initial seed and periodic reseeds).  This is the
 * weak path; the boot log says so. */
static void snapshot_pool(uint8_t material[RNGC_SEED_LEN]) {
    for (int i = 0; i < JPOOL_WORDS / 2; i++) {
        uint64_t w = jpool[i] ^ jpool[i + JPOOL_WORDS / 2];
        for (int b = 0; b < 8; b++) material[i * 8 + b] = (uint8_t)(w >> (b * 8));
    }
    uint64_t ctx = read_tsc()
                 ^ ((uint64_t)timer_get_ticks() * 0x9E3779B97F4A7C15ULL)
                 ^ (uint64_t)(uintptr_t)material
                 ^ ((uint64_t)jsamples << 32);
    for (int b = 0; b < 8; b++) material[32 + b] = (uint8_t)(ctx >> (b * 8));
    uint64_t ctx2 = rotl64(ctx, 17) ^ 0xBF58476D1CE4E5B9ULL;
    for (int b = 0; b < 8; b++) material[40 + b] = (uint8_t)(ctx2 >> (b * 8));
}

/* ---- seeding ---- */

/* Seed the DRBG from the jitter pool snapshot.  Caller holds rng_lock. */
static void seed_from_pool_locked(void) {
    uint8_t material[RNGC_SEED_LEN];
    snapshot_pool(material);
    rngc_drbg_seed(&drbg, material);
    memset(material, 0, sizeof(material));
    bytes_since_reseed = 0;
    rng_ready = 1;
    wq_wake_all(&rng_ready_wq);                       /* O7 */
    kprintf("[rng] seeded from interrupt-jitter pool (%u samples, est. %u bits)\n",
            jsamples, jbits);
}

/* Try to collect and install hardware entropy.  Caller holds rng_lock.
 * Returns 1 when a hardware seed was installed. */
static int try_seed_from_hw_locked(void) {
    uint8_t material[RNGC_SEED_LEN];

    if (have_rdseed && collect_hw(hw_rdseed64, material)) {
        rngc_drbg_seed(&drbg, material);
        memset(material, 0, sizeof(material));
        bytes_since_reseed = 0;
        rng_ready = 1;
        wq_wake_all(&rng_ready_wq);                   /* O7 */
        kprintf("[rng] seeded from RDSEED (%d bits of hardware entropy)\n",
                RNGC_SEED_LEN * 8);
        return 1;
    }
    if (have_rdrand && collect_hw(hw_rdrand64, material)) {
        rngc_drbg_seed(&drbg, material);
        memset(material, 0, sizeof(material));
        bytes_since_reseed = 0;
        rng_ready = 1;
        wq_wake_all(&rng_ready_wq);                   /* O7 */
        kprintf("[rng] seeded from RDRAND (%d bits of hardware entropy)\n",
                RNGC_SEED_LEN * 8);
        return 1;
    }
    return 0;
}

/* ---- boot self-test: catch the obvious failures loudly ---- */

/* Byte-frequency and bit-run checks over RNG_SELFTEST_LEN bytes.  These do
 * not claim cryptographic quality; they catch stuck generators, counters
 * and badly collapsed seeds. */
static int rng_self_test(void) {
    static uint8_t buf[RNG_SELFTEST_LEN];
    static uint32_t freq[256];

    /* OPT_PLAN.md O2: FULL analyses the historical 16 KiB; FAST proves
     * the same stuck-generator/counter failures over 2 KiB (expected
     * count per byte value drops 64 -> 8; the +/-50%% band still
     * catches a collapsed seed by orders of magnitude); OFF skips
     * loudly.  Seeding itself is never skipped -- this knob trades
     * boot-time statistics, not entropy. */
    size_t len = (size_t)selftest_scale(RNG_SELFTEST_LEN, 2048);
    if (len == 0) {
        kprintf("[rng] self-test: SKIPPED (selftest=off)\n");
        return 1;
    }

    /* Generate WITHOUT the backtracking re-key noise mattering: just fill. */
    rngc_drbg_fill(&drbg, buf, len);

    for (int i = 0; i < 256; i++) freq[i] = 0;
    for (size_t i = 0; i < len; i++) freq[buf[i]]++;

    uint32_t expected = (uint32_t)(len / 256);
    /* Band calibration is a Poisson question, and the historical +/-50%
     * band is only sound at the historical size: at FULL (len 16 KiB)
     * expected=64 and 32..96 sits ~4 sigma out.  At FAST (2 KiB)
     * expected=8, and P(count > 12 or < 4) is ~6% PER BUCKET -- across
     * 256 buckets an ordinary healthy boot fails almost surely (measured:
     * "FAIL (byte 0x04 count 14, expected ~8)" on the first fast boot).
     * So FAST keeps only an upper bound at 4x expected (P ~ 1e-10 per
     * bucket): a stuck generator still pierces it by orders of magnitude,
     * and a counter generator is the bit-runs test's catch, not this
     * one's. */
    uint32_t lo = (len == RNG_SELFTEST_LEN) ? expected / 2 : 0;
    uint32_t hi = (len == RNG_SELFTEST_LEN) ? expected + expected / 2
                                            : expected * 4;
    for (int i = 0; i < 256; i++) {
        if (freq[i] < lo || freq[i] > hi) {
            kprintf("[rng] self-test: FAIL (byte 0x%02x count %u, expected ~%u)\n",
                    i, freq[i], expected);
            return 0;
        }
    }

    /* Bit runs: count maximal runs of equal bits.  For N random bits the
     * expected count is ~N/2; allow +-10%. */
    uint64_t nbits = (uint64_t)len * 8;
    uint64_t runs = 1;
    uint64_t longest = 1, cur = 1;
    int prev = buf[0] & 1;
    for (size_t i = 0; i < len; i++) {
        for (int bit = (i == 0 ? 1 : 0); bit < 8; bit++) {
            int b = (buf[i] >> bit) & 1;
            if (b == prev) {
                cur++;
                if (cur > longest) longest = cur;
            } else {
                runs++;
                cur = 1;
                prev = b;
            }
        }
    }
    uint64_t exp_runs = nbits / 2;
    if (runs < exp_runs - exp_runs / 10 || runs > exp_runs + exp_runs / 10) {
        kprintf("[rng] self-test: FAIL (runs %u, expected ~%u)\n",
                (unsigned)runs, (unsigned)exp_runs);
        return 0;
    }
    if (longest > 64) {
        kprintf("[rng] self-test: FAIL (longest bit run %u > 64)\n",
                (unsigned)longest);
        return 0;
    }

    kprintf("[rng] self-test: PASS (%u KiB, byte-frequency + bit-runs, "
            "runs=%u longest=%u)\n",
            (unsigned)(len / 1024), (unsigned)runs,
            (unsigned)longest);
    return 1;
}

/* Print a 32-byte sample so an integration test can prove two boots do not
 * produce identical output. */
static void rng_print_sample(void) {
    uint8_t s[32];
    rngc_drbg_fill(&drbg, s, sizeof(s));
    kprintf("[rng] sample: ");
    for (size_t i = 0; i < sizeof(s); i++) kprintf("%02x", s[i]);
    kprintf("\n");
}

/* ---- public API ---- */

void rng_init(void) {
    if (rng_module_up) return;

    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpuid_count(1, 0, &a, &b, &c, &d);
    have_rdrand = (c & (1u << 30)) != 0;          /* CPUID.1:ECX.RDRAND */
    cpuid_count(7, 0, &a, &b, &c, &d);
    have_rdseed = (b & (1u << 18)) != 0;          /* CPUID.7:EBX.RDSEED */
    kprintf("[rng] CPU features: RDRAND=%s RDSEED=%s\n",
            have_rdrand ? "yes" : "no", have_rdseed ? "yes" : "no");

    uint64_t fl = spinlock_acquire_irqsave(&rng_lock);
    if (try_seed_from_hw_locked()) {
        rng_self_test();
        rng_print_sample();
    } else {
        kprintf("[rng] no usable hardware RNG; falling back to the "
                "interrupt-jitter pool\n");
        kprintf("[rng] pool: %u samples, est. %u bits (threshold %d)\n",
                jsamples, jbits, RNG_POOL_BITS);
        if (jbits >= RNG_POOL_BITS) {
            seed_from_pool_locked();
            rng_self_test();
            rng_print_sample();
        } else {
            kprintf("[rng] NOT READY: getentropy() returns -ENOSYS until the "
                    "pool reaches %d estimated bits\n", RNG_POOL_BITS);
        }
    }
    spinlock_release_irqrestore(&rng_lock, fl);

    __asm__ volatile ("" ::: "memory");
    rng_module_up = 1;
}

void rng_jitter_event(uint64_t tsc_now) {
    uint64_t prev = last_irq_tsc;
    last_irq_tsc = tsc_now;
    if (prev == 0) return;                   /* first event: no delta yet */

    uint64_t delta = tsc_now - prev;
    if (delta == 0) return;

    uint32_t idx = jsamples++;
    jpool[idx % JPOOL_WORDS] ^= rotl64(delta, (idx * 7) % 64)
                              + 0x9E3779B97F4A7C15ULL * (idx + 1);

    /* Estimate: count how many of the low 16 delta bits CHANGED since the
     * previous interrupt, capped at 4 bits per event.  This measures
     * observed variation; it never claims more than it saw. */
    int changed = popcnt16((uint32_t)((delta ^ last_irq_delta) & 0xFFFFu));
    last_irq_delta = delta;
    if (changed > 4) changed = 4;
    if (jbits < 1024) jbits += (uint32_t)changed;

    /* Opportunistic completion: cross the threshold -> seed right here. */
    if (rng_module_up && !rng_ready && jbits >= RNG_POOL_BITS) {
        uint64_t fl = spinlock_acquire_irqsave(&rng_lock);
        if (!rng_ready && jbits >= RNG_POOL_BITS) {
            if (!try_seed_from_hw_locked()) seed_from_pool_locked();
            rng_self_test();
            rng_print_sample();
        }
        spinlock_release_irqrestore(&rng_lock, fl);
    }
}

int rng_available(void) {
    if (rng_ready) return 1;
    if (!rng_module_up) return 0;
    uint64_t fl = spinlock_acquire_irqsave(&rng_lock);
    if (!rng_ready && jbits >= RNG_POOL_BITS) {
        if (!try_seed_from_hw_locked()) seed_from_pool_locked();
        rng_self_test();
        rng_print_sample();
    }
    spinlock_release_irqrestore(&rng_lock, fl);
    return rng_ready;
}

int rng_try_fill(void *out, size_t len) {
    if (!out && len) return -EFAULT;
    if (len == 0) return 0;
    if (!rng_available()) {
        /* ENOSYS is the agreed loud refusal (INTERNET_PLAN N0 / D1): the
         * caller must be able to tell "no entropy" from "entropy served". */
        return -ENOSYS;
    }

    uint64_t fl = spinlock_acquire_irqsave(&rng_lock);
    rngc_drbg_fill(&drbg, (uint8_t *)out, len);
    bytes_since_reseed += len;
    if (bytes_since_reseed >= RNG_RESEED_BYTES) {
        uint8_t material[RNGC_SEED_LEN];
        int from_hw = 0;
        if (have_rdseed && collect_hw(hw_rdseed64, material)) from_hw = 1;
        else if (have_rdrand && collect_hw(hw_rdrand64, material)) from_hw = 1;
        else snapshot_pool(material);
        rngc_drbg_reseed(&drbg, material);
        memset(material, 0, sizeof(material));
        bytes_since_reseed = 0;
        kprintf("[rng] periodic reseed from %s\n",
                from_hw ? (have_rdseed ? "RDSEED" : "RDRAND") : "jitter pool");
    }
    spinlock_release_irqrestore(&rng_lock, fl);
    return 0;
}

void rng_fill(void *out, size_t len) {
    if (!out || len == 0) return;
    if (rng_try_fill(out, len) != 0) memset(out, 0, len);
}

uint64_t rng_u64(void) {
    uint64_t v = 0;
    if (rng_try_fill(&v, sizeof(v)) != 0) return 0;
    return v;
}
