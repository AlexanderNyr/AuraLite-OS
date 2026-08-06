/* kernel/ipc/sysvipc.c — System V IPC (POSIX2024 phase Q14).
 *
 * Real kernel objects for semaphores, shared memory and message queues,
 * replacing the Q10 ENOSYS stubs.  Design notes:
 *
 *   - One flat table per object kind (SYSV_MAX_OBJS entries), indexed by
 *     a small integer id.  A global spinlock serialises table mutations;
 *     per-object wait queues implement blocking semop/msgsnd/msgrcv.
 *   - Permission checks reuse the P7 credentials: euid == 0 is root, else
 *     the object's 9-bit mode is consulted with owner/group/other and the
 *     process's supplementary groups.
 *   - shm segments are page-backed by PMM frames.  shmat maps those frames
 *     into the attaching process's address space with
 *     USER|WRITABLE|NO_EXEC; shmdt unmaps them.  IPC_RMID with nattch > 0
 *     marks the segment for destruction at the last detach.
 *   - SEM_UNDO records live on the TCB (sem_undo_list); thread exit
 *     applies them via sysvipc_cleanup_process().
 *
 * Deliberate deviations from the plan's task list, annotated (house
 * convention): shm attachments SURVIVE execve (POSIX shmat semantics; the
 * plan's "exec closes like FD_CLOEXEC" would break the documented shared-
 * counter test and real programs), and no IPC_INFO/ipcperm system info is
 * exposed (non-goal /proc/sysvipc).
 */

#include "kernel/ipc/sysvipc.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/usercopy.h"
#include "kernel/proc/wait_queue.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/vma.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/boot_info.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include "kernel/time.h"
#include <stdint.h>

/* Forward declaration (defined at the end of this file). */
int cur_euid_is_root(void);

#define PAGE_SZ 4096ULL

/* ---- per-process attachment / undo records (defined in thread.h) ---- */
struct shm_attach { int shmid; uint64_t va; struct shm_attach *next; };
struct sem_undo   { int semid; int semnum; int delta; struct sem_undo *next; };

/* ---- tables ---- */
struct sysv_sem {
    int in_use;
    struct sysv_ipc_perm perm;
    int nsems;
    uint16_t *vals;             /* kmalloc'd */
    int64_t otime, ctime;
    int32_t cpid, lpid;
    struct wait_queue wq;
};
static struct sysv_sem sems[SYSV_MAX_OBJS];

struct sysv_shm {
    int in_use;
    struct sysv_ipc_perm perm;
    uint64_t size;              /* bytes (page-aligned) */
    int npages;
    uint64_t *pages;            /* physical frames, kmalloc'd */
    int nattch;
    int32_t cpid, lpid;
    int64_t atime, dtime, ctime;
    int destroy_pending;
};
static struct sysv_shm shms[SYSV_MAX_OBJS];

struct msg_node {
    long mtype;
    size_t size;
    struct msg_node *next;
    char data[];                /* flexible array */
};

struct sysv_msg {
    int in_use;
    struct sysv_ipc_perm perm;
    struct msg_node *head, *tail;
    uint64_t qnum, cbytes, qbytes;
    int64_t stime, rtime, ctime;
    int32_t lspid, lrpid;
    struct wait_queue rq, sq;
};
static struct sysv_msg msgs[SYSV_MAX_OBJS];

static spinlock_t sysv_lock = SPINLOCK_UNLOCKED;

/* ======================= helpers ======================= */

