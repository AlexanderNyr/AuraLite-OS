/* kernel32_thr.c — W32A-3: threads, per-thread TLS, and synchronisation.
 *
 * The ledger-decided threading API (docs/plans/W32APP_PLAN.md phase W32A-3):
 * CreateThread/ExitThread/TerminateThread/ResumeThread/GetCurrentThread(Id)/
 * GetThreadTimes/SetThreadAffinityMask/GetExitCodeThread/FreeLibraryAndExit-
 * Thread, Tls+Fls x8, critical sections, exclusive-only SRW + CV, InitOnce
 * (Begin/Complete), SListHead+Flush, events, mutexes, semaphores, the three
 * waits + QueueUserAPC + SleepEx(alertable), and a minimal threadpool.
 *
 * DESIGN (the whole file follows from these five facts):
 *
 * 1. GS is the TEB, FS stays libc's.  Every w32 thread installs a TEB-lite
 *    page (w32/w32_teb.h) as its USER GS.base via ARCH_SET_GS — the main
 *    thread in w32_thr_init, every worker in its trampoline before user
 *    code runs.  FS keeps pointing at a libc-compatible cell (self@0,
 *    errno@8: the FIX_R3 ABI in lib/libc/include/pthread_tls.h) so libc
 *    errno works in workers.  The two bases never alias: same page would
 *    collide offsets, so the FS cell is small and separate.
 *
 * 2. clone() is raw.  Threads come from SYS_CLONE (56) with the Linux-
 *    compatible flags, resumed at the call site with RAX=0 on a seeded
 *    stack exactly like libc's pthread_create (lib/libc/src/pthread/
 *    pthread.c is the template).  The seed's third qword carries the
 *    struct w32_thread*; a 3-instruction asm stub pops it into RDI and
 *    jumps to the C trampoline.  CLONE_CHILD_SETTID publishes the tid
 *    into thread_obj->tid_word — but only once the child is FIRST
 *    SCHEDULED, so a zero word means "dead OR unborn", never "dead".
 *    The one join primitive is exited_word, a monotone 0 -> 1 flag the
 *    trampoline's ExitThread (or a TKILL's killer, once death is
 *    observed) sets under the spin; joiners futex-wait on it.  CLEARTID
 *    still zeroes tid_word at exit: the killers poll THAT to observe
 *    kernel death.  Tids are never reused (kernel next_tid is
 *    monotonic), so a dead tid stays dead and kill-all cannot hit an
 *    unrelated thread.
 *
 * 3. One global spin guards every w32 thread/sync structure.  It is a leaf
 *    lock (never held across user callbacks, futex waits, or thread
 *    creation), so there is no lock order to violate.  Blocking uses the
 *    kernel futex (SYS_FUTEX: 530 native, 202 on the Linux host — the ONE
 *    number that differs between worlds; clone/arch_prctl/tkill/exit/
 *    getpid match).  INFINITE waits block in the kernel; finite waits
 *    poll in 1 ms slices because the kernel ignores futex timeouts
 *    (documented limit, pinned by the fixtures with a +2 ms tolerance).
 *    Alertable waits always poll: APCs land within 1 ms without a
 *    kernel multiwait.
 *
 * 4. Exit frees thread-owned memory; CloseHandle frees the object.  Exit-
 *    Thread releases the stack, TEB, TLS blocks and APC nodes, abandons
 *    owned mutexes and unregisters from the live list — like Windows,
 *    where the kernel owns the stack.  The heap object lives until the
 *    last CloseHandle, like every other w32 handle kind.
 *
 * 5. No new syscalls.  Everything here rides clone/arch_prctl/futex/tkill/
 *    exit/getpid/nanosleep, all pre-existing.  The kernel half of the
 *    phase is the swapgs protocol + ARCH_SET_GS/ARCH_GET_GS only.
 */

#include <setjmp.h>   /* before w32_crt.h: sigjmp_buf */
#include "w32/kernel32.h"
#include "w32/w32_teb.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/w32_module.h"
#include "w32/w32_crt.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>

/* ---- raw syscall numbers -------------------------------------------------
 * Clone/arch_prctl/exit/getpid are identical on AuraLite and Linux; futex
 * and tkill are native-only numbers (the LX map translates Linux's).  The
 * host unit test runs this same file against Linux, hence the two ifdefs —
 * the ONLY host/guest divergence in the phase. */

#define THR_SYS_CLONE      56
#define THR_SYS_ARCH_PRCTL 158
#define THR_SYS_EXIT       60
#define THR_SYS_GETPID     39
#ifdef AURALITE_W32_HOST_TEST
#define THR_SYS_GETTID     186
#else
#define THR_SYS_GETTID     39
#endif
#ifdef AURALITE_W32_HOST_TEST
#define THR_SYS_FUTEX      202
#define THR_SYS_TKILL      200
#else
#define THR_SYS_FUTEX      530
#define THR_SYS_TKILL      531
#endif

#define THR_ARCH_SET_GS 0x1001
#define THR_ARCH_GET_GS 0x1004

#define THR_CLONE_VM             0x00000100
#define THR_CLONE_FS             0x00000200
#define THR_CLONE_FILES          0x00000400
#define THR_CLONE_SIGHAND        0x00000800
#define THR_CLONE_THREAD         0x00010000
#define THR_CLONE_SETTLS         0x00080000
#define THR_CLONE_PARENT_SETTID  0x00100000
#define THR_CLONE_CHILD_CLEARTID 0x00200000
#define THR_CLONE_CHILD_SETTID   0x01000000

#define THR_FUTEX_WAIT 0
#define THR_FUTEX_WAKE 1

#define THR_CREATE_SUSPENDED 0x4u

/* Default worker stack: 1 MiB, the pthread size and the Windows default
 * reserve.  No guard page (same documented limit as pthread_create). */
#define THR_STACK_DEFAULT (1024u * 1024u)
#define THR_STACK_MAX     (64u * 1024u * 1024u)

/* Live-thread cap.  Windows has none; 256 threads x 1 MiB stacks is 256 MiB
 * of virtual, and past it CreateThread fails loudly instead of over-
 * committing the address space. */
#define THR_LIVE_MAX 256

/* Finite-wait poll slice, in nanoseconds (1 ms). */
#define THR_POLL_NS 1000000L

static long thr_clone(unsigned long flags, uint64_t stack, uint64_t ptid,
                      uint64_t ctid, uint64_t tls) {
    return (long)syscall((int64_t)THR_SYS_CLONE, (uint64_t)flags, stack,
                         ptid, ctid, tls, 0);
}

static long thr_arch_prctl(int code, uint64_t addr) {
    return (long)syscall((int64_t)THR_SYS_ARCH_PRCTL, (uint64_t)(int64_t)code,
                         addr, 0, 0, 0, 0);
}

static long thr_futex_wait(volatile uint32_t *uaddr, uint32_t val) {
    return (long)syscall((int64_t)THR_SYS_FUTEX, (uint64_t)uaddr,
                         (uint64_t)THR_FUTEX_WAIT, (uint64_t)val, 0, 0, 0);
}

static long thr_futex_wake(volatile uint32_t *uaddr, int n) {
    return (long)syscall((int64_t)THR_SYS_FUTEX, (uint64_t)uaddr,
                         (uint64_t)THR_FUTEX_WAKE, (uint64_t)(int64_t)n,
                         0, 0, 0);
}

static long thr_tkill(long tid, int sig) {
    return (long)syscall((int64_t)THR_SYS_TKILL, (uint64_t)tid,
                         (uint64_t)(int64_t)sig, 0, 0, 0, 0);
}

/* Thread-only death: raw SYS_exit (60), which kills just the caller in
 * a thread group — on BOTH worlds (AuraLite's 60 is per-thread too).
 * NEVER glibc _exit() here: that issues exit_group and would murder the
 * whole process when a single worker falls off its start routine. */
static void thr_thread_exit(int code) __attribute__((noreturn));
static void thr_thread_exit(int code) {
    (void)syscall((int64_t)THR_SYS_EXIT, (uint64_t)(int64_t)code,
                  0, 0, 0, 0, 0);
    for (;;) {
        /* Unreachable (a thread that refuses to die spins instead of
         * returning into a torn-down trampoline). */
    }
}

static long thr_getpid(void) {
    return (long)syscall((int64_t)THR_SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

/* Current THREAD's id (the owner-word identity everywhere).  On Linux
 * this must be gettid(186): getpid(39) returns the TGID, identical for
 * every thread — using it here made all CS/MUTEX/SRW ownership one
 * shared id.  On AuraLite 39 already reads the thread's own id. */
static long thr_gettid(void) {
    return (long)syscall((int64_t)THR_SYS_GETTID, 0, 0, 0, 0, 0, 0);
}

/* ---- the global spin ------------------------------------------------------
 * Guards: the live list, the TLS/FLS allocators, the TLS module registry,
 * every waitable object's state + waiter list, every APC queue, the pool
 * queue.  Leaf discipline: sleep, user callbacks and clone() never happen
 * under it (asserted by review, not by code — the critical sections below
 * are all straight-line). */

static volatile int thr_lock;

static void thr_spin_lock(void) {
    while (__sync_lock_test_and_set(&thr_lock, 1)) {
        /* Spin, cooperatively: another thread holds it for nanoseconds. */
        (void)syscall((int64_t)24 /* SYS_SCHED_YIELD */, 0, 0, 0, 0, 0, 0);
    }
}

static void thr_spin_unlock(void) {
    __sync_lock_release(&thr_lock);
}

/* ---- process state -------------------------------------------------------- */

static int thr_ready;
static W32_DWORD thr_pid;        /* main thread's tid == process id */
static uint64_t thr_online_mask; /* 1 bit per online CPU */

int w32_thr_ready(void) { return thr_ready; }

W32_DWORD w32_thr_process_id(void) { return thr_ready ? thr_pid : 0; }

struct w32_teb *w32_teb_self(void) {
    struct w32_teb *t;
    if (!thr_ready)
        return 0;
    __asm__ volatile ("mov %%gs:0x30, %0" : "=r"(t));
    return t;
}

/* ---- the FS cell (guest only) ----------------------------------------------
 * libc errno reads %fs:0->errno_cell (FIX_R3 ABI).  A w32 worker's FS must
 * therefore point at 16 bytes shaped { self@0, errno@8 } — the numeric
 * asserts below pin the coupling against lib/libc/include/pthread_tls.h.
 * On the Linux host there is no SETTLS: the child inherits glibc's FS so
 * glibc errno keeps working (shared with the parent, racy but functional
 * — the host suite never asserts on worker errno). */

struct w32_fscell {
    struct w32_fscell *self;
    int errno_cell;
    int pad;
};

_Static_assert(offsetof(struct w32_fscell, self) == 0, "fscell self");
_Static_assert(offsetof(struct w32_fscell, errno_cell) == 8, "fscell errno");

/* ---- thread objects -------------------------------------------------------- */

typedef W32_DWORD (W32ABI *thr_start_fn)(void *param);
typedef void (W32ABI *thr_apc_fn)(uint64_t data);

struct thr_apc {
    struct thr_apc *next;
    thr_apc_fn fn;
    uint64_t data;
};

struct thr_waiter;
struct w32_thread {
    W32_DWORD tid;               /* kernel tid (0 == main pre-lookup) */
    volatile uint32_t tid_word;  /* SETTID target / CLEARTID word.  NOT a
                                  * liveness test: CHILD_SETTID lands on the
                                  * child's FIRST SCHEDULE, so a live but
                                  * never-scheduled thread reads 0. */
    volatile uint32_t exited_word; /* monotone 0 -> 1, the ONLY liveness
                                  * test.  Set under the spin by ExitThread
                                  * or (for a TKILL'd thread) by the killer
                                  * once death is observed. */
    int is_main;
    W32_DWORD exit_code;
    uint64_t birth_ft;           /* creation time (FILETIME) */
    W32_DWORD suspend_count;     /* >0: trampoline waits before user proc */
    volatile uint32_t start_seq; /* futex word for the suspend wait */
    uint64_t affinity_mask;      /* recorded, not enforced (no sched API) */
    thr_start_fn start;
    void *param;
    void *stack_base;
    uint64_t stack_size;
    struct w32_teb *teb;
    void **tls_blocks;           /* [W32_TLS_MODULES_MAX], pre-allocated */
    struct thr_apc *apc_head;
    struct thr_apc *apc_tail;
    struct w32_thr_mutex *owned; /* intrusive list of held mutexes */
    int in_live_list;
    struct thr_waiter *wait_wr; /* current general-lane registration (self
                              * only): set after enqueue, cleared after
                              * dequeue, so a suicide/kill can deregister
                              * a mid-wait victim (a stale entry would
                              * dangle into a munmap'd stack). */
    W32_HANDLE *wait_hs;
    int wait_n;
    volatile int kill_pending; /* host TerminateThread: die at the next
                              * checkpoint with kill_code (the guest
                              * kills preemptively; Linux cannot, so the
                              * host kills cooperatively — see below). */
    W32_DWORD kill_code;
    int orphaned;            /* handle closed while running: nobody will
                              * reap; ExitThread frees the heap object
                              * itself (the stack/TEB mappings leak — a
                              * thread cannot unmap the stack it runs
                              * on — reaped at process exit). */
};

static struct w32_thread *thr_live[THR_LIVE_MAX];
static int thr_live_n;

static uint64_t thr_now_ft(void) {
    /* FILETIME: 100 ns ticks since 1601-01-01.  Same epoch math as the
     * process times (kernel32_ps.c). */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 10000000ull
         + (uint64_t)ts.tv_nsec / 100ull + 116444736000000000ull;
}

/* ---- TLS module registry ----------------------------------------------------
 * One record per loaded image WITH a TLS directory (exe at slot 0, then
 * DLLs in load order).  The loader (w32run for the exe, w32_module for a
 * DLL) registers the template + resolved callback pointers; thread start
 * instantiates one block per module per thread and runs ATTACH; thread
 * exit runs DETACH; unload runs PROCESS_DETACH and frees every live
 * thread's block for that slot. */

typedef void (W32ABI *thr_tls_cb)(void *base, W32_DWORD reason, void *reserved);

#define THR_TLS_CB_MAX 64

struct thr_tls_mod {
    int used;
    void *base;                  /* image base (the callback's DllHandle) */
    uint64_t raw_size;           /* EndOfRawData - StartOfRawData */
    const void *raw_init;        /* template bytes (inside the image) */
    uint64_t zero_fill;
    thr_tls_cb cbs[THR_TLS_CB_MAX + 1]; /* NULL-terminated */
};

static struct thr_tls_mod thr_tls_mods[W32_TLS_MODULES_MAX];

/* Register a module.  Returns the slot (0..15) or -1 past the cap.  Called
 * with the module mapped and stable; the template bytes are COPIED OUT of
 * the image?  No — referenced: unload unregisters first, so no thread can
 * observe a dangling template (unload runs under the same spin that
 * guards thread start). */
int w32_tls_register_module(void *base, const void *raw_init,
                            uint64_t raw_size, uint64_t zero_fill,
                            thr_tls_cb *cbs, int ncb) {
    int slot = -1;
    thr_spin_lock();
    for (int i = 0; i < W32_TLS_MODULES_MAX; i++) {
        if (!thr_tls_mods[i].used) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        struct thr_tls_mod *m = &thr_tls_mods[slot];
        m->used = 1;
        m->base = base;
        m->raw_init = raw_init;
        m->raw_size = raw_size;
        m->zero_fill = zero_fill;
        for (int i = 0; i <= THR_TLS_CB_MAX; i++)
            m->cbs[i] = 0;
        for (int i = 0; i < ncb && i < THR_TLS_CB_MAX; i++)
            m->cbs[i] = cbs[i];
    }
    thr_spin_unlock();
    return slot;
}

/* Instantiate every registered module's block for one thread.  Runs in the
 * trampoline under the spin (registry stable); pre-allocated blocks arrive
 * via t->tls_blocks (parent side) and only the delta is malloc'd here. */
static int thr_tls_instantiate(struct w32_thread *t) {
    for (int i = 0; i < W32_TLS_MODULES_MAX; i++) {
        struct thr_tls_mod *m = &thr_tls_mods[i];
        uint64_t total;
        void *blk;
        if (!m->used || t->tls_blocks[i])
            continue;
        total = m->raw_size + m->zero_fill;
        if (total == 0) {
            /* A module with callbacks but no data still gets a slot so
             * the index is dense; the block pointer stays NULL and only
             * the callbacks run. */
            continue;
        }
        blk = calloc(1, (size_t)total);
        if (!blk)
            return -1;
        if (m->raw_size && m->raw_init)
            memcpy(blk, m->raw_init, (size_t)m->raw_size);
        t->tls_blocks[i] = blk;
    }
    return 0;
}

/* Run one module's callbacks for one reason, in array order.  NEVER under
 * the spin: callbacks are user code and may call back into this file. */
static void thr_tls_run_cbs(int slot, W32_DWORD reason) {
    thr_tls_cb cbs[THR_TLS_CB_MAX + 1];
    void *base;
    thr_spin_lock();
    if (slot < 0 || slot >= W32_TLS_MODULES_MAX || !thr_tls_mods[slot].used) {
        thr_spin_unlock();
        return;
    }
    base = thr_tls_mods[slot].base;
    for (int i = 0; i <= THR_TLS_CB_MAX; i++)
        cbs[i] = thr_tls_mods[slot].cbs[i];
    thr_spin_unlock();
    for (int i = 0; i <= THR_TLS_CB_MAX && cbs[i]; i++)
        cbs[i](base, reason, NULL);
}

/* Run one module's callbacks for one reason (LoadLibrary's ATTACH).
 * Never under the spin (user code). */
void w32_tls_run_module(int slot, W32_DWORD reason) {
    thr_tls_run_cbs(slot, reason);
}

/* Unregister (unload): PROCESS_DETACH first (TLS-before-DllMain, like
 * attach), then free every live thread's block for the slot. */
void w32_tls_unregister_module(void *base) {
    int slot = -1;
    thr_spin_lock();
    for (int i = 0; i < W32_TLS_MODULES_MAX; i++) {
        if (thr_tls_mods[i].used && thr_tls_mods[i].base == base) {
            slot = i;
            break;
        }
    }
    thr_spin_unlock();
    if (slot < 0)
        return;
    thr_tls_run_cbs(slot, W32_DLL_PROCESS_DETACH);
    thr_spin_lock();
    for (int i = 0; i < thr_live_n; i++) {
        struct w32_thread *t = thr_live[i];
        if (t && t->tls_blocks && t->tls_blocks[slot]) {
            free(t->tls_blocks[slot]);
            t->tls_blocks[slot] = 0;
        }
    }
    /* Main thread's blocks: the main obj is in the live list too. */
    thr_tls_mods[slot].used = 0;
    thr_tls_mods[slot].base = 0;
    thr_spin_unlock();
}

/* ---- Tls/Fls slot allocators -------------------------------------------------
 * Bitmask allocators; alloc zeroes the slot in every live TEB (a reused
 * index never shows a stale value — deterministic, fixture-pinned).  FLS
 * callbacks live in a parallel array, set at FlsAlloc. */

static uint64_t thr_tls_used;
static uint64_t thr_fls_used_lo;
static uint64_t thr_fls_used_hi;
typedef void (W32ABI *thr_fls_cb)(void *value);
static thr_fls_cb thr_fls_cbs[W32_TEB_FLS_SLOTS];

static int thr_bit_alloc(uint64_t *m0, uint64_t *m1, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t *m = (i < 64) ? m0 : m1;
        int b = i & 63;
        if (m && !(*m & (1ull << b))) {
            *m |= (1ull << b);
            return i;
        }
    }
    return -1;
}

static void thr_slots_zero_all(int is_fls, int idx) {
    for (int i = 0; i < thr_live_n; i++) {
        struct w32_thread *t = thr_live[i];
        if (!t || !t->teb)
            continue;
        if (is_fls)
            t->teb->fls_slots[idx] = 0;
        else
            t->teb->tls_slots[idx] = 0;
    }
}

/* ---- waitable objects ---------------------------------------------------------
 * Events, mutexes and semaphores are heap structs behind HANDLEs.  State +
 * waiter list live under the global spin; each object has a futex word
 * bumped on every state change.  Waiters: lock, test, enqueue private
 * node, unlock, block on the PRIVATE word, re-test.  No lost wakeups: the
 * test and the enqueue are atomic under the spin. */

struct thr_waiter {
    struct thr_waiter *next;
    volatile uint32_t word;      /* private futex: bumped to wake */
};

struct thr_waitable {
    int kind;                    /* handle kind */
    volatile uint32_t seq;       /* bumped on every state change */
    struct thr_waiter *waiters;
};

static void thr_waitable_wake(struct thr_waitable *w) {
    w->seq++;
    /* Direct seq-waiters too (the pool workers wait on ev->w.seq without
     * registering — without this wake they sleep through the bump). */
    thr_futex_wake(&w->seq, 0x7fffffff);
    for (struct thr_waiter *wr = w->waiters; wr; wr = wr->next) {
        wr->word++;
        thr_futex_wake(&wr->word, 1);
    }
}

static void thr_waiter_add(struct thr_waitable *w, struct thr_waiter *wr) {
    wr->next = w->waiters;
    w->waiters = wr;
}

static void thr_waiter_del(struct thr_waitable *w, struct thr_waiter *wr) {
    struct thr_waiter **p = &w->waiters;
    while (*p) {
        if (*p == wr) {
            *p = wr->next;
            return;
        }
        p = &(*p)->next;
    }
}

struct w32_thr_event {
    struct thr_waitable w;
    int manual;
    int signaled;
    char *name;                  /* process-local name, or NULL */
    int refs;
};

struct w32_thr_mutex {
    struct thr_waitable w;
    W32_DWORD owner_tid;         /* 0 == free */
    int recursion;
    int abandoned;               /* set when the owner died holding it */
    char *name;
    int refs;
    struct w32_thr_mutex *owner_next; /* intrusive per-thread owned list */
};

struct w32_thr_sem {
    struct thr_waitable w;
    W32_LONG count;
    W32_LONG max;
    char *name;
    int refs;
};

/* Process-local named-object table (same name == same object, like Windows
 * within one process; cross-process visibility: none, documented). */
#define THR_NAMES_MAX 64
static struct {
    int kind;
    char name[64];
    void *obj;
} thr_names[THR_NAMES_MAX];

static void *thr_name_lookup(int kind, const char *name) {
    if (!name || !*name)
        return 0;
    for (int i = 0; i < THR_NAMES_MAX; i++) {
        if (thr_names[i].obj && thr_names[i].kind == kind &&
            strcmp(thr_names[i].name, name) == 0)
            return thr_names[i].obj;
    }
    return 0;
}

static int thr_name_insert(int kind, const char *name, void *obj) {
    if (!name || !*name)
        return 0;
    for (int i = 0; i < THR_NAMES_MAX; i++) {
        if (!thr_names[i].obj) {
            thr_names[i].kind = kind;
            strncpy(thr_names[i].name, name, sizeof(thr_names[i].name) - 1);
            thr_names[i].name[sizeof(thr_names[i].name) - 1] = '\0';
            thr_names[i].obj = obj;
            return 0;
        }
    }
    return -1;
}

static void thr_name_remove(void *obj) {
    for (int i = 0; i < THR_NAMES_MAX; i++) {
        if (thr_names[i].obj == obj) {
            thr_names[i].obj = 0;
            thr_names[i].name[0] = '\0';
            return;
        }
    }
}

/* ---- forward declarations ----------------------------------------------------- */

static W32_DWORD thr_wait_n(int n, W32_HANDLE *hs, int wait_all,
                            W32_DWORD ms, int alertable);
static void thr_abandon_owned(struct w32_thread *t);
#ifdef AURALITE_W32_HOST_TEST
static void thr_waker_fn(int sig);
#endif
#define THR_WAKER_SIG SIGURG
static void thr_checkpoint(void);
static void thr_wait_dequeue(int n, W32_HANDLE *hs,
                             struct thr_waiter *wr);

/* ---- process init -------------------------------------------------------------- */

void w32_thr_init(void) {
    long n;
    struct w32_thread *main_thr;
    struct w32_teb *teb;

    if (thr_ready) {
        /* Idempotent: the host suite re-inits.  The main TEB persists;
         * the registries reset to fresh-process state. */
        thr_spin_lock();
        thr_tls_used = 0;
        thr_fls_used_lo = 0;
        thr_fls_used_hi = 0;
        for (int i = 0; i < W32_TEB_FLS_SLOTS; i++)
            thr_fls_cbs[i] = 0;
        thr_spin_unlock();
        return;
    }

    thr_pid = (W32_DWORD)thr_getpid();
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0)
        n = 1;
    if (n >= 64)
        thr_online_mask = ~0ull;
    else
        thr_online_mask = (1ull << n) - 1u;

    /* The main thread's TEB: one zeroed page, Self + ClientId filled,
     * installed via ARCH_SET_GS before anything else runs. */
    teb = (struct w32_teb *)mmap(0, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (teb == MAP_FAILED)
        return;   /* preposterous; the process limps on with errno-only */
    memset(teb, 0, sizeof(*teb));
    teb->self = (uint64_t)(uintptr_t)teb;
    teb->client_pid = thr_pid;
    teb->client_tid = thr_pid;
    teb->last_error = W32_ERROR_SUCCESS;
    if (thr_arch_prctl(THR_ARCH_SET_GS, (uint64_t)(uintptr_t)teb) != 0) {
        munmap(teb, 4096);
        return;
    }

    /* The main thread's object: in the live list (slot walks need its
     * TEB), stackless (never freed), wordless (no handle waits on it). */
    main_thr = (struct w32_thread *)calloc(1, sizeof(*main_thr));
    if (!main_thr) {
        munmap(teb, 4096);
        return;
    }
    main_thr->tid = thr_pid;
    main_thr->is_main = 1;
    main_thr->birth_ft = thr_now_ft();
    main_thr->affinity_mask = thr_online_mask;
    main_thr->teb = teb;
    main_thr->tls_blocks = (void **)calloc(W32_TLS_MODULES_MAX,
                                           sizeof(void *));
    if (!main_thr->tls_blocks) {
        free(main_thr);
        munmap(teb, 4096);
        return;
    }
    teb->thread_obj = main_thr;
    teb->tls_storage_ptr = (uint64_t)(uintptr_t)main_thr->tls_blocks;

    thr_spin_lock();
    thr_live[0] = main_thr;
    thr_live_n = 1;
    main_thr->in_live_list = 1;
#ifdef AURALITE_W32_HOST_TEST
    /* The cooperative-kill tap (host only — the guest kills with
     * SIGKILL and never sets kill_pending).  Deliberately WITHOUT
     * SA_RESTART: the tap must EINTR the victim out of its wait. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = thr_waker_fn;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        (void)sigaction(THR_WAKER_SIG, &sa, 0);
    }
#endif
    thr_ready = 1;
    thr_spin_unlock();

    /* Main-thread TLS instantiation + PROCESS_ATTACH callbacks for the
     * modules registered before init (the exe's: w32run registers right
     * after this returns — attach order is exe-first regardless because
     * no user code has run yet; see w32_tls_attach_main). */
}

/* After the exe's TLS module is registered (w32run, post-init), finish the
 * main thread: instantiate its blocks and run PROCESS_ATTACH.  DLLs loaded
 * later take the LoadLibrary path instead. */
void w32_tls_attach_main(void) {
    struct w32_thread *t;
    if (!thr_ready)
        return;
    t = (struct w32_thread *)w32_teb_self()->thread_obj;
    thr_spin_lock();
    (void)thr_tls_instantiate(t);
    thr_spin_unlock();
    for (int i = 0; i < W32_TLS_MODULES_MAX; i++) {
        thr_spin_lock();
        int used = thr_tls_mods[i].used;
        thr_spin_unlock();
        if (used)
            thr_tls_run_cbs(i, W32_DLL_PROCESS_ATTACH);
    }
}

/* ---- CreateThread + the trampoline -------------------------------------------------- */

/* The seed: the child returns from clone() with RAX=0 and RSP=child_sp,
 * and the `ret` closing syscall() pops the stub address.  Layout:
 *   [sp+0]  thr_child_entry (asm stub)
 *   [sp+8]  pad (RSP after the ret: 8 mod 16, as-if-called)
 *   [sp+16] struct w32_thread* (the handoff) */
__asm__(
".text\n"
".globl thr_child_entry\n"
"thr_child_entry:\n"
"    mov 8(%rsp), %rdi\n"
"    jmp thr_child_main\n"
);

extern void thr_child_entry(void);
void thr_child_main(struct w32_thread *t) __attribute__((noreturn));

void thr_child_main(struct w32_thread *t) {
    /* The FIRST thing: install GS.  No w32 call happens before this
     * (raw syscalls only), because LastError already routes via %gs. */
    struct w32_teb *teb = t->teb;
    W32_DWORD me = (W32_DWORD)thr_gettid();
    W32_DWORD code;

    teb->client_tid = me;
    if (thr_arch_prctl(THR_ARCH_SET_GS, (uint64_t)(uintptr_t)teb) != 0) {
        t->exit_code = W32_ERROR_INVALID_FUNCTION;
        thr_thread_exit(1);
    }

    /* Per-module TLS blocks + THREAD_ATTACH, in load order. */
    thr_spin_lock();
    if (thr_tls_instantiate(t) != 0) {
        thr_spin_unlock();
        t->exit_code = W32_ERROR_NOT_ENOUGH_MEMORY;
        thr_thread_exit(8);
    }
    thr_spin_unlock();
    for (int i = 0; i < W32_TLS_MODULES_MAX; i++) {
        thr_spin_lock();
        int used = thr_tls_mods[i].used;
        thr_spin_unlock();
        if (used)
            thr_tls_run_cbs(i, W32_DLL_THREAD_ATTACH);
    }

    /* CREATE_SUSPENDED: wait for the first ResumeThread.  The count is
     * re-checked after every wake (no lost wakeup: ResumeThread bumps
     * the word under the spin whenever it drops the count). */
    for (;;) {
        uint32_t seq;
        W32_DWORD susp;
        thr_checkpoint();   /* a suspended victim still dies on kill */
        thr_spin_lock();
        susp = t->suspend_count;
        seq = t->start_seq;
        thr_spin_unlock();
        if (susp == 0)
            break;
        (void)thr_futex_wait(&t->start_seq, seq);
    }

    code = t->start(t->param);
    /* Falling off the end is ExitThread(code). */
    ExitThread(code);
    thr_thread_exit((int)code);
}

/* Death-watch for a TKILL'd child: confirm birth (CHILD_SETTID lands
 * on first schedule), then observe the CLEARTID zero, both bounded.
 * Callers must not unmap the victim's stack/TEB before this returns —
 * the SIGKILL may still be in flight while the child runs its first
 * instructions. */
static void thr_wait_kernel_dead(struct w32_thread *t) {
    int i;
    for (i = 0; i < 50 && t->tid_word == 0; i++) {
        struct timespec sl = { 0, THR_POLL_NS };
        nanosleep(&sl, 0);
    }
    for (i = 0; i < 2000 && t->tid_word != 0; i++) {
        struct timespec sl = { 0, THR_POLL_NS };
        nanosleep(&sl, 0);
    }
}

W32ABI W32_HANDLE CreateThread(void *security, W32_SIZE_T stack_size,
                               thr_start_fn start, void *param,
                               W32_DWORD flags, W32_DWORD *tid_out) {
    struct w32_thread *t;
    struct w32_teb *teb;
    void *stack = 0;
    uint64_t stack_sz = stack_size ? (uint64_t)stack_size : THR_STACK_DEFAULT;
    uint64_t child_sp;
    uint64_t *seed;
    struct w32_fscell *cell = 0;
    long flags_cl;
    long tid;
    W32_HANDLE h;

    (void)security;   /* like Windows without ACLs: accepted, ignored */
    if (!start) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & ~(THR_CREATE_SUSPENDED | 0x10000u)) {
        /* 0x10000 (STACK_SIZE_PARAM_IS_A_RESERVATION) is accepted: our
         * stack_size already IS the reservation.  Anything else refuses. */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (stack_sz < 64u * 1024u)
        stack_sz = 64u * 1024u;   /* floor: TEB-adjacent frames need room */
    if (stack_sz > THR_STACK_MAX) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    stack_sz = (stack_sz + 4095u) & ~4095u;

    t = (struct w32_thread *)calloc(1, sizeof(*t));
    teb = (struct w32_teb *)mmap(0, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!t || teb == MAP_FAILED) {
        free(t);
        if (teb != MAP_FAILED)
            munmap(teb, 4096);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    stack = mmap(0, (size_t)stack_sz, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        free(t);
        munmap(teb, 4096);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    t->tls_blocks = (void **)calloc(W32_TLS_MODULES_MAX, sizeof(void *));
    if (!t->tls_blocks) {
        free(t);
        munmap(teb, 4096);
        munmap(stack, (size_t)stack_sz);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    /* Pre-allocate the TLS blocks for the modules registered NOW, under
     * the spin (the registry cannot grow mid-snapshot).  The trampoline
     * only mallocs the delta if a LoadLibrary lands in between. */
    thr_spin_lock();
    if (thr_live_n >= THR_LIVE_MAX) {
        thr_spin_unlock();
        free(t->tls_blocks);
        free(t);
        munmap(teb, 4096);
        munmap(stack, (size_t)stack_sz);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (thr_tls_instantiate(t) != 0) {
        thr_spin_unlock();
        for (int i = 0; i < W32_TLS_MODULES_MAX; i++)
            free(t->tls_blocks[i]);
        free(t->tls_blocks);
        free(t);
        munmap(teb, 4096);
        munmap(stack, (size_t)stack_sz);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    thr_spin_unlock();

    memset(teb, 0, sizeof(*teb));
    teb->self = (uint64_t)(uintptr_t)teb;
    teb->client_pid = thr_pid;
    teb->client_tid = 0;   /* trampoline fills it (getpid, race-free) */
    teb->last_error = W32_ERROR_SUCCESS;
    teb->thread_obj = t;
    teb->tls_storage_ptr = (uint64_t)(uintptr_t)t->tls_blocks;
    teb->nt_stack_base = (uint64_t)(uintptr_t)stack + stack_sz;
    teb->nt_stack_limit = (uint64_t)(uintptr_t)stack;

    t->start = start;
    t->param = param;
    t->stack_base = stack;
    t->stack_size = stack_sz;
    t->teb = teb;
    t->birth_ft = thr_now_ft();
    t->affinity_mask = thr_online_mask;
    t->exit_code = W32_STILL_ACTIVE;
    if (flags & THR_CREATE_SUSPENDED)
        t->suspend_count = 1;

    /* The seed + (guest) the FS cell at the stack top. */
    child_sp = ((uint64_t)(uintptr_t)stack + stack_sz) & ~15ull;
#ifndef AURALITE_W32_HOST_TEST
    child_sp -= 16;
    cell = (struct w32_fscell *)(uintptr_t)child_sp;
    cell->self = cell;
    cell->errno_cell = 0;
#endif
    /* 32 bytes: after the ret that pops seed[0] RSP is 8 mod 16, as the
     * x86-64 ABI requires on C function entry (seed[2] is the handoff). */
    child_sp -= 32;
    seed = (uint64_t *)(uintptr_t)child_sp;
    seed[0] = (uint64_t)(uintptr_t)thr_child_entry;
    seed[1] = 0;
    seed[2] = (uint64_t)(uintptr_t)t;
    seed[3] = 0;

    flags_cl = THR_CLONE_VM | THR_CLONE_FS | THR_CLONE_FILES |
               THR_CLONE_SIGHAND | THR_CLONE_THREAD |
               THR_CLONE_PARENT_SETTID | THR_CLONE_CHILD_SETTID |
               THR_CLONE_CHILD_CLEARTID;
#ifndef AURALITE_W32_HOST_TEST
    flags_cl |= THR_CLONE_SETTLS;
#endif

    tid = thr_clone((unsigned long)flags_cl, child_sp,
                    (uint64_t)(uintptr_t)&t->tid,
                    (uint64_t)(uintptr_t)&t->tid_word,
                    cell ? (uint64_t)(uintptr_t)cell : 0);
    if (tid < 0) {
        for (int i = 0; i < W32_TLS_MODULES_MAX; i++)
            free(t->tls_blocks[i]);
        free(t->tls_blocks);
        free(t);
        munmap(teb, 4096);
        munmap(stack, (size_t)stack_sz);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    /* Only the PARENT_SETTID store precedes the return (so t->tid is
     * valid here); CHILD_SETTID lands on the child's first schedule.
     * Never read tid_word as a liveness test — see exited_word. */
    t->tid = (W32_DWORD)tid;

    thr_spin_lock();
    if (thr_live_n < THR_LIVE_MAX) {
        thr_live[thr_live_n++] = t;
        t->in_live_list = 1;
    }
    thr_spin_unlock();
    if (!t->in_live_list) {
        /* Over the cap between the check and the publish (racing
         * CreateThread): kill the child — it only runs the trampoline,
         * which notices nothing — and fail. */
        (void)thr_tkill(tid, SIGKILL);
        thr_wait_kernel_dead(t);
        for (int i = 0; i < W32_TLS_MODULES_MAX; i++)
            free(t->tls_blocks[i]);
        free(t->tls_blocks);
        free(t);
        munmap(teb, 4096);
        munmap(stack, (size_t)stack_sz);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    h = w32_handle_alloc_obj(W32_HANDLE_KIND_THREAD, t, NULL);
    if (!h) {
        /* Table full: same teardown (the free_fn is NULL — thread objects
         * are freed by CloseHandle's reaper, below, not by the table). */
        (void)thr_tkill(tid, SIGKILL);
        thr_wait_kernel_dead(t);
        thr_spin_lock();
        for (int i = 0; i < thr_live_n; i++) {
            if (thr_live[i] == t) {
                thr_live[i] = thr_live[--thr_live_n];
                break;
            }
        }
        t->in_live_list = 0;
        thr_spin_unlock();
        for (int i = 0; i < W32_TLS_MODULES_MAX; i++)
            free(t->tls_blocks[i]);
        free(t->tls_blocks);
        free(t);
        munmap(teb, 4096);
        munmap(stack, (size_t)stack_sz);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return 0;
    }

    if (tid_out)
        *tid_out = (W32_DWORD)tid;
    w32_set_last_error(W32_ERROR_SUCCESS);
    return h;
}

/* ---- thread exit --------------------------------------------------------------- */

/* Run the FLS callbacks for the exiting thread, ascending index.  Values
 * are cleared as they run (a callback re-setting its own slot re-arms
 * it for... nothing: single pass, documented). */
static void thr_fls_run_exit(struct w32_teb *teb) {
    thr_fls_cb cbs[W32_TEB_FLS_SLOTS];
    void *vals[W32_TEB_FLS_SLOTS];
    thr_spin_lock();
    for (int i = 0; i < W32_TEB_FLS_SLOTS; i++) {
        cbs[i] = thr_fls_cbs[i];
        vals[i] = teb->fls_slots[i];
        teb->fls_slots[i] = 0;
    }
    thr_spin_unlock();
    for (int i = 0; i < W32_TEB_FLS_SLOTS; i++) {
        if (cbs[i] && vals[i])
            cbs[i](vals[i]);
    }
}

/* Abandon every mutex the thread holds: waiters get WAIT_ABANDONED + the
 * lock.  Under the spin (callers hold it or take it). */
static void thr_abandon_owned(struct w32_thread *t) {
    struct w32_thr_mutex *m = t->owned;
    t->owned = 0;
    while (m) {
        struct w32_thr_mutex *nx = m->owner_next;
        m->owner_next = 0;
        m->owner_tid = 0;
        m->recursion = 0;
        m->abandoned = 1;
        thr_waitable_wake(&m->w);
        m = nx;
    }
}

static void thr_live_remove(struct w32_thread *t) {
    for (int i = 0; i < thr_live_n; i++) {
        if (thr_live[i] == t) {
            thr_live[i] = thr_live[--thr_live_n];
            t->in_live_list = 0;
            return;
        }
    }
}

/* ---- cooperative kill (host TerminateThread) ---------------------------------------
 *
 * Linux cannot asynchronously kill ONE thread of a group: a fatal signal
 * murders the whole process.  AuraLite's kernel CAN (a fatal signal
 * terminates only the target TCB), so the guest TerminateThread stays
 * preemptive (tkill/SIGKILL).  On the host the killer instead FLAGS the
 * victim (kill_pending + kill_code, under the spin) and taps it awake;
 * the victim observes the flag at its next CHECKPOINT and suicides.
 * Checkpoints sit at every blocking re-test, at Sleep, and at CS
 * enter/leave + SRW acquire (so lock-spinners die promptly).  A pure
 * userspace spinner (no w32 calls at all) never observes the flag:
 * documented host-only limit (the guest kernel-kills it).
 *
 * The tap is a no-op SIGURG (no SA_RESTART): it EINTRs the victim out of
 * futex_wait/nanosleep into its re-test, which checkpoints.  There is a
 * ~200 ns lost-tap hole (flag set + tap consumed between the victim's
 * check and its wait-entry); steady-state blocked victims are already
 * IN the wait so the tap lands, spinners checkpoint every few ns
 * without needing the tap at all, and finite waits re-checkpoint every
 * 1 ms slice.  Host-only, best-effort, documented. */
#ifdef AURALITE_W32_HOST_TEST
static void thr_waker_fn(int sig) {
    (void)sig;   /* the EINTR is the message; there is no payload */
}
#endif

/* Suicide: what a checkpointed victim runs.  Same shape as ExitThread's
 * publish step (killer's code wins, abandon, live-remove, APC-drop,
 * TLS-free, orphaned self-free) but NO user callbacks — Windows runs
 * no TLS/FLS detach on terminated threads, and neither do we. */
static void thr_suicide(W32_DWORD code) __attribute__((noreturn));
static void thr_suicide(W32_DWORD code) {
    struct w32_teb *teb = w32_teb_self();
    struct w32_thread *t;
    struct thr_apc *apc;
    if (!teb || !teb->thread_obj)
        thr_thread_exit((int)code);
    t = (struct w32_thread *)teb->thread_obj;
    /* Defensive deregister: every current checkpoint sits where the
     * victim cannot be enqueued (loop tops run after the dequeue), so
     * wait_wr is always NULL here today — but a future mid-wait
     * checkpoint would dangle the stack without this.  (The guest
     * killer's post-death walk below is the LIVE one: kernel death can
     * strike mid-wait.) */
    thr_spin_lock();
    if (t->wait_wr && t->wait_hs)
        thr_wait_dequeue(t->wait_n, t->wait_hs, t->wait_wr);
    t->wait_wr = 0;
    t->wait_hs = 0;
    t->wait_n = 0;
    if (!t->exited_word)
        t->exit_code = code;
    t->exited_word = 1;
    thr_futex_wake(&t->exited_word, 0x7fffffff);
    thr_abandon_owned(t);
    thr_live_remove(t);
    apc = t->apc_head;
    t->apc_head = t->apc_tail = 0;
    {
        int orph = t->orphaned;
        thr_spin_unlock();
        while (apc) {
            struct thr_apc *nx = apc->next;
            free(apc);
            apc = nx;
        }
        if (!t->is_main) {
            if (t->tls_blocks) {
                for (int i = 0; i < W32_TLS_MODULES_MAX; i++)
                    free(t->tls_blocks[i]);
                free(t->tls_blocks);
                t->tls_blocks = 0;
            }
            /* The stack/TEB mappings go to the closer (self-munmap is
             * death — see ExitThread step 5); an orphaned victim frees
             * the heap object itself. */
            if (orph)
                free(t);
        }
    }
    thr_thread_exit((int)code);
}

/* Checkpoint: call with NO spin held, at every blocking re-test and at
 * the spinner-called entries.  One volatile branch when unflagged. */
static void thr_checkpoint(void) {
    struct w32_teb *teb = w32_teb_self();
    struct w32_thread *t;
    if (!teb)
        return;
    t = (struct w32_thread *)teb->thread_obj;
    if (!t || !t->kill_pending)
        return;
    thr_suicide(t->kill_code);
}

/* Sleep (kernel32.c) checkpoints through here. */
void w32_thr_checkpoint(void) {
    thr_checkpoint();
}

W32ABI void ExitThread(W32_DWORD code) {
    struct w32_teb *teb = w32_teb_self();
    struct w32_thread *t;
    struct thr_apc *apc;

    if (!teb || !teb->thread_obj) {
        /* Pre-init or foreign thread: nothing w32 owns can be released.
         * Raw exit is the only honest move. */
        thr_thread_exit((int)code);
    }
    t = (struct w32_thread *)teb->thread_obj;

    /* 1. FLS callbacks (user code, no locks held). */
    thr_fls_run_exit(teb);

    /* 2. TLS THREAD_DETACH, reverse module order. */
    for (int i = W32_TLS_MODULES_MAX - 1; i >= 0; i--) {
        thr_spin_lock();
        int used = thr_tls_mods[i].used;
        thr_spin_unlock();
        if (used)
            thr_tls_run_cbs(i, W32_DLL_THREAD_DETACH);
    }

    /* 3. Publish death: the code (unless a racing TerminateThread beat
     * us — its code stands), the monotone flag, and a wake for EVERY
     * joiner; abandon owned mutexes + leave the live list.  All under
     * the spin, so a concurrent killer serializes with us. */
    thr_spin_lock();
    /* A racing host-killer wins the code (deterministic: kill_pending is
     * set under this same spin, so exactly one of us publishes first). */
    if (t->kill_pending)
        code = t->kill_code;
    if (!t->exited_word)
        t->exit_code = code;
    t->exited_word = 1;
    thr_futex_wake(&t->exited_word, 0x7fffffff);
    thr_abandon_owned(t);
    thr_live_remove(t);
    /* 4. Drop queued APCs (undelivered is correct: the thread is gone). */
    apc = t->apc_head;
    t->apc_head = t->apc_tail = 0;
    thr_spin_unlock();
    while (apc) {
        struct thr_apc *nx = apc->next;
        free(apc);
        apc = nx;
    }

    /* 5. Free thread-owned HEAP memory (TLS blocks + APC nodes went
     * above; all self-safe — the heap is not the stack).  The stack and
     * TEB mappings are NEVER unmapped here: this thread runs ON that
     * stack, so self-munmap is instant death.  The closer (running on
     * another thread) reaps them — see w32_thread_close.  An orphaned
     * thread (handle closed while running) additionally frees the heap
     * object itself: with no handles and no live-list entry nobody else
     * can reach it; only its two mappings leak, reaped at process exit.
     * Nothing below touches t after a self-free. */
    if (!t->is_main) {
        for (int i = 0; i < W32_TLS_MODULES_MAX; i++)
            free(t->tls_blocks[i]);
        free(t->tls_blocks);
        t->tls_blocks = 0;
        thr_spin_lock();
        {
            int orph = t->orphaned;
            thr_spin_unlock();
            if (orph)
                free(t);
        }
    }
    /* 6. Gone: the kernel zeroes tid_word on the way (the killers'
     * death-watch); joiners already woke on exited_word. */
    thr_thread_exit((int)code);
    for (;;) { }
}

W32ABI void FreeLibraryAndExitThread(void *mod, W32_DWORD code) {
    /* The free is attempted, the exit is unconditional — even a bogus
     * module cannot keep the thread alive. */
    (void)w32_FreeLibrary(mod);
    ExitThread(code);
    for (;;) { }
}

W32ABI W32_BOOL TerminateThread(W32_HANDLE h, W32_DWORD code) {
    struct w32_thread *t;
    W32_DWORD tid;

    if (h == W32_CURRENT_THREAD) {
        struct w32_teb *teb = w32_teb_self();
        if (!teb || !teb->thread_obj) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return W32_FALSE;
        }
        t = (struct w32_thread *)teb->thread_obj;
    } else {
        t = (struct w32_thread *)w32_handle_get_obj(h,
                                              W32_HANDLE_KIND_THREAD);
        if (!t) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return W32_FALSE;
        }
    }
    tid = t->tid ? t->tid : (W32_DWORD)thr_gettid();

    if (!t->is_main && t->exited_word) {
        /* Already dead: the goal state.  The recorded code stands. */
        w32_set_last_error(W32_ERROR_SUCCESS);
        return W32_TRUE;
    }

#ifdef AURALITE_W32_HOST_TEST
    /* HOST: cooperative kill (a Linux SIGKILL would murder the group).
     * Flag + prompt abandon + kicks, then return ASYNC like Windows —
     * the victim suicides at its next checkpoint and publishes there;
     * the join observes it.  A racing victim that already published
     * keeps its code (decided under the spin). */
    thr_spin_lock();
    if (!t->exited_word) {
        t->kill_pending = 1;
        t->kill_code = code;
        thr_abandon_owned(t);
        t->start_seq++;   /* suspend-kick (already-exiting ignores it) */
    }
    thr_spin_unlock();
    thr_futex_wake(&t->start_seq, 1);
    if (!t->is_main) {
        (void)thr_tkill((long)tid, THR_WAKER_SIG);
        w32_set_last_error(W32_ERROR_SUCCESS);
        return W32_TRUE;
    }
    /* Main suicide on the host: the process must die with it (a lone
     * dead main beside live workers is unrepresentable — and the test
     * never does this).  exit_group, documented divergence. */
    _exit((int)code);
#else
    /* GUEST: preemptive kill (AuraLite SIGKILL terminates only the
     * target TCB — siblings survive).  Abandon BEFORE the kill
     * (promptness) and AFTER death is observed (airtightness: a dead
     * thread cannot acquire, so the second walk cannot miss).  The
     * victim's own ExitThread path never runs — no FLS/TLS callbacks,
     * no stack free — exactly like Windows. */
    thr_spin_lock();
    thr_abandon_owned(t);
    thr_spin_unlock();
    (void)thr_tkill((long)tid, SIGKILL);
    if (!t->is_main) {
        /* Bounded wait for death, observed on the kernel word.  Two
         * phases: first CONFIRM BIRTH (CHILD_SETTID lands on first
         * schedule — a zero word may mean "unborn", not "dead"), then
         * wait for the CLEARTID zero.  SIGKILL lands at the victim's
         * next boundary — microseconds, unless it is stuck in
         * uninterruptible sleep, hence the bounds. */
        thr_wait_kernel_dead(t);
    } else {
        /* Suicide of the main thread: the killer IS the victim.  SIGKILL
         * to self lands on return to user — but we are IN user, so force
         * the boundary with a yield loop; the signal kills us mid-loop.
         * If it somehow does not (masking SIGKILL is impossible), fall
         * out and exit below. */
        for (int i = 0; i < 2000; i++) {
            (void)syscall((int64_t)24, 0, 0, 0, 0, 0, 0);
        }
    }
#endif
    thr_spin_lock();
    /* A mid-wait victim died enqueued: deregister (its stack will be
     * munmap'd by the closer — a stale entry would dangle).  Post-death
     * only: dead threads cannot re-enqueue, so one walk is airtight. */
    if (t->wait_wr && t->wait_hs)
        thr_wait_dequeue(t->wait_n, t->wait_hs, t->wait_wr);
    t->wait_wr = 0;
    t->wait_hs = 0;
    t->wait_n = 0;
    /* Publish the kill (a racing ExitThread may have beaten us: then the
     * flag is already set and the victim's own teardown ran — taking
     * the code anyway would lie about who won.  The killer's code wins
     * only while the victim had not published yet. */
    if (!t->exited_word)
        t->exit_code = code;
    t->exited_word = 1;
    thr_futex_wake(&t->exited_word, 0x7fffffff);
    thr_abandon_owned(t);
    thr_live_remove(t);
    thr_spin_unlock();

    if (t->is_main) {
        /* Main-thread suicide that survived the loop: finish like
         * ExitThread for the process-leading thread.  Workers keep
         * running; the process exits with the last of them. */
        ExitThread(code);
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

/* ---- thread queries ---------------------------------------------------------------- */

W32ABI W32_HANDLE GetCurrentThread(void) {
    return W32_CURRENT_THREAD;
}

W32ABI W32_DWORD GetCurrentThreadId(void) {
    /* gettid, not getpid: on Linux getpid is the TGID (same for every
     * thread); on AuraLite both read the thread's own id. */
    return (W32_DWORD)thr_gettid();
}

W32ABI W32_DWORD ResumeThread(W32_HANDLE h) {
    struct w32_thread *t;
    W32_DWORD prev;

    if (h == W32_CURRENT_THREAD) {
        /* A running thread is never suspended: previous count 0. */
        w32_set_last_error(W32_ERROR_SUCCESS);
        return 0;
    }
    t = (struct w32_thread *)w32_handle_get_obj(h, W32_HANDLE_KIND_THREAD);
    if (!t) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return (W32_DWORD)-1;
    }
    thr_spin_lock();
    prev = t->suspend_count;
    if (!t->is_main && t->exited_word) {
        thr_spin_unlock();
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_DWORD)-1;
    }
    if (t->suspend_count > 0) {
        t->suspend_count--;
        t->start_seq++;
        thr_futex_wake(&t->start_seq, 1);
    }
    thr_spin_unlock();
    w32_set_last_error(W32_ERROR_SUCCESS);
    return prev;
}

W32ABI W32_BOOL GetThreadTimes(W32_HANDLE h, W32_FILETIME *creation,
                               W32_FILETIME *exit, W32_FILETIME *kernel,
                               W32_FILETIME *user) {
    struct w32_thread *t;
    if (h == W32_CURRENT_THREAD) {
        struct w32_teb *teb = w32_teb_self();
        t = (teb) ? (struct w32_thread *)teb->thread_obj : 0;
    } else {
        t = (struct w32_thread *)w32_handle_get_obj(h,
                                              W32_HANDLE_KIND_THREAD);
    }
    if (!t) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    /* Creation is real; CPU times are zeros (same documented limit as
     * GetProcessTimes: the kernel keeps no per-thread counters). */
    if (creation) {
        creation->dwLowDateTime = (W32_DWORD)(t->birth_ft & 0xFFFFFFFFu);
        creation->dwHighDateTime = (W32_DWORD)(t->birth_ft >> 32);
    }
    if (exit) {
        exit->dwLowDateTime = 0;
        exit->dwHighDateTime = 0;
    }
    if (kernel) {
        kernel->dwLowDateTime = 0;
        kernel->dwHighDateTime = 0;
    }
    if (user) {
        user->dwLowDateTime = 0;
        user->dwHighDateTime = 0;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI W32_BOOL GetExitCodeThread(W32_HANDLE h, W32_DWORD *code) {
    struct w32_thread *t;
    if (!code) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (h == W32_CURRENT_THREAD) {
        *code = W32_STILL_ACTIVE;
        w32_set_last_error(W32_ERROR_SUCCESS);
        return W32_TRUE;
    }
    t = (struct w32_thread *)w32_handle_get_obj(h, W32_HANDLE_KIND_THREAD);
    if (!t) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    /* Not in the ledger (no app reads thread codes); kept so the fixtures
     * can observe ExitThread/TerminateThread codes. */
    if (!t->is_main && !t->exited_word)
        *code = W32_STILL_ACTIVE;
    else
        *code = t->exit_code;
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI uint64_t SetThreadAffinityMask(W32_HANDLE h, uint64_t mask) {
    struct w32_thread *t;
    uint64_t prev;
    if (h == W32_CURRENT_THREAD) {
        struct w32_teb *teb = w32_teb_self();
        t = (teb) ? (struct w32_thread *)teb->thread_obj : 0;
    } else {
        t = (struct w32_thread *)w32_handle_get_obj(h,
                                              W32_HANDLE_KIND_THREAD);
    }
    if (!t) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    /* Recorded, not enforced: the kernel has no scheduler-affinity API,
     * so the call reports what it pinned (the request, validated) and
     * the mask is visible to later reads.  At least one online CPU must
     * be selected, like Windows. */
    if (mask == 0 || (mask & thr_online_mask) == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    prev = t->affinity_mask;
    t->affinity_mask = mask & thr_online_mask;
    w32_set_last_error(W32_ERROR_SUCCESS);
    return prev;
}

/* CloseHandle's thread reaper (called from CloseHandle, not the table:
 * the table's free_fn for threads is NULL).  On an EXITED thread this
 * reaps everything: the live-list entry (idempotent — ExitThread
 * removed it), the stack + TEB mappings (safe: this runs on ANOTHER
 * thread's stack), and the heap object.  On a RUNNING thread it only
 * marks orphaned: the thread keeps its live-list entry (ExitProcess
 * still kills it) and ExitThread frees the heap object itself; the two
 * mappings leak until process exit (documented).  The exited/orphaned
 * pair is decided under one spin hold on each side, so exactly one of
 * {self-free here} happens. */
void w32_thread_close(void *p) {
    struct w32_thread *t = (struct w32_thread *)p;
    void *stack = 0;
    uint64_t stack_sz = 0;
    struct w32_teb *teb = 0;
    int reap = 0;
    if (!t || t->is_main)
        return;
    thr_spin_lock();
    if (t->exited_word) {
        thr_live_remove(t);
        stack = t->stack_base;
        stack_sz = t->stack_size;
        teb = t->teb;
        reap = 1;
    } else {
        t->orphaned = 1;
    }
    thr_spin_unlock();
    if (!reap)
        return;
    if (stack)
        munmap(stack, (size_t)stack_sz);
    if (teb)
        munmap(teb, 4096);
    free(t);
}

/* ExitProcess support: SIGKILL every live worker but the caller, then a
 * bounded wait for their words to zero.  DllMain DETACH runs after this
 * (Windows order: threads die, then DLLs detach, then the process). */
void w32_thr_kill_all(void) {
    W32_DWORD me;
    W32_DWORD victims[THR_LIVE_MAX];
    struct w32_thread *objs[THR_LIVE_MAX];
    int n = 0;
    if (!thr_ready)
        return;
    me = (W32_DWORD)thr_gettid();
    thr_spin_lock();
    for (int i = 0; i < thr_live_n && n < THR_LIVE_MAX; i++) {
        struct w32_thread *t = thr_live[i];
        if (!t || t->is_main || t->tid == me)
            continue;
        victims[n] = t->tid;
        objs[n] = t;
        n++;
    }
    thr_spin_unlock();
#ifdef AURALITE_W32_HOST_TEST
    /* HOST: flag + tap every victim (SIGKILL would murder the group —
     * the process death below reaps stragglers anyway).  Best-effort
     * ~100 ms for checkpoints to fire, then proceed to detach. */
    for (int i = 0; i < n; i++) {
        thr_spin_lock();
        if (!objs[i]->exited_word) {
            objs[i]->kill_pending = 1;
            objs[i]->kill_code = 0;
            objs[i]->start_seq++;
        }
        thr_spin_unlock();
        thr_futex_wake(&objs[i]->start_seq, 1);
        if (victims[i])
            (void)thr_tkill((long)victims[i], THR_WAKER_SIG);
    }
    for (int i = 0; i < 100; i++) {
        int alive = 0;
        for (int k = 0; k < n; k++) {
            if (!objs[k]->exited_word) {
                alive = 1;
                break;
            }
        }
        if (!alive)
            break;
        {
            struct timespec sl = { 0, THR_POLL_NS };
            nanosleep(&sl, 0);
        }
    }
#else
    for (int i = 0; i < n; i++) {
        if (victims[i])
            (void)thr_tkill((long)victims[i], SIGKILL);
    }
    for (int i = 0; i < 2000; i++) {
        int alive = 0;
        for (int k = 0; k < n; k++) {
            if (!objs[k]->exited_word && objs[k]->tid_word != 0) {
                alive = 1;
                break;
            }
        }
        if (!alive)
            break;
        {
            struct timespec sl = { 0, THR_POLL_NS };
            nanosleep(&sl, 0);
        }
    }
#endif
    /* Whatever is still alive after the bound dies with the address
     * space: _exit tears down the process.  The bound is documented. */
}

/* ---- TlsAlloc family ------------------------------------------------------------
 *
 * Slots live in the TEB (tls_slots[64]); the bitmask allocator is global.
 * Alloc zeroes the slot in every live TEB, so a reused index never shows
 * a stale value. */

W32ABI W32_DWORD TlsAlloc(void) {
    int idx;
    thr_spin_lock();
    idx = thr_bit_alloc(&thr_tls_used, 0, W32_TEB_TLS_SLOTS);
    if (idx >= 0)
        thr_slots_zero_all(0, idx);
    thr_spin_unlock();
    if (idx < 0) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return W32_TLS_OUT_OF_INDEXES;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return (W32_DWORD)idx;
}

W32ABI void *TlsGetValue(W32_DWORD idx) {
    struct w32_teb *teb = w32_teb_self();
    if (idx >= (W32_DWORD)W32_TEB_TLS_SLOTS || !teb) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* Success MUST clear the error (the NULL-value disambiguation rule:
     * a NULL return with SUCCESS means "slot holds NULL"). */
    w32_set_last_error(W32_ERROR_SUCCESS);
    return teb->tls_slots[idx];
}

W32ABI W32_BOOL TlsSetValue(W32_DWORD idx, void *value) {
    struct w32_teb *teb = w32_teb_self();
    if (idx >= (W32_DWORD)W32_TEB_TLS_SLOTS || !teb) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    teb->tls_slots[idx] = value;
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI W32_BOOL TlsFree(W32_DWORD idx) {
    if (idx >= (W32_DWORD)W32_TEB_TLS_SLOTS) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    thr_spin_lock();
    thr_tls_used &= ~(1ull << idx);
    thr_spin_unlock();
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

/* ---- FlsAlloc family -------------------------------------------------------------
 *
 * Same shape, 128 slots + per-index callbacks (fired at thread exit for
 * non-NULL values, ascending index). */

W32ABI W32_DWORD FlsAlloc(void *callback) {
    int idx;
    thr_spin_lock();
    idx = thr_bit_alloc(&thr_fls_used_lo, &thr_fls_used_hi,
                        W32_TEB_FLS_SLOTS);
    if (idx >= 0) {
        thr_fls_cbs[idx] = (thr_fls_cb)callback;
        thr_slots_zero_all(1, idx);
    }
    thr_spin_unlock();
    if (idx < 0) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return W32_FLS_OUT_OF_INDEXES;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return (W32_DWORD)idx;
}

W32ABI void *FlsGetValue(W32_DWORD idx) {
    struct w32_teb *teb = w32_teb_self();
    if (idx >= (W32_DWORD)W32_TEB_FLS_SLOTS || !teb) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return teb->fls_slots[idx];
}

W32ABI W32_BOOL FlsSetValue(W32_DWORD idx, void *value) {
    struct w32_teb *teb = w32_teb_self();
    if (idx >= (W32_DWORD)W32_TEB_FLS_SLOTS || !teb) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    teb->fls_slots[idx] = value;
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI W32_BOOL FlsFree(W32_DWORD idx) {
    if (idx >= (W32_DWORD)W32_TEB_FLS_SLOTS) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    /* Freeing the index does NOT run callbacks (Windows rule); live
     * values stay until thread exit or reuse. */
    thr_spin_lock();
    if (idx < 64)
        thr_fls_used_lo &= ~(1ull << idx);
    else
        thr_fls_used_hi &= ~(1ull << (idx - 64));
    thr_fls_cbs[idx] = 0;
    thr_spin_unlock();
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

/* ---- APCs ---------------------------------------------------------------------------
 *
 * Per-thread FIFO, malloc'd nodes, drained at alertable points (SleepEx,
 * the *Ex waits) in the TARGET thread's own context.  QueueUserAPC never
 * runs the function — it only enqueues. */

W32ABI W32_BOOL QueueUserAPC(void *fn, W32_HANDLE hThread, uint64_t data) {
    struct w32_thread *t;
    struct thr_apc *apc;
    if (!fn) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (hThread == W32_CURRENT_THREAD) {
        struct w32_teb *teb = w32_teb_self();
        t = (teb) ? (struct w32_thread *)teb->thread_obj : 0;
    } else {
        t = (struct w32_thread *)w32_handle_get_obj(hThread,
                                              W32_HANDLE_KIND_THREAD);
    }
    if (!t) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    thr_spin_lock();
    if (!t->is_main && t->exited_word) {
        thr_spin_unlock();
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    thr_spin_unlock();
    /* Malloc OUTSIDE the spin (leaf discipline). */
    apc = (struct thr_apc *)malloc(sizeof(*apc));
    if (!apc) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return W32_FALSE;
    }
    apc->fn = (thr_apc_fn)fn;
    apc->data = data;
    apc->next = 0;
    thr_spin_lock();
    /* Re-check liveness: the thread may have died mid-malloc. */
    if (!t->is_main && t->exited_word) {
        thr_spin_unlock();
        free(apc);
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (t->apc_tail)
        t->apc_tail->next = apc;
    else
        t->apc_head = apc;
    t->apc_tail = apc;
    thr_spin_unlock();
    /* No wake: alertable waits poll at 1 ms, so the APC lands within one
     * slice without a kernel multiwait.  A thread in a NON-alertable wait
     * (or running user code) sees it at its next alertable point. */
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

/* Drain the CURRENT thread's APCs.  Returns 1 if any ran.  Nodes are
 * detached under the spin and run outside it (user code). */
static int thr_drain_apcs(void) {
    struct w32_teb *teb = w32_teb_self();
    struct w32_thread *t;
    struct thr_apc *apc;
    int ran = 0;
    if (!teb || !teb->thread_obj)
        return 0;
    t = (struct w32_thread *)teb->thread_obj;
    for (;;) {
        thr_spin_lock();
        apc = t->apc_head;
        if (apc) {
            t->apc_head = apc->next;
            if (!t->apc_head)
                t->apc_tail = 0;
        }
        thr_spin_unlock();
        if (!apc)
            return ran;
        ran = 1;
        apc->fn(apc->data);
        free(apc);
        /* An APC may queue more APCs (to self or others); the loop
         * picks self-queued ones up in the same drain. */
    }
}

/* SleepEx, alertable for real (moved from kernel32_ps.c, which ignored
 * `alertable` for want of APCs).  Returns WAIT_IO_COMPLETION if APCs ran,
 * else 0 after the full sleep. */
W32ABI W32_DWORD SleepEx(W32_DWORD ms, W32_BOOL alertable) {
    if (!alertable) {
        Sleep(ms);
        return 0;
    }
    if (ms == W32_INFINITE) {
        for (;;) {
            struct timespec sl = { 0, THR_POLL_NS };
            thr_checkpoint();
            if (thr_drain_apcs())
                return W32_WAIT_IO_COMPLETION;
            nanosleep(&sl, 0);
        }
    } else {
        uint64_t deadline = GetTickCount64() + ms;
        for (;;) {
            struct timespec sl = { 0, THR_POLL_NS };
            thr_checkpoint();
            if (thr_drain_apcs())
                return W32_WAIT_IO_COMPLETION;
            if ((int64_t)(GetTickCount64() - deadline) >= 0)
                return 0;
            nanosleep(&sl, 0);
        }
    }
}

/* ---- critical sections ------------------------------------------------------------------
 *
 * 24 bytes (fits any RTL_CRITICAL_SECTION an app allocates).  Three-state
 * lock word: 0 free, 1 held uncontended, 2 held contended — the 1/2 split
 * avoids futex syscalls on uncontended Leave.  Recursive, owned by tid.
 * Enter spins `spin` times before blocking (the SpinCount contract). */

static void thr_cs_init_common(W32_CRITICAL_SECTION *cs, W32_DWORD spin) {
    cs->lock = 0;
    cs->recursion = 0;
    cs->owner_tid = 0;
    cs->spin = spin;
    cs->waiters = 0;
    cs->pad = 0;
}

W32ABI W32_BOOL InitializeCriticalSection(W32_CRITICAL_SECTION *cs) {
    if (!cs) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    thr_cs_init_common(cs, 0);
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI W32_BOOL InitializeCriticalSectionAndSpinCount(W32_CRITICAL_SECTION *cs,
                                                      W32_DWORD spin) {
    if (!cs) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    /* Bit 31 (PREALLOCATE_EVENT) is accepted and ignored: we never
     * allocate a kernel event object. */
    thr_cs_init_common(cs, spin & 0x7FFFFFFFu);
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI W32_BOOL InitializeCriticalSectionEx(W32_CRITICAL_SECTION *cs,
                                            W32_DWORD spin,
                                            W32_DWORD flags) {
    if (!cs || (flags & ~0x3u)) {
        /* 0x1 PREALLOCATE, 0x2 DEBUG_INFO: accepted, both no-ops. */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    thr_cs_init_common(cs, spin & 0x7FFFFFFFu);
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI void EnterCriticalSection(W32_CRITICAL_SECTION *cs) {
    W32_DWORD me;
    W32_DWORD spin;
    if (!cs)
        return;
    me = (W32_DWORD)thr_gettid();
    thr_checkpoint();   /* lock-spinners observe the kill without blocking */
    /* Fast path: free, or already ours (recursion). */
    if (__sync_bool_compare_and_swap(&cs->lock, 0, 1)) {
        cs->owner_tid = me;
        cs->recursion = 1;
        return;
    }
    if (cs->owner_tid == me) {
        cs->recursion++;
        return;
    }
    /* Slow path: spin, then mark contended and block. */
    spin = cs->spin;
    for (W32_DWORD i = 0; i < spin; i++) {
        if (__sync_bool_compare_and_swap(&cs->lock, 0, 2)) {
            cs->owner_tid = me;
            cs->recursion = 1;
            return;
        }
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile ("pause");
#endif
    }
    for (;;) {
        W32_DWORD prev = __sync_lock_test_and_set(&cs->lock, 2);
        if (prev == 0) {
            cs->owner_tid = me;
            cs->recursion = 1;
            return;
        }
        if (cs->owner_tid == me && cs->lock != 0) {
            /* The owner released and re-acquired between our reads;
             * re-check recursion (owner_tid is only written by the
             * holder, so equality means it is ours NOW). */
            cs->recursion++;
            return;
        }
        __sync_fetch_and_add(&cs->waiters, 1);
        (void)thr_futex_wait(&cs->lock, 2);
        __sync_fetch_and_add(&cs->waiters, (W32_DWORD)-1);
    }
}

W32ABI void LeaveCriticalSection(W32_CRITICAL_SECTION *cs) {
    if (!cs)
        return;
    thr_checkpoint();
    /* Releasing a section owned by another thread (or no thread) is
     * undefined on Windows; we refuse to corrupt: only the owner
     * unwinds. */
    if (cs->owner_tid != (W32_DWORD)thr_gettid() || cs->lock == 0)
        return;
    if (--cs->recursion > 0)
        return;
    cs->owner_tid = 0;
    if (__sync_lock_test_and_set(&cs->lock, 0) == 2) {
        /* Was contended: wake one waiter (it re-contends if it loses). */
        (void)thr_futex_wake(&cs->lock, 1);
    }
}

W32ABI void DeleteCriticalSection(W32_CRITICAL_SECTION *cs) {
    if (!cs)
        return;
    /* Nothing was allocated (no kernel event object), so deletion is a
     * state reset.  Deleting a HELD section is app-undefined; we leave
     * the lock word alone and clear the bookkeeping. */
    cs->recursion = 0;
    cs->owner_tid = 0;
    cs->spin = 0;
    cs->waiters = 0;
}

/* ---- exclusive-only SRW --------------------------------------------------------------------
 *
 * 8 bytes, zero-init valid (apps static-init; InitializeSRWLock is not in
 * the ledger).  One writer, no readers: the word is 0 free / tid held.
 * Recursive acquisition by the owner deadlocks on Windows too (undefined);
 * ours spins forever the same way — documented, never fixture-pinned. */

W32ABI void AcquireSRWLockExclusive(W32_SRWLOCK *lock) {
    W32_DWORD me;
    if (!lock)
        return;
    me = (W32_DWORD)thr_gettid();
    for (;;) {
        thr_checkpoint();
        if (__sync_bool_compare_and_swap(&lock->word, 0, me))
            return;
        (void)thr_futex_wait(&lock->word, lock->word);
    }
}

W32ABI W32_BOOL TryAcquireSRWLockExclusive(W32_SRWLOCK *lock) {
    if (!lock)
        return W32_FALSE;
    return __sync_bool_compare_and_swap(&lock->word, 0,
                                        (W32_DWORD)thr_gettid());
}

W32ABI void ReleaseSRWLockExclusive(W32_SRWLOCK *lock) {
    if (!lock)
        return;
    __sync_lock_test_and_set(&lock->word, 0);
    /* One waiter: exclusive-only has no reader herds.  A redundant wake
     * with no waiters is a cheap syscall, not a bug. */
    (void)thr_futex_wake(&lock->word, 1);
}

/* ---- condition variables (SRW-only wake-all) -----------------------------------------------
 *
 * 8 bytes, zero-init valid.  The seq word counts WakeAlls; sleepers wait
 * for it to change.  Spurious wakeups are possible (Windows allows them):
 * callers re-test their predicate in a loop, the documented contract. */

W32ABI W32_BOOL SleepConditionVariableSRW(W32_CONDITION_VARIABLE *cv,
                                          W32_SRWLOCK *lock,
                                          W32_DWORD ms, W32_DWORD flags) {
    uint32_t seq;
    (void)flags;   /* CONDITION_VARIABLE_LOCKMODE_SHARED: refused below */
    if (!cv || !lock) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (flags != 0) {
        /* Shared-mode sleep needs shared SRW, which the ledger excludes. */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    seq = cv->seq;
    ReleaseSRWLockExclusive(lock);
    if (ms == W32_INFINITE) {
        for (;;) {
            thr_checkpoint();
            (void)thr_futex_wait(&cv->seq, seq);
            if (cv->seq != seq)
                break;
        }
    } else {
        uint64_t deadline = GetTickCount64() + ms;
        for (;;) {
            struct timespec sl = { 0, THR_POLL_NS };
            thr_checkpoint();
            if (cv->seq != seq)
                break;
            if ((int64_t)(GetTickCount64() - deadline) >= 0) {
                AcquireSRWLockExclusive(lock);
                w32_set_last_error(W32_ERROR_WAIT_TIMEOUT);
                return W32_FALSE;
            }
            nanosleep(&sl, 0);
        }
    }
    AcquireSRWLockExclusive(lock);
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI void WakeAllConditionVariable(W32_CONDITION_VARIABLE *cv) {
    if (!cv)
        return;
    __sync_fetch_and_add(&cv->seq, 1);
    (void)thr_futex_wake(&cv->seq, 0x7FFFFFFF);
}

/* ---- InitOnce (Begin/Complete) ----------------------------------------------------------------
 *
 * 8 bytes, zero-init valid (INIT_ONCE_STATIC_INIT).  States: 0 unrun,
 * 1 running (by the winner's tid in the high half... simpler: 1 running,
 * 2 done).  Losers block on the word until it leaves 1. */

W32ABI W32_BOOL InitOnceBeginInitialize(W32_INIT_ONCE *once, W32_DWORD flags,
                                        W32_BOOL *pending, void **ctx) {
    (void)flags;
    (void)ctx;   /* async variants: none, flags must be 0 */
    if (!once || !pending) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (flags != 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    for (;;) {
        W32_DWORD st = once->state;
        if (st == 2) {
            *pending = W32_FALSE;
            w32_set_last_error(W32_ERROR_SUCCESS);
            return W32_TRUE;
        }
        if (st == 0 &&
            __sync_bool_compare_and_swap(&once->state, 0, 1)) {
            *pending = W32_TRUE;
            w32_set_last_error(W32_ERROR_SUCCESS);
            return W32_TRUE;
        }
        (void)thr_futex_wait(&once->state, 1);
    }
}

W32ABI W32_BOOL InitOnceComplete(W32_INIT_ONCE *once, W32_DWORD flags,
                                 void *ctx) {
    (void)flags;
    (void)ctx;
    if (!once) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (flags != 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    __sync_lock_test_and_set(&once->state, 2);
    (void)thr_futex_wake(&once->state, 0x7FFFFFFF);
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

/* ---- SListHead + Flush ---------------------------------------------------------------------------
 *
 * 16-byte header, 16-byte CAS (cmpxchg16b via __int128 atomics).  Depth
 * and Sequence are maintained; Flush exchanges the whole header with
 * {NULL,0,seq+1} and returns the old chain.  Push/Pop are NOT in the
 * ledger: apps grow lists with their own interlocked code or not at all. */

W32ABI void InitializeSListHead(W32_SLIST_HEADER *head) {
    if (!head)
        return;
    head->next = 0;
    head->depth = 0;
    memset(head->seq, 0, sizeof(head->seq));
}

/* NOTE: the guest userland builds for the x86-64 baseline WITHOUT -mcx16
 * and ships no libatomic, so a 16-byte __atomic CAS would link-fail in the
 * guest.  The swap below is serialised by a process-wide spinlock instead:
 * still atomic to every observer, just not lock-free.  No test pins
 * lock-freedom (it is unobservable through this API surface).
 * The serialiser is the global G spinlock (futex-backed). */
W32ABI void *InterlockedFlushSList(W32_SLIST_HEADER *head) {
    void *ret;
    if (!head)
        return 0;
    thr_spin_lock();
    ret = head->next;
    head->next = 0;
    head->depth = 0;
    head->seq[0]++;   /* keep a sequence ticking like the real header */
    thr_spin_unlock();
    return ret;
}

/* ---- events --------------------------------------------------------------------------------
 *
 * Manual-reset latches or auto-reset pulses.  Auto-reset handoff is under
 * the spin: exactly one waiter consumes the signal, the rest keep waiting.
 * Same-name opens return the same object (process-local table). */

/* W -> UTF-8 name dup, capped at 63 bytes (the registry slots are
 * fixed).  Same two-pass shape as ps_w16_dup (kernel32_ps.c). */
static char *thr_dup_name(const W32_WCHAR *name) {
    size_t n;
    size_t need = 0;
    char *out;
    if (!name)
        return 0;
    n = w32_utf16_len(name, 64);
    if (n >= 64)
        return 0;
    if (w32_utf16_to_utf8(name, n, NULL, 0, &need) != W32_UTF_OK)
        return 0;
    if (need >= 64)
        return 0;
    out = (char *)malloc(need + 1);
    if (!out)
        return 0;
    if (w32_utf16_to_utf8(name, n, out, need, NULL) != W32_UTF_OK) {
        free(out);
        return 0;
    }
    out[need] = '\0';
    return out;
}

static W32_HANDLE thr_event_create(int manual, int initial, const char *name8) {
    struct w32_thr_event *ev;
    W32_HANDLE h;
    thr_spin_lock();
    ev = (struct w32_thr_event *)thr_name_lookup(W32_HANDLE_KIND_EVENT,
                                                 name8);
    if (ev) {
        ev->refs++;
        thr_spin_unlock();
        h = w32_handle_alloc_obj(W32_HANDLE_KIND_EVENT, ev, NULL);
        if (!h) {
            thr_spin_lock();
            ev->refs--;
            thr_spin_unlock();
            w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
            return 0;
        }
        w32_set_last_error(W32_ERROR_ALREADY_EXISTS);
        return h;
    }
    thr_spin_unlock();
    ev = (struct w32_thr_event *)calloc(1, sizeof(*ev));
    if (!ev) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    ev->manual = manual;
    ev->signaled = initial ? 1 : 0;
    ev->refs = 1;
    if (name8) {
        ev->name = malloc(strlen(name8) + 1);
        if (ev->name)
            strcpy(ev->name, name8);
    }
    thr_spin_lock();
    if (ev->name && thr_name_insert(W32_HANDLE_KIND_EVENT, name8, ev) != 0) {
        /* Table full: the event still works, it just has no name. */
        free(ev->name);
        ev->name = 0;
    }
    thr_spin_unlock();
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_EVENT, ev, NULL);
    if (!h) {
        thr_spin_lock();
        thr_name_remove(ev);
        thr_spin_unlock();
        free(ev->name);
        free(ev);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return 0;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return h;
}

W32ABI W32_HANDLE CreateEventA(void *security, W32_BOOL manual,
                               W32_BOOL initial, const char *name) {
    (void)security;
    return thr_event_create(manual ? 1 : 0, initial ? 1 : 0, name);
}

W32ABI W32_HANDLE CreateEventW(void *security, W32_BOOL manual,
                               W32_BOOL initial, const W32_WCHAR *name) {
    char *n8;
    W32_HANDLE h;
    (void)security;
    if (!name)
        return thr_event_create(manual ? 1 : 0, initial ? 1 : 0, 0);
    n8 = thr_dup_name(name);
    if (!n8) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    h = thr_event_create(manual ? 1 : 0, initial ? 1 : 0, n8);
    free(n8);
    return h;
}

W32ABI W32_BOOL SetEvent(W32_HANDLE h) {
    struct w32_thr_event *ev =
        (struct w32_thr_event *)w32_handle_get_obj(h, W32_HANDLE_KIND_EVENT);
    if (!ev) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    thr_spin_lock();
    ev->signaled = 1;
    thr_waitable_wake(&ev->w);
    thr_spin_unlock();
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

W32ABI W32_BOOL ResetEvent(W32_HANDLE h) {
    struct w32_thr_event *ev =
        (struct w32_thr_event *)w32_handle_get_obj(h, W32_HANDLE_KIND_EVENT);
    if (!ev) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    thr_spin_lock();
    ev->signaled = 0;
    thr_spin_unlock();
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

void w32_event_close(void *p) {
    struct w32_thr_event *ev = (struct w32_thr_event *)p;
    int drop = 0;
    if (!ev)
        return;
    thr_spin_lock();
    if (--ev->refs <= 0) {
        thr_name_remove(ev);
        drop = 1;
    }
    thr_spin_unlock();
    if (drop) {
        free(ev->name);
        free(ev);
    }
}

/* ---- mutexes ---------------------------------------------------------------------------------
 *
 * Recursive, owned by tid, abandoned when the owner dies holding them.
 * The owned-list is intrusive (no alloc on acquire).  Same-name opens
 * return the same object. */

static W32_HANDLE thr_mutex_create(int initial, const char *name8) {
    struct w32_thr_mutex *m;
    W32_HANDLE h;
    thr_spin_lock();
    m = (struct w32_thr_mutex *)thr_name_lookup(W32_HANDLE_KIND_MUTEX,
                                                name8);
    if (m) {
        m->refs++;
        thr_spin_unlock();
        h = w32_handle_alloc_obj(W32_HANDLE_KIND_MUTEX, m, NULL);
        if (!h) {
            thr_spin_lock();
            m->refs--;
            thr_spin_unlock();
            w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
            return 0;
        }
        w32_set_last_error(W32_ERROR_ALREADY_EXISTS);
        return h;
    }
    thr_spin_unlock();
    m = (struct w32_thr_mutex *)calloc(1, sizeof(*m));
    if (!m) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    m->refs = 1;
    if (initial) {
        W32_DWORD me = (W32_DWORD)thr_gettid();
        struct w32_teb *teb = w32_teb_self();
        m->owner_tid = me;
        m->recursion = 1;
        if (teb && teb->thread_obj) {
            struct w32_thread *t = (struct w32_thread *)teb->thread_obj;
            thr_spin_lock();
            m->owner_next = t->owned;
            t->owned = m;
            thr_spin_unlock();
        }
    }
    if (name8) {
        m->name = malloc(strlen(name8) + 1);
        if (m->name)
            strcpy(m->name, name8);
    }
    thr_spin_lock();
    if (m->name && thr_name_insert(W32_HANDLE_KIND_MUTEX, name8, m) != 0) {
        free(m->name);
        m->name = 0;
    }
    thr_spin_unlock();
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_MUTEX, m, NULL);
    if (!h) {
        thr_spin_lock();
        thr_name_remove(m);
        thr_spin_unlock();
        free(m->name);
        free(m);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return 0;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return h;
}

W32ABI W32_HANDLE CreateMutexA(void *security, W32_BOOL initial,
                               const char *name) {
    (void)security;
    return thr_mutex_create(initial ? 1 : 0, name);
}

W32ABI W32_HANDLE CreateMutexW(void *security, W32_BOOL initial,
                               const W32_WCHAR *name) {
    char *n8;
    W32_HANDLE h;
    (void)security;
    if (!name)
        return thr_mutex_create(initial ? 1 : 0, 0);
    n8 = thr_dup_name(name);
    if (!n8) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    h = thr_mutex_create(initial ? 1 : 0, n8);
    free(n8);
    return h;
}

/* Acquire, assuming the spin is held.  Returns 1 if the mutex was taken
 * (or abandoned-to-us), with *abandoned set when the previous owner died
 * holding it.  A dead owner is detected by its tid word: it zeroes only
 * at kernel teardown, so a zero word means the owner cannot come back. */
static int thr_mutex_try_take(struct w32_thr_mutex *m, W32_DWORD me,
                              struct w32_thread *t, int *abandoned) {
    *abandoned = 0;
    if (m->owner_tid == 0) {
        if (m->abandoned) {
            m->abandoned = 0;
            *abandoned = 1;
        }
        m->owner_tid = me;
        m->recursion = 1;
        if (t) {
            m->owner_next = t->owned;
            t->owned = m;
        }
        return 1;
    }
    if (m->owner_tid == me) {
        m->recursion++;
        return 1;
    }
    /* Held by another: is the owner dead?  Scan the live list for the
     * owner's word (a dead thread leaves the list, so absence == dead). */
    {
        int owner_alive = 0;
        for (int i = 0; i < thr_live_n; i++) {
            struct w32_thread *o = thr_live[i];
            if (o && o->tid == m->owner_tid) {
                owner_alive = (o->is_main || !o->exited_word);
                break;
            }
        }
        if (!owner_alive) {
            /* Abandoned mid-wait (the TerminateThread race window):
             * take it and report. */
            m->owner_tid = me;
            m->recursion = 1;
            m->abandoned = 0;
            if (t) {
                m->owner_next = t->owned;
                t->owned = m;
            }
            *abandoned = 1;
            return 1;
        }
    }
    return 0;
}

W32ABI W32_BOOL ReleaseMutex(W32_HANDLE h) {
    struct w32_thr_mutex *m =
        (struct w32_thr_mutex *)w32_handle_get_obj(h, W32_HANDLE_KIND_MUTEX);
    W32_DWORD me;
    if (!m) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    me = (W32_DWORD)thr_gettid();
    thr_spin_lock();
    if (m->owner_tid != me || m->owner_tid == 0) {
        thr_spin_unlock();
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (--m->recursion > 0) {
        thr_spin_unlock();
        w32_set_last_error(W32_ERROR_SUCCESS);
        return W32_TRUE;
    }
    m->owner_tid = 0;
    /* Unlink from the owner's list. */
    {
        struct w32_teb *teb = w32_teb_self();
        if (teb && teb->thread_obj) {
            struct w32_thread *t = (struct w32_thread *)teb->thread_obj;
            struct w32_thr_mutex **p = &t->owned;
            while (*p) {
                if (*p == m) {
                    *p = m->owner_next;
                    break;
                }
                p = &(*p)->owner_next;
            }
        }
    }
    m->owner_next = 0;
    thr_waitable_wake(&m->w);
    thr_spin_unlock();
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

void w32_mutex_close(void *p) {
    struct w32_thr_mutex *m = (struct w32_thr_mutex *)p;
    int drop = 0;
    if (!m)
        return;
    thr_spin_lock();
    if (--m->refs <= 0) {
        thr_name_remove(m);
        drop = 1;
    }
    thr_spin_unlock();
    if (drop) {
        free(m->name);
        free(m);
    }
}

/* ---- semaphores ----------------------------------------------------------------------------------
 *
 * Count + max, FIFO-ish wake order (whatever the waiter list yields —
 * Windows makes no order promise either). */

static W32_HANDLE thr_sem_create(W32_LONG initial, W32_LONG max,
                                 const char *name8) {
    struct w32_thr_sem *s;
    W32_HANDLE h;
    if (initial < 0 || max <= 0 || initial > max) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    thr_spin_lock();
    s = (struct w32_thr_sem *)thr_name_lookup(W32_HANDLE_KIND_SEMAPHORE,
                                              name8);
    if (s) {
        s->refs++;
        thr_spin_unlock();
        h = w32_handle_alloc_obj(W32_HANDLE_KIND_SEMAPHORE, s, NULL);
        if (!h) {
            thr_spin_lock();
            s->refs--;
            thr_spin_unlock();
            w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
            return 0;
        }
        w32_set_last_error(W32_ERROR_ALREADY_EXISTS);
        return h;
    }
    thr_spin_unlock();
    s = (struct w32_thr_sem *)calloc(1, sizeof(*s));
    if (!s) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    s->count = initial;
    s->max = max;
    s->refs = 1;
    if (name8) {
        s->name = malloc(strlen(name8) + 1);
        if (s->name)
            strcpy(s->name, name8);
    }
    thr_spin_lock();
    if (s->name && thr_name_insert(W32_HANDLE_KIND_SEMAPHORE, name8, s) != 0) {
        free(s->name);
        s->name = 0;
    }
    thr_spin_unlock();
    h = w32_handle_alloc_obj(W32_HANDLE_KIND_SEMAPHORE, s, NULL);
    if (!h) {
        thr_spin_lock();
        thr_name_remove(s);
        thr_spin_unlock();
        free(s->name);
        free(s);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return 0;
    }
    w32_set_last_error(W32_ERROR_SUCCESS);
    return h;
}

W32ABI W32_HANDLE CreateSemaphoreW(void *security, W32_LONG initial,
                                   W32_LONG max, const W32_WCHAR *name) {
    char *n8;
    W32_HANDLE h;
    (void)security;
    /* No CreateSemaphoreA in the ledger: the A row stays a stub. */
    if (!name)
        return thr_sem_create(initial, max, 0);
    n8 = thr_dup_name(name);
    if (!n8) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    h = thr_sem_create(initial, max, n8);
    free(n8);
    return h;
}

W32ABI W32_BOOL ReleaseSemaphore(W32_HANDLE h, W32_LONG count,
                                 W32_LONG *prev) {
    struct w32_thr_sem *s =
        (struct w32_thr_sem *)w32_handle_get_obj(h, W32_HANDLE_KIND_SEMAPHORE);
    if (!s) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (count <= 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    thr_spin_lock();
    if (s->count + count > s->max) {
        thr_spin_unlock();
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (prev)
        *prev = s->count;
    s->count += count;
    thr_waitable_wake(&s->w);
    thr_spin_unlock();
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_TRUE;
}

void w32_sem_close(void *p) {
    struct w32_thr_sem *s = (struct w32_thr_sem *)p;
    int drop = 0;
    if (!s)
        return;
    thr_spin_lock();
    if (--s->refs <= 0) {
        thr_name_remove(s);
        drop = 1;
    }
    thr_spin_unlock();
    if (drop) {
        free(s->name);
        free(s);
    }
}

/* ---- the waits --------------------------------------------------------------------------------------
 *
 * One core (thr_wait_n) serves all three waits.  Each iteration, under the
 * spin: test every object (acquiring mutexes/semaphores/auto-events as a
 * side effect when satisfied); if nothing is satisfied and the deadline
 * allows, enqueue one private waiter node on every waitable object and
 * block on the private word (INFINITE, exact) or poll (finite/alertable,
 * 1 ms slices).  Ex variants drain APCs per iteration and return
 * WAIT_IO_COMPLETION when any ran without an object firing.
 *
 * WaitAll on mutexes acquires one by one (Windows acquires atomically;
 * the deviation is documented — no ladder app waits All on mutexes). */

#define THR_MAXIMUM_WAIT_OBJECTS 64

/* Test-and-acquire object i.  Spin HELD.  Returns 1 when satisfied (with
 * *abandoned set for an abandoned mutex take). */
static int thr_wait_test_one(W32_HANDLE h, W32_DWORD me,
                             struct w32_thread *self, int *abandoned) {
    int kind;
    *abandoned = 0;
    if (h == W32_CURRENT_THREAD) {
        /* Waiting on self: satisfied only if... never (a running thread
         * is not dead).  Falls through to block/timeout. */
        return 0;
    }
    kind = w32_handle_kind(h);
    if (kind == W32_HANDLE_KIND_THREAD) {
        struct w32_thread *t =
            (struct w32_thread *)w32_handle_get_obj(h,
                                              W32_HANDLE_KIND_THREAD);
        if (!t)
            return -1;
        if (t->is_main || !t->exited_word)
            return 0;
        /* Dead: re-wake so a CHAIN of waiters all observe it (one wake
         * per waiter keeps the whole chain moving). */
        thr_futex_wake((volatile uint32_t *)&t->exited_word, 1);
        return 1;
    }
    if (kind == W32_HANDLE_KIND_EVENT) {
        struct w32_thr_event *ev =
            (struct w32_thr_event *)w32_handle_get_obj(h,
                                                 W32_HANDLE_KIND_EVENT);
        if (!ev)
            return -1;
        if (!ev->signaled)
            return 0;
        if (!ev->manual)
            ev->signaled = 0;   /* auto-reset: consume */
        return 1;
    }
    if (kind == W32_HANDLE_KIND_MUTEX) {
        struct w32_thr_mutex *m =
            (struct w32_thr_mutex *)w32_handle_get_obj(h,
                                                 W32_HANDLE_KIND_MUTEX);
        if (!m)
            return -1;
        return thr_mutex_try_take(m, me, self, abandoned);
    }
    if (kind == W32_HANDLE_KIND_SEMAPHORE) {
        struct w32_thr_sem *s =
            (struct w32_thr_sem *)w32_handle_get_obj(h,
                                              W32_HANDLE_KIND_SEMAPHORE);
        if (!s)
            return -1;
        if (s->count <= 0)
            return 0;
        s->count--;
        return 1;
    }
    if (kind == W32_HANDLE_KIND_PROC) {
        /* A process handle (W32A-2 object): signaled when the child
         * exited.  w32_ps_is_exited reaps into the exit ring. */
        return w32_ps_is_exited(h) ? 1 : 0;
    }
    return -1;   /* files, maps, finds: not waitable */
}

/* Enqueue wr on every waitable object of the set.  Spin HELD. */
static void thr_wait_enqueue(int n, W32_HANDLE *hs, struct thr_waiter *wr) {
    for (int i = 0; i < n; i++) {
        int kind;
        if (hs[i] == W32_CURRENT_THREAD)
            continue;
        kind = w32_handle_kind(hs[i]);
        if (kind == W32_HANDLE_KIND_EVENT) {
            struct w32_thr_event *ev =
                (struct w32_thr_event *)w32_handle_get_obj(hs[i],
                                                     W32_HANDLE_KIND_EVENT);
            if (ev)
                thr_waiter_add(&ev->w, wr);
        } else if (kind == W32_HANDLE_KIND_MUTEX) {
            struct w32_thr_mutex *m =
                (struct w32_thr_mutex *)w32_handle_get_obj(hs[i],
                                                     W32_HANDLE_KIND_MUTEX);
            if (m)
                thr_waiter_add(&m->w, wr);
        } else if (kind == W32_HANDLE_KIND_SEMAPHORE) {
            struct w32_thr_sem *s =
                (struct w32_thr_sem *)w32_handle_get_obj(hs[i],
                                                  W32_HANDLE_KIND_SEMAPHORE);
            if (s)
                thr_waiter_add(&s->w, wr);
        }
        /* Threads and processes have no waiter list: threads are watched
         * via the tid word (re-checked per iteration; the exit wake hits
         * the word directly), processes via the exit ring.  An INFINITE
         * single wait on a thread blocks on the tid word itself (see
         * below); multi-waits involving threads poll the words at 1 ms
         * (documented: no kernel multiwait). */
    }
}

static void thr_wait_dequeue(int n, W32_HANDLE *hs, struct thr_waiter *wr) {
    for (int i = 0; i < n; i++) {
        int kind;
        if (hs[i] == W32_CURRENT_THREAD)
            continue;
        kind = w32_handle_kind(hs[i]);
        if (kind == W32_HANDLE_KIND_EVENT) {
            struct w32_thr_event *ev =
                (struct w32_thr_event *)w32_handle_get_obj(hs[i],
                                                     W32_HANDLE_KIND_EVENT);
            if (ev)
                thr_waiter_del(&ev->w, wr);
        } else if (kind == W32_HANDLE_KIND_MUTEX) {
            struct w32_thr_mutex *m =
                (struct w32_thr_mutex *)w32_handle_get_obj(hs[i],
                                                     W32_HANDLE_KIND_MUTEX);
            if (m)
                thr_waiter_del(&m->w, wr);
        } else if (kind == W32_HANDLE_KIND_SEMAPHORE) {
            struct w32_thr_sem *s =
                (struct w32_thr_sem *)w32_handle_get_obj(hs[i],
                                                  W32_HANDLE_KIND_SEMAPHORE);
            if (s)
                thr_waiter_del(&s->w, wr);
        }
    }
}

static W32_DWORD thr_wait_n(int n, W32_HANDLE *hs, int wait_all,
                            W32_DWORD ms, int alertable) {
    struct w32_teb *teb = w32_teb_self();
    struct w32_thread *self =
        (teb) ? (struct w32_thread *)teb->thread_obj : 0;
    W32_DWORD me = (W32_DWORD)thr_gettid();
    uint64_t deadline = 0;
    int infinite = (ms == W32_INFINITE);
    int set_has_thread = 0;
    int set_has_proc = 0;
    struct thr_waiter wr;

    if (n <= 0 || n > THR_MAXIMUM_WAIT_OBJECTS || !hs) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_WAIT_FAILED;
    }
    /* Validate the set up front (bad handles fail, never block). */
    for (int i = 0; i < n; i++) {
        int kind;
        if (hs[i] == W32_CURRENT_THREAD) {
            set_has_thread = 1;
            continue;
        }
        kind = w32_handle_kind(hs[i]);
        if (kind == W32_HANDLE_KIND_THREAD)
            set_has_thread = 1;
        else if (kind == W32_HANDLE_KIND_PROC)
            set_has_proc = 1;
        else if (kind != W32_HANDLE_KIND_EVENT &&
                 kind != W32_HANDLE_KIND_MUTEX &&
                 kind != W32_HANDLE_KIND_SEMAPHORE) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return W32_WAIT_FAILED;
        } else if (!w32_handle_get_obj(hs[i], kind)) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return W32_WAIT_FAILED;
        }
    }
    if (!infinite)
        deadline = GetTickCount64() + ms;

    /* Fast lane: an INFINITE single wait on one thread blocks on the
     * monotone exited flag directly — no polling, exact wakeup, and no
     * birth-window race (the flag starts 0 == alive and only death
     * sets it, unlike the SETTID word which reads 0 while unborn). */
    if (infinite && !alertable && n == 1 && !wait_all &&
        hs[0] != W32_CURRENT_THREAD &&
        w32_handle_kind(hs[0]) == W32_HANDLE_KIND_THREAD) {
        struct w32_thread *t =
            (struct w32_thread *)w32_handle_get_obj(hs[0],
                                              W32_HANDLE_KIND_THREAD);
        if (!t) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return W32_WAIT_FAILED;
        }
        for (;;) {
            uint32_t w;
            thr_checkpoint();   /* joiners die too when killed */
            w = t->exited_word;

            if (w != 0) {
                thr_futex_wake((volatile uint32_t *)&t->exited_word, 1);
                w32_set_last_error(W32_ERROR_SUCCESS);
                return W32_WAIT_OBJECT_0;
            }
            (void)thr_futex_wait((volatile uint32_t *)&t->exited_word, w);
        }
    }

    /* General lane: test under the spin; block or poll per iteration. */
    wr.next = 0;
    wr.word = 0;
    for (;;) {
        thr_checkpoint();   /* every re-test is a kill point */
        int fire = -1;
        int abandoned = 0;
        int all_ok;
        thr_spin_lock();
        if (!wait_all) {
            for (int i = 0; i < n; i++) {
                int ab = 0;
                int r = thr_wait_test_one(hs[i], me, self, &ab);
                if (r < 0) {
                    thr_spin_unlock();
                    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
                    return W32_WAIT_FAILED;
                }
                if (r) {
                    fire = i;
                    abandoned = ab;
                    break;
                }
            }
        } else {
            /* WaitAll: every object must test true IN ONE hold of the
             * spin (mutexes already acquired stay acquired on partial
             * failure — the documented deviation). */
            all_ok = 1;
            for (int i = 0; i < n; i++) {
                int ab = 0;
                int r = thr_wait_test_one(hs[i], me, self, &ab);
                if (r < 0) {
                    thr_spin_unlock();
                    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
                    return W32_WAIT_FAILED;
                }
                if (!r) {
                    all_ok = 0;
                    break;
                }
                if (ab)
                    abandoned = 1;
            }
            if (all_ok)
                fire = 0;
        }
        if (fire >= 0) {
            thr_spin_unlock();
            w32_set_last_error(abandoned ?
                               W32_ERROR_ABANDONED_WAIT_0 :
                               W32_ERROR_SUCCESS);
            if (wait_all)
                return abandoned ? W32_WAIT_ABANDONED_0 : W32_WAIT_OBJECT_0;
            return (abandoned ? W32_WAIT_ABANDONED_0 : W32_WAIT_OBJECT_0) +
                   (W32_DWORD)fire;
        }
        /* Nothing fired.  Deadline first (a zero timeout returns here). */
        if (!infinite && (int64_t)(GetTickCount64() - deadline) >= 0) {
            thr_spin_unlock();
            w32_set_last_error(W32_ERROR_WAIT_TIMEOUT);
            return W32_WAIT_TIMEOUT;
        }
        if (alertable) {
            thr_spin_unlock();
            if (thr_drain_apcs()) {
                w32_set_last_error(W32_ERROR_SUCCESS);
                return W32_WAIT_IO_COMPLETION;
            }
        } else {
            /* Enqueue on the waitable objects for the exact-wake lane. */
            thr_wait_enqueue(n, hs, &wr);
            if (self) {
                self->wait_wr = &wr;
                self->wait_hs = hs;
                self->wait_n = n;
            }
            thr_spin_unlock();
        }
        if (alertable || set_has_thread || set_has_proc || !infinite) {
            /* Poll lane: 1 ms slices (futex has no timeout; threads and
             * processes have no waiter list; APCs need a pulse). */
            struct timespec sl = { 0, THR_POLL_NS };
            nanosleep(&sl, 0);
            if (!alertable) {
                thr_spin_lock();
                thr_wait_dequeue(n, hs, &wr);
                if (self) {
                    self->wait_wr = 0;
                    self->wait_hs = 0;
                    self->wait_n = 0;
                }
                thr_spin_unlock();
            }
        } else {
            /* Exact lane: INFINITE, non-alertable, no threads/procs.
             * Block on the private word; any state change wakes us. */
            uint32_t w0;
            thr_spin_lock();
            w0 = wr.word;
            thr_spin_unlock();
            (void)thr_futex_wait(&wr.word, w0);
            thr_spin_lock();
            thr_wait_dequeue(n, hs, &wr);
            if (self) {
                self->wait_wr = 0;
                self->wait_hs = 0;
                self->wait_n = 0;
            }
            thr_spin_unlock();
        }
    }
}

W32ABI W32_DWORD WaitForSingleObject(W32_HANDLE h, W32_DWORD ms) {
    return thr_wait_n(1, &h, 0, ms, 0);
}

W32ABI W32_DWORD WaitForSingleObjectEx(W32_HANDLE h, W32_DWORD ms,
                                       W32_BOOL alertable) {
    return thr_wait_n(1, &h, 0, ms, alertable ? 1 : 0);
}

W32ABI W32_DWORD WaitForMultipleObjects(W32_DWORD n, W32_HANDLE *hs,
                                        W32_BOOL wait_all, W32_DWORD ms) {
    if (n > (W32_DWORD)THR_MAXIMUM_WAIT_OBJECTS) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_WAIT_FAILED;
    }
    return thr_wait_n((int)n, hs, wait_all ? 1 : 0, ms, 0);
}

/* ---- minimal threadpool -------------------------------------------------------------
 *
 * N workers (N = online CPUs, 1..64, fixed at first use), one FIFO, one
 * event.  Callbacks run as void cb(instance, ctx, work) with instance ==
 * NULL (no instance APIs in the ledger).  CloseThreadpoolWork detaches;
 * queued callbacks still run.  Workers are ordinary w32 threads: they die
 * with ExitProcess's kill-all and never need joining. */

typedef void (W32ABI *thr_pool_cb)(void *instance, void *ctx, void *work);

struct w32_thr_pool_work {
    thr_pool_cb cb;
    void *ctx;
    int closed;
    int queued;
    struct w32_thr_pool_work *next;
};

static struct {
    int started;
    int nworkers;
    struct w32_thr_pool_work *head;
    struct w32_thr_pool_work *tail;
    struct w32_thr_event *ev;   /* manual-reset: set while work pends */
} thr_pool;

static W32_DWORD W32ABI thr_pool_worker(void *param) {
    (void)param;
    for (;;) {
        struct w32_thr_pool_work *w = 0;
        thr_checkpoint();
        thr_spin_lock();
        w = thr_pool.head;
        if (w) {
            thr_pool.head = w->next;
            if (!thr_pool.head)
                thr_pool.tail = 0;
            w->queued = 0;
            if (!thr_pool.head && thr_pool.ev)
                thr_pool.ev->signaled = 0;
        }
        thr_spin_unlock();
        if (!w) {
            /* Idle: block on the pool event directly (exact sleep). */
            struct w32_thr_event *ev;
            uint32_t seq;
            thr_spin_lock();
            ev = thr_pool.ev;
            /* Re-check under the spin (submit may have landed). */
            if (thr_pool.head) {
                thr_spin_unlock();
                continue;
            }
            seq = ev ? ev->w.seq : 0;
            thr_spin_unlock();
            if (!ev)
                return 0;
            (void)thr_futex_wait(&ev->w.seq, seq);
            continue;
        }
        w->cb(0, w->ctx, w);
    }
}

static void thr_pool_ensure(void) {
    long n;
    thr_spin_lock();
    if (thr_pool.started) {
        thr_spin_unlock();
        return;
    }
    thr_pool.started = 1;
    thr_spin_unlock();
    /* Build outside the spin (CreateThread + event take it themselves). */
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0)
        n = 1;
    if (n > 64)
        n = 64;
    thr_pool.ev = (struct w32_thr_event *)calloc(1, sizeof(*thr_pool.ev));
    if (thr_pool.ev) {
        thr_pool.ev->manual = 1;
        thr_pool.ev->refs = 1;
    }
    thr_pool.nworkers = 0;
    for (long i = 0; i < n; i++) {
        W32_HANDLE h = CreateThread(0, 0, thr_pool_worker, 0, 0, 0);
        if (!h)
            break;
        /* The handle is closed immediately: workers are reaped by
         * ExitProcess, never waited on.  (CloseHandle on a live thread
         * only drops the object ref — see w32_thread_close.) */
        CloseHandle(h);
        thr_pool.nworkers++;
    }
    if (thr_pool.nworkers == 0) {
        /* No worker could start (catastrophic): submissions queue and
         * never run.  Documented, untestable without fault injection. */
    }
}

W32ABI void *CreateThreadpoolWork(void *callback, void *ctx, void *cbd) {
    struct w32_thr_pool_work *w;
    (void)cbd;   /* no callback-env APIs in the ledger: must be NULL */
    if (!callback || cbd) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    thr_pool_ensure();
    w = (struct w32_thr_pool_work *)calloc(1, sizeof(*w));
    if (!w) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    w->cb = (thr_pool_cb)callback;
    w->ctx = ctx;
    w32_set_last_error(W32_ERROR_SUCCESS);
    return w;
}

W32ABI void SubmitThreadpoolWork(void *work) {
    struct w32_thr_pool_work *w = (struct w32_thr_pool_work *)work;
    if (!w)
        return;
    thr_pool_ensure();
    thr_spin_lock();
    if (w->closed || w->queued) {
        /* Closed work ignores submits; an already-queued item is not
         * queued twice (Windows coalesces the same way). */
        thr_spin_unlock();
        return;
    }
    w->queued = 1;
    w->next = 0;
    if (thr_pool.tail)
        thr_pool.tail->next = w;
    else
        thr_pool.head = w;
    thr_pool.tail = w;
    if (thr_pool.ev) {
        thr_pool.ev->signaled = 1;
        thr_waitable_wake(&thr_pool.ev->w);
    }
    thr_spin_unlock();
}

W32ABI void CloseThreadpoolWork(void *work) {
    struct w32_thr_pool_work *w = (struct w32_thr_pool_work *)work;
    if (!w)
        return;
    /* Detach: queued callbacks still run (documented Windows-compat:
     * Close does not wait and does not cancel). */
    thr_spin_lock();
    w->closed = 1;
    thr_spin_unlock();
}