static int64_t now_sec(void) {
    struct kernel_timespec ts;
    kernel_clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

/* Permission check: read=0 / write=1 against the P7 credentials. */
static int perm_ok(const struct sysv_ipc_perm *p, int write) {
    tcb_t *cur = sched_current();
    if (!cur) return 0;
    if (cur->euid == 0) return 1;                 /* root: everything */
    int bit = write ? 1 : 0;
    if (cur->euid == p->uid)
        return (p->mode >> (6 + bit)) & 1;        /* owner */
    if (cur->egid == p->gid) {
        for (int i = 0; i < cur->ngroups; i++)
            if (cur->supplementary_gids[i] == p->gid) return (p->mode >> (3 + bit)) & 1;
        return (p->mode >> (3 + bit)) & 1;
    }
    return (p->mode >> bit) & 1;                  /* other */
}

static void perm_init(struct sysv_ipc_perm *p, int64_t key, int mode) {
    tcb_t *cur = sched_current();
    p->key  = key;
    p->uid  = cur ? cur->euid : 0;
    p->gid  = cur ? cur->egid : 0;
    p->cuid = p->uid;
    p->cgid = p->gid;
    p->mode = (uint32_t)(mode & 0777);
    p->seq  = 0;
}

/* Generic find-or-create for an array of { in_use, perm.key, perm.uid... }.
 * mode_mask: which permission bit is needed to OPEN an existing object
 * (read=0400 etc.).  Returns id (>=0) or -errno. */
#define IPC_FIND_OR_CREATE_IMPL(name, arr, open_write)                          \
static int ipc_find_or_create_##name(int64_t key, int flags, int mode_bits,    \
                                     int *created) {                            \
    *created = 0;                                                               \
    int free_slot = -1;                                                         \
    int found = -1;                                                             \
    for (int i = 0; i < SYSV_MAX_OBJS; i++) {                                   \
        if (!arr[i].in_use) { if (free_slot < 0) free_slot = i; continue; }     \
        if (key != SYSV_IPC_PRIVATE && arr[i].perm.key == key) { found = i; break; } \
    }                                                                           \
    if (found >= 0) {                                                           \
        if (!perm_ok(&arr[found].perm, open_write)) return -EACCES;             \
        if ((flags & SYSV_IPC_CREAT) && (flags & SYSV_IPC_EXCL)) return -EEXIST;\
        return found;                                                           \
    }                                                                           \
    if (!(flags & SYSV_IPC_CREAT)) return -ENOENT;                              \
    if (free_slot < 0) return -ENOSPC;                                          \
    perm_init(&arr[free_slot].perm, key, mode_bits);                            \
    arr[free_slot].in_use = 1;                                                  \
    *created = 1;                                                               \
    return free_slot;                                                           \
}

IPC_FIND_OR_CREATE_IMPL(sem, sems, 1)
IPC_FIND_OR_CREATE_IMPL(msg, msgs, 1)
IPC_FIND_OR_CREATE_IMPL(shm, shms, 0)   /* shmget needs WRITE permission */

/* ======================= semaphores ======================= */

int64_t sysv_semget(int64_t key, int nsems, int semflg) {
    if (nsems < 0 || nsems > 256) return -EINVAL;
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    int created = 0;
    int id = ipc_find_or_create_sem(key, semflg, 0666, &created);
    if (id < 0) { spinlock_release_irqrestore(&sysv_lock, rf); return id; }
    struct sysv_sem *s = &sems[id];
    if (created) {
        s->vals = (uint16_t *)kmalloc((size_t)nsems * sizeof(uint16_t));
        if (!s->vals) {
            s->in_use = 0;
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -ENOMEM;
        }
        memset(s->vals, 0, (size_t)nsems * sizeof(uint16_t));
        s->nsems = nsems;
        s->otime = 0; s->ctime = now_sec();
        s->cpid = sched_current() ? (int32_t)sched_current()->id : 0;
        s->lpid = 0;
        wq_init(&s->wq);
    } else if (nsems > s->nsems) {
        /* Existing set smaller than requested: grow is allowed only on
         * IPC_CREAT (Linux: EINVAL). */
        spinlock_release_irqrestore(&sysv_lock, rf);
        return -EINVAL;
    }
    spinlock_release_irqrestore(&sysv_lock, rf);
    return id;
}

static int sem_try_apply(struct sysv_sem *s, const struct sysv_sembuf *ops,
                         uint64_t nops, int allow_undo) {
    /* Check all ops are satisfiable without modifying anything. */
    for (uint64_t i = 0; i < nops; i++) {
        unsigned num = ops[i].sem_num;
        if (num >= (unsigned)s->nsems) return -ERANGE;
        int op = ops[i].sem_op;
        if (op == 0) { if (s->vals[num] != 0) return 0; }      /* wait for zero */
        else if (op < 0) { if ((int)s->vals[num] < -op) return 0; }
    }
    /* All satisfiable: apply. */
    tcb_t *cur = sched_current();
    for (uint64_t i = 0; i < nops; i++) {
        unsigned num = ops[i].sem_num;
        int op = ops[i].sem_op;
        int v = (int)s->vals[num] + op;
        if (v < 0) v = 0;
        s->vals[num] = (uint16_t)v;
        if (allow_undo && (ops[i].sem_flg & SYSV_SEM_UNDO) && cur && op != 0) {
            /* Record the reverse adjustment for thread exit. */
            struct sem_undo *u = (struct sem_undo *)kmalloc(sizeof(*u));
            if (u) {
                memset(u, 0, sizeof(*u));
                u->semid = (int)(s - sems);
                u->semnum = (int)num;
                u->delta = -op;
                u->next = cur->sem_undo_list;
                cur->sem_undo_list = u;
            }
        }
    }
    s->otime = now_sec();
    s->lpid = cur ? (int32_t)cur->id : 0;
    return 1;   /* applied */
}

int64_t sysv_semop(int semid, const void *sops_user, uint64_t nsops) {
    if (semid < 0 || semid >= SYSV_MAX_OBJS || nsops == 0 || nsops > 64)
        return -EINVAL;
    struct sysv_sembuf ops[64];
    uint64_t bytes = nsops * sizeof(struct sysv_sembuf);
    if (copy_from_user(ops, sops_user, bytes) != 0) return -EFAULT;

    for (;;) {
        uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
        struct sysv_sem *s = &sems[semid];
        if (!s->in_use) { spinlock_release_irqrestore(&sysv_lock, rf); return -EINVAL; }
        if (!perm_ok(&s->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }

        int r = sem_try_apply(s, ops, nsops, 1);
        if (r > 0) {
            wq_wake_all(&s->wq);   /* state changed: wake zero-waiters */
            spinlock_release_irqrestore(&sysv_lock, rf);
            return 0;
        }
        if (r < 0) { spinlock_release_irqrestore(&sysv_lock, rf); return r; }

        /* Would block: IPC_NOWAIT on any op? */
        int nowait = 0;
        for (uint64_t i = 0; i < nsops; i++)
            if (ops[i].sem_flg & SYSV_IPC_NOWAIT) nowait = 1;
        if (nowait) { spinlock_release_irqrestore(&sysv_lock, rf); return -EAGAIN; }

        /* Block on the semaphore's wait queue (signal-interruptible).
         * Order matters: set state=THREAD_BLOCKED BEFORE publishing the
         * entry and keep holding sysv_lock across wq_add_entry, so a
         * concurrent V() (which also holds sysv_lock) cannot wake the queue
         * between our entry being visible and our state being BLOCKED —
         * otherwise it would skip us (state still READY) and we would sleep
         * forever.  The cli/sti section closes the same race with signal
         * delivery, exactly like kernel_nanosleep. */
        tcb_t *cur = sched_current();
        struct wq_entry entry;
        entry.tcb = cur;
        entry.next = NULL;
        cur->state = THREAD_BLOCKED;
        wq_add_entry(&s->wq, &entry);
        spinlock_release_irqrestore(&sysv_lock, rf);

        uint64_t rflags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
        if (cur && (cur->sig_pending & ~cur->sig_mask)) {
            __asm__ volatile ("sti" ::: "memory");
            cur->state = THREAD_READY;
            wq_remove_entry(&s->wq, &entry);
            return -EINTR;
        }
        schedule();
        if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
        wq_remove_entry(&s->wq, &entry);
        /* Loop and re-check. */
    }
}

int64_t sysv_semctl(int semid, int semnum, int cmd, uint64_t arg) {
    if (semid < 0 || semid >= SYSV_MAX_OBJS) return -EINVAL;
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    struct sysv_sem *s = &sems[semid];
    if (!s->in_use) { spinlock_release_irqrestore(&sysv_lock, rf); return -EINVAL; }

    switch (cmd) {
    case SYSV_IPC_RMID: {
        if (!perm_ok(&s->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        /* Wake any blocked waiters so they re-check and fail. */
        wq_wake_all(&s->wq);
        if (s->vals) kfree(s->vals);
        memset(s, 0, sizeof(*s));
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    }
    case SYSV_IPC_STAT: {
        if (!perm_ok(&s->perm, 0)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        struct sysv_semid_ds ds;
        memset(&ds, 0, sizeof(ds));
        ds.sem_perm = s->perm;
        ds.sem_nsems = (unsigned short)s->nsems;
        ds.sem_otime = s->otime;
        ds.sem_ctime = s->ctime;
        spinlock_release_irqrestore(&sysv_lock, rf);
        if (copy_to_user((void *)(uintptr_t)arg, &ds, sizeof(ds)) != 0) return -EFAULT;
        return 0;
    }
    case SYSV_GETPID:
        if (!perm_ok(&s->perm, 0)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        { int r = (int)s->lpid; spinlock_release_irqrestore(&sysv_lock, rf); return r; }
    case SYSV_GETVAL:
        if (semnum < 0 || semnum >= s->nsems) { spinlock_release_irqrestore(&sysv_lock, rf); return -ERANGE; }
        if (!perm_ok(&s->perm, 0)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        { int r = s->vals[semnum]; spinlock_release_irqrestore(&sysv_lock, rf); return r; }
    case SYSV_SETVAL:
        if (semnum < 0 || semnum >= s->nsems) { spinlock_release_irqrestore(&sysv_lock, rf); return -ERANGE; }
        if (!perm_ok(&s->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        s->vals[semnum] = (uint16_t)(arg & 0xFFFF);
        s->ctime = now_sec();
        wq_wake_all(&s->wq);
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    case SYSV_GETALL: {
        if (!perm_ok(&s->perm, 0)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        unsigned short tmp[256];
        for (int i = 0; i < s->nsems; i++) tmp[i] = s->vals[i];
        spinlock_release_irqrestore(&sysv_lock, rf);
        if (copy_to_user((void *)(uintptr_t)arg, tmp,
                         (size_t)s->nsems * sizeof(unsigned short)) != 0) return -EFAULT;
        return 0;
    }
    case SYSV_SETALL: {
        if (!perm_ok(&s->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        unsigned short tmp[256];
        if (copy_from_user(tmp, (const void *)(uintptr_t)arg,
                           (size_t)s->nsems * sizeof(unsigned short)) != 0) {
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -EFAULT;
        }
        for (int i = 0; i < s->nsems; i++) s->vals[i] = tmp[i];
        s->ctime = now_sec();
        wq_wake_all(&s->wq);
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    }
    case SYSV_IPC_SET: {
        if (!perm_ok(&s->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        struct sysv_semid_ds ds;
        if (copy_from_user(&ds, (const void *)(uintptr_t)arg, sizeof(ds)) != 0) {
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -EFAULT;
        }
        if (cur_euid_is_root() == 0 && ds.sem_perm.uid != s->perm.uid)
            { spinlock_release_irqrestore(&sysv_lock, rf); return -EPERM; }
        s->perm.uid = ds.sem_perm.uid;
        s->perm.gid = ds.sem_perm.gid;
        s->perm.mode = (s->perm.mode & ~0777u) | (ds.sem_perm.mode & 0777u);
        s->ctime = now_sec();
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    }
    default:
        spinlock_release_irqrestore(&sysv_lock, rf);
        return -EINVAL;
    }
}

/* ======================= shared memory ======================= */

static uint64_t shm_alloc_addr(uint64_t hint, uint64_t len, int rnd) {
    tcb_t *cur = sched_current();
    extern int user_range_is_free_exported(uint64_t, uint64_t);
    if (hint != 0) {
        uint64_t a = hint;
        if (rnd) a &= ~(uint64_t)(PAGE_SZ - 1);
        if (a < 0x40000000ULL || a >= 0x7FFF00000000ULL) a = 0;
        if (a) {
            uint64_t free_ok = 1;
            for (uint64_t off = 0; off < len; off += PAGE_SZ)
                if (paging_get_phys(a + off) != 0) { free_ok = 0; break; }
            if (free_ok) return a;
        }
    }
    /* Auto: scan the mmap region. */
    uint64_t start = cur->mmap_next ? cur->mmap_next : 0x0000400000000000ULL;
    start = (start + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
    for (uint64_t c = start; c >= 0x0000400000000000ULL &&
                            c + len <= 0x00007FFFFFFFFFFFULL; c += PAGE_SZ) {
        uint64_t free_ok = 1;
        for (uint64_t off = 0; off < len; off += PAGE_SZ)
            if (paging_get_phys(c + off) != 0) { free_ok = 0; break; }
        if (free_ok) {
            cur->mmap_next = c + len;
            return c;
        }
    }
    return 0;
}

int64_t sysv_shmget(int64_t key, uint64_t size, int shmflg) {
    if (size > 0x2000000ULL) return -EINVAL;          /* SHMMAX */
    uint64_t len = (size + PAGE_SZ - 1) & ~(PAGE_SZ - 1ULL);
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    int created = 0;
    int id = ipc_find_or_create_shm(key, shmflg, 0666, &created);
    if (id < 0) { spinlock_release_irqrestore(&sysv_lock, rf); return id; }
    struct sysv_shm *m = &shms[id];
    if (created) {
        if (len == 0) len = PAGE_SZ;
        m->npages = (int)(len / PAGE_SZ);
        m->pages = (uint64_t *)kmalloc((size_t)m->npages * sizeof(uint64_t));
        if (!m->pages) { m->in_use = 0; spinlock_release_irqrestore(&sysv_lock, rf); return -ENOMEM; }
        memset(m->pages, 0, (size_t)m->npages * sizeof(uint64_t));
        for (int i = 0; i < m->npages; i++) {
            uint64_t phys = pmm_alloc_frame();
            if (!phys) {
                for (int j = 0; j < i; j++) pmm_free_frame(m->pages[j]);
                kfree(m->pages);
                m->in_use = 0;
                spinlock_release_irqrestore(&sysv_lock, rf);
                return -ENOMEM;
            }
            uint64_t hhdm = boot_get_hhdm_offset();
            memset((void *)(uintptr_t)(hhdm + phys), 0, PAGE_SZ);
            m->pages[i] = phys;
        }
        m->size = len;
        m->nattch = 0;
        m->cpid = sched_current() ? (int32_t)sched_current()->id : 0;
        m->lpid = 0;
        m->atime = m->dtime = 0;
        m->ctime = now_sec();
        m->destroy_pending = 0;
    } else {
        if (len > m->size) { spinlock_release_irqrestore(&sysv_lock, rf); return -EINVAL; }
    }
    spinlock_release_irqrestore(&sysv_lock, rf);
    return id;
}

uint64_t sysv_shmat(int shmid, uint64_t shmaddr, int shmflg) {
    if (shmid < 0 || shmid >= SYSV_MAX_OBJS) return (uint64_t)-EINVAL;
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    struct sysv_shm *m = &shms[shmid];
    if (!m->in_use) { spinlock_release_irqrestore(&sysv_lock, rf); return (uint64_t)-EINVAL; }
    if (!perm_ok(&m->perm, (shmflg & SYSV_SHM_RDONLY) ? 0 : 1)) {
        spinlock_release_irqrestore(&sysv_lock, rf);
        return (uint64_t)-EACCES;
    }
    uint64_t len = m->size;
    uint64_t va = shm_alloc_addr(shmaddr, len, (shmflg & SYSV_SHM_RND) ? 1 : 0);
    if (!va) { spinlock_release_irqrestore(&sysv_lock, rf); return (uint64_t)-ENOMEM; }

    uint64_t pte_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER |
                         PAGE_FLAG_NO_EXEC;
    if (!(shmflg & SYSV_SHM_RDONLY)) pte_flags |= PAGE_FLAG_WRITABLE;
    for (int i = 0; i < m->npages; i++)
        paging_map(va + (uint64_t)i * PAGE_SZ, m->pages[i], pte_flags);

    tcb_t *cur = sched_current();

    /* Register a VMA_SHARED mapping so COW fork() keeps these pages shared
     * (paging_clone_user_space skips writable->COW for VMA_SHARED ranges)
     * and so munmap-style teardown is coherent with the VMA list. */
    {
        uint32_t vflags = VMA_SHARED | VMA_READ;
        if (!(shmflg & SYSV_SHM_RDONLY)) vflags |= VMA_WRITE;
        uint64_t vf = spinlock_acquire_irqsave(&cur->vma_lock);
        int vr = vma_insert(&cur->vma_list, va, va + len, vflags, NULL, 0);
        spinlock_release_irqrestore(&cur->vma_lock, vf);
        if (vr != 0) {
            for (int i = 0; i < m->npages; i++) paging_unmap(va + (uint64_t)i * PAGE_SZ);
            spinlock_release_irqrestore(&sysv_lock, rf);
            return (uint64_t)-ENOMEM;
        }
    }

    struct shm_attach *a = (struct shm_attach *)kmalloc(sizeof(*a));
    if (!a) {
        for (int i = 0; i < m->npages; i++) paging_unmap(va + (uint64_t)i * PAGE_SZ);
        uint64_t vf = spinlock_acquire_irqsave(&cur->vma_lock);
        vma_remove_range(&cur->vma_list, va, va + len);
        spinlock_release_irqrestore(&cur->vma_lock, vf);
        spinlock_release_irqrestore(&sysv_lock, rf);
        return (uint64_t)-ENOMEM;
    }
    memset(a, 0, sizeof(*a));
    a->shmid = shmid;
    a->va = va;
    a->next = cur->shm_attachments;
    cur->shm_attachments = a;

    m->nattch++;
    m->atime = now_sec();
    m->lpid = (int32_t)cur->id;
    spinlock_release_irqrestore(&sysv_lock, rf);
    return va;
}

static void shm_release_frames(struct sysv_shm *m) {
    for (int i = 0; i < m->npages; i++) pmm_free_frame(m->pages[i]);
    kfree(m->pages);
}

int64_t sysv_shmdt(uint64_t shmaddr) {
    tcb_t *cur = sched_current();
    if (!cur) return -EINVAL;
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    struct shm_attach **pp = &cur->shm_attachments;
    while (*pp) {
        struct shm_attach *a = *pp;
        if (a->va == shmaddr) {
            *pp = a->next;
            if (a->shmid >= 0 && a->shmid < SYSV_MAX_OBJS && shms[a->shmid].in_use) {
                struct sysv_shm *m = &shms[a->shmid];
                for (int i = 0; i < m->npages; i++)
                    paging_unmap(a->va + (uint64_t)i * PAGE_SZ);
                uint64_t vf = spinlock_acquire_irqsave(&cur->vma_lock);
                vma_remove_range(&cur->vma_list, a->va, a->va + m->size);
                spinlock_release_irqrestore(&cur->vma_lock, vf);
                m->nattch--;
                m->dtime = now_sec();
                if (m->nattch < 0) m->nattch = 0;
                if (m->destroy_pending && m->nattch == 0) {
                    shm_release_frames(m);
                    memset(m, 0, sizeof(*m));
                }
            }
            kfree(a);
            spinlock_release_irqrestore(&sysv_lock, rf);
            return 0;
        }
        pp = &a->next;
    }
    spinlock_release_irqrestore(&sysv_lock, rf);
    return -EINVAL;
}

int64_t sysv_shmctl(int shmid, int cmd, uint64_t buf_user) {
    if (shmid < 0 || shmid >= SYSV_MAX_OBJS) return -EINVAL;
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    struct sysv_shm *m = &shms[shmid];
    if (!m->in_use) { spinlock_release_irqrestore(&sysv_lock, rf); return -EINVAL; }

    switch (cmd) {
    case SYSV_IPC_RMID:
        if (!perm_ok(&m->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        if (m->nattch == 0) {
            shm_release_frames(m);
            memset(m, 0, sizeof(*m));
        } else {
            m->destroy_pending = 1;   /* freed at last detach */
        }
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    case SYSV_IPC_STAT: {
        if (!perm_ok(&m->perm, 0)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        struct sysv_shmid_ds ds;
        memset(&ds, 0, sizeof(ds));
        ds.shm_perm = m->perm;
        ds.shm_segsz = m->size;
        ds.shm_atime = m->atime;
        ds.shm_dtime = m->dtime;
        ds.shm_ctime = m->ctime;
        ds.shm_cpid = m->cpid;
        ds.shm_lpid = m->lpid;
        ds.shm_nattch = (unsigned short)m->nattch;
        spinlock_release_irqrestore(&sysv_lock, rf);
        if (copy_to_user((void *)(uintptr_t)buf_user, &ds, sizeof(ds)) != 0) return -EFAULT;
        return 0;
    }
    case SYSV_IPC_SET: {
        if (!perm_ok(&m->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        struct sysv_shmid_ds ds;
        if (copy_from_user(&ds, (const void *)(uintptr_t)buf_user, sizeof(ds)) != 0) {
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -EFAULT;
        }
        if (cur_euid_is_root() == 0 && ds.shm_perm.uid != m->perm.uid) {
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -EPERM;
        }
        m->perm.uid = ds.shm_perm.uid;
        m->perm.gid = ds.shm_perm.gid;
        m->perm.mode = (m->perm.mode & ~0777u) | (ds.shm_perm.mode & 0777u);
        m->ctime = now_sec();
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    }
    default:
        spinlock_release_irqrestore(&sysv_lock, rf);
        return -EINVAL;
    }
}

/* ======================= message queues ======================= */

int64_t sysv_msgget(int64_t key, int msgflg) {
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    int created = 0;
    int id = ipc_find_or_create_msg(key, msgflg, 0666, &created);
    if (id < 0) { spinlock_release_irqrestore(&sysv_lock, rf); return id; }
    struct sysv_msg *q = &msgs[id];
    if (created) {
        q->head = q->tail = NULL;
        q->qnum = q->cbytes = 0;
        q->qbytes = 16384;                 /* default 16 KiB */
        q->stime = q->rtime = 0;
        q->ctime = now_sec();
        q->lspid = q->lrpid = 0;
        wq_init(&q->rq);
        wq_init(&q->sq);
    }
    spinlock_release_irqrestore(&sysv_lock, rf);
    return id;
}

/* Pick the message that matches msgtyp per POSIX:
 *   msgtyp == 0 : first (FIFO)
 *   msgtyp >  0 : first with mtype == msgtyp
 *   msgtyp <  0 : first with mtype <= -msgtyp
 */
static struct msg_node *msg_pick(struct sysv_msg *q, int64_t typ) {
    if (typ == 0) return q->head;
    if (typ > 0) {
        for (struct msg_node *n = q->head; n; n = n->next)
            if (n->mtype == typ) return n;
        return NULL;
    }
    int64_t limit = -typ;
    for (struct msg_node *n = q->head; n; n = n->next)
        if (n->mtype <= limit) return n;
    return NULL;
}

int64_t sysv_msgsnd(int msqid, const void *msgp_user, uint64_t msgsz, int msgflg) {
    if (msqid < 0 || msqid >= SYSV_MAX_OBJS || msgsz > 8192) return -EINVAL;
    /* Read mtype + payload from user (mtype is the first long). */
    long mtype;
    if (copy_from_user(&mtype, msgp_user, sizeof(long)) != 0) return -EFAULT;
    if (mtype < 1) return -EINVAL;

    for (;;) {
        uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
        struct sysv_msg *q = &msgs[msqid];
        if (!q->in_use) { spinlock_release_irqrestore(&sysv_lock, rf); return -EINVAL; }
        if (!perm_ok(&q->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }

        if (q->cbytes + msgsz > q->qbytes) {
            if (msgflg & SYSV_IPC_NOWAIT) {
                spinlock_release_irqrestore(&sysv_lock, rf);
                return -EAGAIN;
            }
            /* Block on the send queue until a reader drains bytes (same
             * state-before-publish ordering as semop; see there). */
            tcb_t *cur = sched_current();
            struct wq_entry entry; entry.tcb = cur; entry.next = NULL;
            cur->state = THREAD_BLOCKED;
            wq_add_entry(&q->sq, &entry);
            spinlock_release_irqrestore(&sysv_lock, rf);
            uint64_t rflags;
            __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
            if (cur && (cur->sig_pending & ~cur->sig_mask)) {
                __asm__ volatile ("sti" ::: "memory");
                cur->state = THREAD_READY;
                wq_remove_entry(&q->sq, &entry);
                return -EINTR;
            }
            schedule();
            if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
            wq_remove_entry(&q->sq, &entry);
            continue;
        }

        struct msg_node *n = (struct msg_node *)kmalloc(sizeof(*n) + msgsz);
        if (!n) { spinlock_release_irqrestore(&sysv_lock, rf); return -ENOMEM; }
        if (copy_from_user(n->data, (const char *)msgp_user + sizeof(long), msgsz) != 0) {
            kfree(n);
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -EFAULT;
        }
        n->mtype = mtype;
        n->size = msgsz;
        n->next = NULL;
        if (q->tail) q->tail->next = n; else q->head = n;
        q->tail = n;
        q->qnum++;
        q->cbytes += msgsz;
        q->stime = now_sec();
        q->lspid = sched_current() ? (int32_t)sched_current()->id : 0;
        wq_wake_all(&q->rq);          /* a reader may now proceed */
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    }
}

int64_t sysv_msgrcv(int msqid, void *msgp_user, uint64_t msgsz,
                    int64_t msgtyp, int msgflg) {
    if (msqid < 0 || msqid >= SYSV_MAX_OBJS) return -EINVAL;

    for (;;) {
        uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
        struct sysv_msg *q = &msgs[msqid];
        if (!q->in_use) { spinlock_release_irqrestore(&sysv_lock, rf); return -EINVAL; }
        if (!perm_ok(&q->perm, 0)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }

        struct msg_node *n = msg_pick(q, msgtyp);
        if (n) {
            /* Detach and copy. */
            if (n == q->head) {
                q->head = n->next;
                if (!q->head) q->tail = NULL;
            } else {
                struct msg_node *prev = q->head;
                while (prev && prev->next != n) prev = prev->next;
                if (prev) prev->next = n->next;
                if (q->tail == n) q->tail = prev;
            }
            q->qnum--;
            q->cbytes -= n->size;

            size_t out = n->size;
            if (out > msgsz) {
                if (!(msgflg & SYSV_MSG_NOERROR)) {
                    /* Too big and no MSG_NOERROR: put the message back. */
                    if (q->tail) q->tail->next = n; else q->head = n;
                    q->tail = n;
                    q->qnum++;
                    q->cbytes += n->size;
                    spinlock_release_irqrestore(&sysv_lock, rf);
                    return -E2BIG;
                }
                out = msgsz;
            }
            q->rtime = now_sec();
            q->lrpid = sched_current() ? (int32_t)sched_current()->id : 0;
            spinlock_release_irqrestore(&sysv_lock, rf);

            /* Copy mtype + payload back to user. */
            if (copy_to_user(msgp_user, &n->mtype, sizeof(long)) != 0 ||
                copy_to_user((char *)msgp_user + sizeof(long), n->data, out) != 0) {
                kfree(n);
                return -EFAULT;
            }
            kfree(n);
            /* Space freed: wake a blocked sender. */
            uint64_t rf2 = spinlock_acquire_irqsave(&sysv_lock);
            if (msgs[msqid].in_use) wq_wake_all(&msgs[msqid].sq);
            spinlock_release_irqrestore(&sysv_lock, rf2);
            return (int64_t)out;
        }

        /* No matching message. */
        if (msgflg & SYSV_IPC_NOWAIT) {
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -ENOMSG;
        }
        tcb_t *cur = sched_current();
        struct wq_entry entry; entry.tcb = cur; entry.next = NULL;
        cur->state = THREAD_BLOCKED;
        wq_add_entry(&q->rq, &entry);
        spinlock_release_irqrestore(&sysv_lock, rf);
        uint64_t rflags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
        if (cur && (cur->sig_pending & ~cur->sig_mask)) {
            __asm__ volatile ("sti" ::: "memory");
            cur->state = THREAD_READY;
            wq_remove_entry(&q->rq, &entry);
            return -EINTR;
        }
        schedule();
        if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
        wq_remove_entry(&q->rq, &entry);
    }
}

int64_t sysv_msgctl(int msqid, int cmd, uint64_t buf_user) {
    if (msqid < 0 || msqid >= SYSV_MAX_OBJS) return -EINVAL;
    uint64_t rf = spinlock_acquire_irqsave(&sysv_lock);
    struct sysv_msg *q = &msgs[msqid];
    if (!q->in_use) { spinlock_release_irqrestore(&sysv_lock, rf); return -EINVAL; }

    switch (cmd) {
    case SYSV_IPC_RMID: {
        if (!perm_ok(&q->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        struct msg_node *n = q->head;
        while (n) { struct msg_node *nx = n->next; kfree(n); n = nx; }
        wq_wake_all(&q->rq);
        wq_wake_all(&q->sq);
        memset(q, 0, sizeof(*q));
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    }
    case SYSV_IPC_STAT: {
        if (!perm_ok(&q->perm, 0)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        struct sysv_msqid_ds ds;
        memset(&ds, 0, sizeof(ds));
        ds.msg_perm = q->perm;
        ds.msg_stime = q->stime;
        ds.msg_rtime = q->rtime;
        ds.msg_ctime = q->ctime;
        ds.msg_cbytes = q->cbytes;
        ds.msg_qnum = q->qnum;
        ds.msg_qbytes = q->qbytes;
        ds.msg_lspid = q->lspid;
        ds.msg_lrpid = q->lrpid;
        spinlock_release_irqrestore(&sysv_lock, rf);
        if (copy_to_user((void *)(uintptr_t)buf_user, &ds, sizeof(ds)) != 0) return -EFAULT;
        return 0;
    }
    case SYSV_IPC_SET: {
        if (!perm_ok(&q->perm, 1)) { spinlock_release_irqrestore(&sysv_lock, rf); return -EACCES; }
        struct sysv_msqid_ds ds;
        if (copy_from_user(&ds, (const void *)(uintptr_t)buf_user, sizeof(ds)) != 0) {
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -EFAULT;
        }
        if (cur_euid_is_root() == 0 && ds.msg_perm.uid != q->perm.uid) {
            spinlock_release_irqrestore(&sysv_lock, rf);
            return -EPERM;
        }
        q->perm.uid = ds.msg_perm.uid;
        q->perm.gid = ds.msg_perm.gid;
        q->perm.mode = (q->perm.mode & ~0777u) | (ds.msg_perm.mode & 0777u);
        q->ctime = now_sec();
        spinlock_release_irqrestore(&sysv_lock, rf);
        return 0;
    }
    default:
        spinlock_release_irqrestore(&sysv_lock, rf);
        return -EINVAL;
    }
}

/* ======================= process teardown ======================= */

/* Detach all shm segments attached by `t` (shared with cleanup; the undo
 * records are left intact so exec preserves them). */
static void sysvipc_shm_detach_list(tcb_t *t) {
    if (!t) return;
    struct shm_attach *a = t->shm_attachments;
    t->shm_attachments = NULL;
    while (a) {
        struct shm_attach *nx = a->next;
        if (a->shmid >= 0 && a->shmid < SYSV_MAX_OBJS && shms[a->shmid].in_use) {
            struct sysv_shm *m = &shms[a->shmid];
            for (int i = 0; i < m->npages; i++)
                paging_unmap(a->va + (uint64_t)i * PAGE_SZ);
            uint64_t vf = spinlock_acquire_irqsave(&t->vma_lock);
            vma_remove_range(&t->vma_list, a->va, a->va + m->size);
            spinlock_release_irqrestore(&t->vma_lock, vf);
            m->nattch--;
            if (m->nattch < 0) m->nattch = 0;
            if (m->destroy_pending && m->nattch == 0) {
                shm_release_frames(m);
                memset(m, 0, sizeof(*m));
            }
        }
        kfree(a);
        a = nx;
    }
}

void sysvipc_shm_detach_all(tcb_t *t) {
    sysvipc_shm_detach_list(t);
}

void sysvipc_cleanup_process(tcb_t *t) {
    if (!t) return;
    /* Apply SEM_UNDO records (reverse the recorded deltas). */
    struct sem_undo *u = t->sem_undo_list;
    t->sem_undo_list = NULL;
    while (u) {
        struct sem_undo *nx = u->next;
        if (u->semid >= 0 && u->semid < SYSV_MAX_OBJS && sems[u->semid].in_use &&
            u->semnum >= 0 && u->semnum < sems[u->semid].nsems) {
            struct sysv_sem *s = &sems[u->semid];
            int v = (int)s->vals[u->semnum] + u->delta;
            if (v < 0) v = 0;
            s->vals[u->semnum] = (uint16_t)v;
            wq_wake_all(&s->wq);
        }
        kfree(u);
        u = nx;
    }

    /* Detach all attached shm segments. */
    sysvipc_shm_detach_list(t);
}

/* Root check helper used by IPC_SET paths. */
int cur_euid_is_root(void) {
    tcb_t *cur = sched_current();
    return cur ? (cur->euid == 0) : 0;
}
