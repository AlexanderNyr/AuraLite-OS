/* kernel/arch/aarch64/user_a64.c -- EL0 + svc (ARM64_PLAN A4).
 *
 * user_rv.c's privilege round trip, the fourth ISA's spelling:
 *
 *   kernel thread -> user_a64_run_image
 *     copy the image to a fresh frame, map it at 0x40000000 with
 *     EL0 R+X -- REAL permissions: W^X holds at EL0 too, and TWICE
 *     (kernel pages are UXN from A3; user text is PXN from here --
 *     each world is non-executable from the other)
 *     user stack below it (EL0 R+W, and pointedly NOT X)
 *     user_enter_a64: trap stack in sp, SP_EL0 := user sp, eret
 *   EL0 -> svc #0 (x8 number / x0-x5 args / x0 return, D4)
 *     the hardware lands on SP_EL1 (no scratch-CSR dance -- the I7
 *     esp0 contract is an architecture feature here), vectors.S
 *     spills, a64_trap routes EC 0x15 here
 *   SYS_A64_EXIT longjmps the kernel thread out of the user context
 *     (a64_setjmp/a64_longjmp_entry -- A3's fault-probe pair IS the
 *     exit trampoline; one mechanism, two tenants, the V3/V4 rhyme)
 *
 * An EL0 fault (the lower-EL vector rows / lower-EL ECs) terminates
 * the image with 128+EC through the same longjmp -- the kernel
 * survives, the negative control depends on it.
 *
 * PAN note (SUM's sibling, opposite default): riscv64 measured that
 * kernel loads from user pages trap until sstatus.SUM is set.  On
 * ARMv8.0 there is NO PAN -- the kernel can touch EL0-accessible
 * pages freely, so this file needs no sum_on/sum_off pair.  PAN
 * arrives at v8.1 as an OPT-IN hardening (SCTLR_EL1.SPAN); the A-plan
 * runs v8.0 semantics and NAMES the residue here instead of silently
 * relying on it: when PAN hardening lands, every user access in this
 * file funnels through the two copy helpers below, which is where
 * the pan_off/pan_on pair will go.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/user_a64.h"
#include "kernel/arch/aarch64/kheap_a64.h"
#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/pmm_a64.h"
#include "kernel/arch/aarch64/thread_a64.h"
#include "kernel/arch/aarch64/initrd_a64.h"
#include "kernel/arch/aarch64/elfa64load.h"
#include "kernel/arch/aarch64/kheap_a64.h"
#include "kernel/arch/aarch64/irqflags.h"   /* A6: the DAIF backend */
#include "kernel/arch/aarch64/fsglue_a64.h"  /* P4: mounted ops */
#include "kernel/arch/aarch64/pmm_a64.h"     /* R6: the brk heap */
#include "kernel/fs/vfs.h"
#include "kernel/fs/vfsmount.h"
#include "lib/abi/fsabi.h"

/* context_a64.S */
extern void user_enter_a64(uint64_t entry, uint64_t user_sp,
                           uint64_t trap_stack_top)
    __attribute__((noreturn));
/* vectors.S */
extern int  a64_setjmp(uint64_t *buf);
extern void a64_longjmp_entry(void);

#define USER_STACK_TOP_A64   USER_TEXT_VADDR_A64
#define USER_STACK_VADDR_A64 (USER_STACK_TOP_A64 - PAGE_SIZE_A64)
#define TRAP_STACK_SIZE      8192

/* One image at a time (boot-CPU bring-up, D5). */
static uint64_t exit_jmpbuf[14];
static volatile int user_active;

static void put_udec_(uint64_t v) { pl011_putdec64(v); }
static void put_hex_(uint64_t v)  { pl011_puthex64(v); }

/* The exit unwind: rewrite the frame so eret lands in
 * a64_longjmp_entry AT EL1 with x0 = jmpbuf (setjmp returns 1; the
 * exit code travels in a static because unlike rv_longjmp_entry's
 * a1-carries-code contract, a64_setjmp's return value is fixed at 1
 * -- one less moving part in the assembly, one static here).
 *
 * SPSR: EL1h with IRQ MASKED -- the rv SPIE lesson's DAIF spelling:
 * the eret lands with sp still meaning "the dedicated trap stack",
 * and an IRQ taken before user_a64_run_image restores the kernel
 * stack notion would build a frame over live unwind state.  The
 * lesson transfers by lineage (bought with a -d int session on the
 * rv port); interrupts reopen in run_image once sp is sane. */
static volatile int exit_code_box;

static void user_a64_leave(a64_trap_frame_t *f, int code)
{
    user_active   = 0;
    exit_code_box = code;
    f->elr        = (uint64_t)a64_longjmp_entry;
    f->spsr       = 0x3C5;                 /* EL1h, DAIF all masked */
    f->regs[0]    = (uint64_t)exit_jmpbuf;
}

/* ---- trap_a64.c hooks ------------------------------------------------------ */

/* ---- A5c: the cooked console line (cons_rv_readline's shape) --------------
 *
 * Echo, backspace, CR->LF, newline stored.  Byte source: the polled
 * PL011 RX (IRQ-driven RX is A7).  The blocking wait is
 * `daifclr, #2; wfi` -- the svc trap path runs with IRQs masked, so a
 * bare wfi here would sleep with a prompt on screen forever: the
 * rv64 comment's sstatus.SIE twin, DAIF spelling (the I7 cleared-IF
 * deadlock, fourth edition).  The 100 Hz timer bounds every wait.
 * THIS function is what makes the shell's read() BLOCK -- without it
 * smallsh spins printing prompts as fast as the serial line drains
 * (measured the loud way: a 45-second boot log of nothing but
 * `auralite#`).  Since A6 the wait is spelled through the irqflags
 * contract -- arch_wait_for_interrupt() IS those two instructions;
 * the backend runs on the shell's hottest blocking path, not just in
 * a compile lane. */
static uint64_t cons_a64_readline(char *buf, uint64_t cap)
{
    uint64_t n = 0;

    for (;;) {
        int ci = pl011_try_getc();
        if (ci < 0) {
            arch_wait_for_interrupt();
            continue;
        }

        char c = (char)ci;
        if (c == '\r')
            c = '\n';

        if (c == '\b' || c == 0x7F) {
            if (n > 0) {
                n--;
                pl011_puts("\b \b");
            }
            continue;
        }

        if (c == '\n') {
            pl011_putc('\n');
            if (n < cap)
                buf[n++] = '\n';
            return n;
        }

        if (n + 1 < cap) {          /* leave room for the newline */
            buf[n++] = c;
            pl011_putc(c);          /* echo */
        }
    }
}

/* Copy a NUL-terminated user path (probe every touched page first --
 * validate, then dereference; no PAN on v8.0, so the probe IS the
 * fence). */
static int copy_user_path(uint64_t uptr, char *dst, uint64_t cap)
{
    if (uptr < ELF_A64_USER_MIN ||
        uptr >= USER_TEXT_VADDR_A64 + PAGE_SIZE_A64)
        return -1;
    for (uint64_t i = 0; i < cap; i++) {
        uint64_t va = uptr + i;
        if (va >= USER_TEXT_VADDR_A64 + PAGE_SIZE_A64)
            return -1;
        if ((i == 0 || (va & (PAGE_SIZE_A64 - 1)) == 0) &&
            paging_a64_probe(va) == ~0UL)
            return -1;
        dst[i] = *(const char *)va;
        if (dst[i] == '\0')
            return 0;
    }
    return -1;                      /* unterminated within cap */
}

/* ---- PARITY P4: the fd layer (user_rv.c's mirror; no PAN on v8.0,
 * so the copy helpers are plain loops with probe checks) ---------- */

#define A64FD_BASE  3
#define A64FD_MAX   8
#define A64_USER_END (USER_TEXT_VADDR_A64 + PAGE_SIZE_A64)

static struct {
    struct vnode *vn;
    uint64_t      pos;
    int           used;
} a64fds[A64FD_MAX];

static int copy_out_user(uint64_t dst, const void *src, uint64_t len)
{
    if (len == 0 || dst < ELF_A64_USER_MIN || dst + len > A64_USER_END)
        return -1;
    for (uint64_t va = dst & ~(uint64_t)(PAGE_SIZE_A64 - 1);
         va < dst + len; va += PAGE_SIZE_A64)
        if (paging_a64_probe(va) == ~0UL)
            return -1;
    for (uint64_t i = 0; i < len; i++)
        ((char *)dst)[i] = ((const char *)src)[i];
    return 0;
}

/* ---- R6: the brk heap (user_rv.c's mirror) ------------------------- */
#define A64_HEAP_BASE 0x20000000UL
#define A64_HEAP_MAX  (A64_HEAP_BASE + 0x100000UL)
static uint64_t a64_heap_end;

static int copy_in_user(void *dst, uint64_t src, uint64_t len)
{
    if (len == 0 || src < ELF_A64_USER_MIN || src + len > A64_USER_END)
        return -1;
    for (uint64_t va = src & ~(uint64_t)(PAGE_SIZE_A64 - 1);
         va < src + len; va += PAGE_SIZE_A64)
        if (paging_a64_probe(va) == ~0UL)
            return -1;
    for (uint64_t i = 0; i < len; i++)
        ((char *)dst)[i] = ((const char *)src)[i];
    return 0;
}

static struct vnode *a64fd_lookup(const char *path)
{
    /* R2: the shared mount table (user_rv.c's mirror). */
    char abs[104];
    if (path[0] != '/') {
        abs[0] = '/';
        unsigned k = 0;
        while (path[k] && k + 2 < sizeof(abs)) {
            abs[k + 1] = path[k];
            k++;
        }
        abs[k + 1] = 0;
        return vfsm_lookup(abs);
    }
    return vfsm_lookup(path);
}

void user_a64_syscall(a64_trap_frame_t *f)
{
    uint64_t nr = f->regs[8];              /* x8 */
    uint64_t a0 = f->regs[0];
    uint64_t a1 = f->regs[1];
    uint64_t a2 = f->regs[2];

    if (!user_active) {                    /* svc from EL1 is a bug */
        pl011_puts("[user] stray EL1 svc -- ignored\n");
        return;
    }

    switch (nr) {
    case SYS_A64_READ: {
        /* read(fd=x0, buf=x1, len=x2): fd 0, cooked console line.
         * Probe every touched page BEFORE the blocking wait -- the
         * i386 rule: validate, then sleep, not the reverse. */
        /* P4: fd >= 3 reads a FILE through the mounted ops table. */
        if (a0 >= A64FD_BASE && a0 < A64FD_BASE + A64FD_MAX) {
            int i = (int)(a0 - A64FD_BASE);
            const struct vfs_ops *ops = a64fds[i].used ? a64fds[i].vn->ops : 0;
            if (!a64fds[i].used || !ops || !ops->read || a2 == 0) {
                f->regs[0] = (uint64_t)-9;             /* -EBADF */
                return;
            }
            static uint8_t fbuf[4096];
            uint64_t want = a2 > sizeof(fbuf) ? sizeof(fbuf) : a2;
            int64_t got = ops->read(a64fds[i].vn, a64fds[i].pos, fbuf, want);
            if (got < 0) {
                f->regs[0] = (uint64_t)-5;             /* -EIO */
                return;
            }
            if (got > 0 && copy_out_user(a1, fbuf, (uint64_t)got) != 0) {
                f->regs[0] = (uint64_t)-14;            /* -EFAULT */
                return;
            }
            a64fds[i].pos += (uint64_t)got;
            f->regs[0] = (uint64_t)got;
            return;
        }
        if (a0 != 0 || a2 == 0 || a2 > 4096 ||
            a1 < ELF_A64_USER_MIN ||
            a1 + a2 > USER_TEXT_VADDR_A64 + PAGE_SIZE_A64) {
            f->regs[0] = (uint64_t)-1;
            return;
        }
        for (uint64_t va = a1 & ~(uint64_t)(PAGE_SIZE_A64 - 1);
             va < a1 + a2; va += PAGE_SIZE_A64) {
            if (paging_a64_probe(va) == ~0UL) {
                static int read_faults;
                if (read_faults < 3) {
                    read_faults++;
                    pl011_puts("[user] READ EFAULT: va=");
                    put_hex_(va);
                    pl011_puts(" buf=");
                    put_hex_(a1);
                    pl011_putc('\n');
                }
                f->regs[0] = (uint64_t)-14;    /* -EFAULT */
                return;
            }
        }
        static char kline[4096];
        uint64_t n = cons_a64_readline(kline, a2);
        for (uint64_t i = 0; i < n; i++)
            ((char *)a1)[i] = kline[i];
        f->regs[0] = n;
        return;
    }
    case SYS_A64_SPAWN: {
        /* spawn(path=x0): run another initrd ELF to completion and
         * return its exit code (the rv64 nesting contract:
         * user_a64_run_elf saves/restores the parent's trampoline). */
        char path[100];
        if (copy_user_path(a0, path, sizeof(path)) != 0) {
            f->regs[0] = (uint64_t)-14;        /* -EFAULT */
            return;
        }
        f->regs[0] = (uint64_t)(int64_t)user_a64_run_elf(path);
        return;
    }
    case SYS_A64_WRITE: {
        /* R6: fd >= 3 writes a FILE through the vnode's ops. */
        if (a0 >= A64FD_BASE && a0 < A64FD_BASE + A64FD_MAX) {
            int i = (int)(a0 - A64FD_BASE);
            const struct vfs_ops *ops = a64fds[i].used ? a64fds[i].vn->ops : 0;
            if (!a64fds[i].used || !ops || !ops->write || a2 == 0) {
                f->regs[0] = (uint64_t)-9;             /* -EBADF */
                return;
            }
            static uint8_t wbuf[4096];
            uint64_t want = a2 > sizeof(wbuf) ? sizeof(wbuf) : a2;
            if (copy_in_user(wbuf, a1, want) != 0) {
                f->regs[0] = (uint64_t)-14;            /* -EFAULT */
                return;
            }
            int64_t put = ops->write(a64fds[i].vn, a64fds[i].pos, wbuf, want);
            if (put < 0) {
                f->regs[0] = (uint64_t)-5;             /* -EIO */
                return;
            }
            a64fds[i].pos += (uint64_t)put;
            f->regs[0] = (uint64_t)put;
            return;
        }
        /* write(fd=x0, buf=x1, len=x2): fd 1, bounds inside the user
         * window, pages probed, then the copy to a KERNEL buffer
         * before printing.  No PAN on v8.0 (see the header comment)
         * -- the bounds+probe pair is what stands between a user
         * pointer and the kernel's console either way. */
        /* A5c widened the window: ELF programs live in
         * [ELF_A64_USER_MIN, ELF_A64_USER_MAX) and the A4 flat-image
         * self-tests sit at USER_TEXT_VADDR_A64 just above it -- the
         * probe loop is what actually vouches for presence (the rv64
         * V5 shape). */
        if (a0 != 1 || a2 == 0 || a2 > 4096 ||
            a1 < ELF_A64_USER_MIN ||
            a1 + a2 > USER_TEXT_VADDR_A64 + PAGE_SIZE_A64) {
            f->regs[0] = (uint64_t)-1;
            return;
        }
        for (uint64_t va = a1 & ~(uint64_t)(PAGE_SIZE_A64 - 1);
             va < a1 + a2; va += PAGE_SIZE_A64) {
            if (paging_a64_probe(va) == ~0UL) {
                f->regs[0] = (uint64_t)-14;    /* -EFAULT */
                return;
            }
        }
        static char kbuf[4096];
        const char *p = (const char *)a1;
        for (uint64_t i = 0; i < a2; i++)
            kbuf[i] = p[i];
        for (uint64_t i = 0; i < a2; i++)
            pl011_putc(kbuf[i]);
        f->regs[0] = a2;
        return;
    }
    case SYS_A64_OPEN: {
        char path[100];
        if (copy_user_path(a0, path, sizeof(path)) != 0) {
            f->regs[0] = (uint64_t)-14;                /* -EFAULT */
            return;
        }
        struct vnode *vn = a64fd_lookup(path);
        if (!vn && (a1 & 1)) {                          /* R6: O_CREAT-lite */
            char abs[104];
            const char *p = path;
            if (p[0] != '/') {
                abs[0] = '/';
                unsigned k = 0;
                while (p[k] && k + 2 < sizeof(abs)) {
                    abs[k + 1] = p[k];
                    k++;
                }
                abs[k + 1] = 0;
                p = abs;
            }
            vn = vfsm_create(p);
        }
        if (!vn) {
            f->regs[0] = (uint64_t)-2;                 /* -ENOENT */
            return;
        }
        for (int i = 0; i < A64FD_MAX; i++) {
            if (!a64fds[i].used) {
                a64fds[i].vn = vn;
                a64fds[i].pos = 0;
                a64fds[i].used = 1;
                f->regs[0] = (uint64_t)(A64FD_BASE + i);
                return;
            }
        }
        f->regs[0] = (uint64_t)-24;                    /* -EMFILE */
        return;
    }

    case SYS_A64_CLOSE: {
        if (a0 < A64FD_BASE || a0 >= A64FD_BASE + A64FD_MAX ||
            !a64fds[a0 - A64FD_BASE].used) {
            f->regs[0] = (uint64_t)-9;                 /* -EBADF */
            return;
        }
        a64fds[a0 - A64FD_BASE].used = 0;
        f->regs[0] = 0;
        return;
    }

    case SYS_A64_LSEEK: {
        if (a0 < A64FD_BASE || a0 >= A64FD_BASE + A64FD_MAX ||
            !a64fds[a0 - A64FD_BASE].used) {
            f->regs[0] = (uint64_t)-9;                 /* -EBADF */
            return;
        }
        int i = (int)(a0 - A64FD_BASE);
        int64_t off = (int64_t)a1;
        uint64_t base;
        switch (a2) {
        case AURA_SEEK_SET: base = 0;                   break;
        case AURA_SEEK_CUR: base = a64fds[i].pos;       break;
        case AURA_SEEK_END: base = a64fds[i].vn->size;  break;
        default:
            f->regs[0] = (uint64_t)-22;                /* -EINVAL */
            return;
        }
        if (off < 0 && (uint64_t)(-off) > base) {
            f->regs[0] = (uint64_t)-22;
            return;
        }
        a64fds[i].pos = base + (uint64_t)off;
        f->regs[0] = a64fds[i].pos;
        return;
    }

    case SYS_A64_STAT: {
        char path[100];
        if (copy_user_path(a0, path, sizeof(path)) != 0) {
            f->regs[0] = (uint64_t)-14;
            return;
        }
        struct vnode *vn = a64fd_lookup(path);
        if (!vn) {
            f->regs[0] = (uint64_t)(a64fs_ops() ? -2 : -19);
            return;
        }
        struct aura_stat st;
        st.size   = vn->size;
        st.is_dir = (vn->type == VFS_TYPE_DIR);
        st._pad   = 0;
        f->regs[0] = copy_out_user(a1, &st, sizeof(st)) == 0
                         ? 0 : (uint64_t)-14;
        return;
    }

    case SYS_A64_READDIR: {
        if (a0 < A64FD_BASE || a0 >= A64FD_BASE + A64FD_MAX ||
            !a64fds[a0 - A64FD_BASE].used) {
            f->regs[0] = (uint64_t)-9;
            return;
        }
        struct vnode *vn = a64fds[a0 - A64FD_BASE].vn;
        const struct vfs_ops *ops = vn ? vn->ops : 0;
        if (!ops || !ops->readdir || vn->type != VFS_TYPE_DIR) {
            f->regs[0] = (uint64_t)-20;                /* -ENOTDIR */
            return;
        }
        static struct vfs_dirent ents[32];
        int n = ops->readdir(vn, ents, 32);
        if (n < 0) {
            f->regs[0] = (uint64_t)-5;
            return;
        }
        if ((int64_t)a1 < 0 || (int64_t)a1 >= n) {
            f->regs[0] = 0;                            /* end */
            return;
        }
        struct aura_dirent de;
        for (unsigned k = 0; k < sizeof(de.name); k++)
            de.name[k] = 0;
        for (unsigned k = 0; k + 1 < sizeof(de.name) &&
                             ents[a1].name[k]; k++)
            de.name[k] = ents[a1].name[k];
        de.is_dir = (ents[a1].type == VFS_TYPE_DIR);
        f->regs[0] = copy_out_user(a2, &de, sizeof(de)) == 0
                         ? 1 : (uint64_t)-14;
        return;
    }

    case SYS_A64_BRK: {
        if (a64_heap_end == 0)
            a64_heap_end = A64_HEAP_BASE;
        if (a0 == 0) {
            f->regs[0] = a64_heap_end;
            return;
        }
        if (a0 < A64_HEAP_BASE || a0 > A64_HEAP_MAX) {
            f->regs[0] = (uint64_t)-12;                /* -ENOMEM */
            return;
        }
        uint64_t want = (a0 + PAGE_SIZE_A64 - 1) & ~(uint64_t)(PAGE_SIZE_A64 - 1);
        uint64_t have = (a64_heap_end + PAGE_SIZE_A64 - 1) & ~(uint64_t)(PAGE_SIZE_A64 - 1);
        for (uint64_t va = have; va < want; va += PAGE_SIZE_A64) {
            if (paging_a64_probe(va) != ~0UL)
                continue;
            uint64_t pa = pmm_a64_alloc_frame();
            if (!pa || paging_a64_map(va, pa, A64_MAP_RW_USER) != 0) {
                f->regs[0] = (uint64_t)-12;
                return;
            }
        }
        a64_heap_end = a0;
        f->regs[0] = a64_heap_end;
        return;
    }

    case SYS_A64_GETPID:
        f->regs[0] = (uint64_t)thread_a64_current_tid();
        return;
    case SYS_A64_YIELD:
        f->regs[0] = 0;
        return;
    case SYS_A64_EXIT:
        pl011_puts("[user] exit(");
        put_udec_(a0);
        pl011_puts(") via svc\n");
        user_a64_leave(f, (int)a0);
        return;
    default:
        pl011_puts("[user] unknown syscall ");
        put_udec_(nr);
        pl011_puts(" -> -ENOSYS\n");
        f->regs[0] = (uint64_t)-38;
        return;
    }
}

int user_a64_fault(a64_trap_frame_t *f, uint64_t esr, uint64_t far)
{
    if (!user_active)
        return 0;
    pl011_puts("[user] EL0 fault: ec=");
    put_udec_((esr >> 26) & 0x3F);
    pl011_puts(" far=");
    put_hex_(far);
    pl011_puts(" elr=");
    put_hex_(f->elr);
    pl011_puts(" -- terminating image (code ");
    put_udec_(128 + ((esr >> 26) & 0x3F));
    pl011_puts(")\n");
    user_a64_leave(f, 128 + (int)((esr >> 26) & 0x3F));
    return 1;
}

/* ---- running an image ------------------------------------------------------ */

int user_a64_run_image(const uint8_t *code, uint64_t code_len)
{
    if (code_len > PAGE_SIZE_A64)
        return -1;

    uint64_t text_frame  = pmm_a64_alloc_frame();
    uint64_t stack_frame = pmm_a64_alloc_frame();
    uint8_t *trap_stack  = (uint8_t *)kmalloc_a64(TRAP_STACK_SIZE);
    if (!text_frame || !stack_frame || !trap_stack)
        goto fail_setup;

    /* Copy through the HHDM, then map with REAL permissions: text
     * EL0 R+X (no W; and PXN -- the kernel must never execute user
     * pages, W^X's second axis), stack EL0 R+W (no X anywhere). */
    uint8_t *dst = (uint8_t *)p2v_a64(text_frame);
    for (uint64_t i = 0; i < code_len; i++)
        dst[i] = code[i];

    if (paging_a64_map(USER_TEXT_VADDR_A64, text_frame,
                       A64_MAP_RX_USER) != 0 ||
        paging_a64_map(USER_STACK_VADDR_A64, stack_frame,
                       A64_MAP_RW_USER) != 0)
        goto fail_mapped;

    user_active = 1;

    int rc = a64_setjmp(exit_jmpbuf);
    if (rc == 0) {
        /* First arrival: down to EL0.  Never returns -- the image
         * leaves through SYS_A64_EXIT or a contained fault, both of
         * which longjmp back here with rc = 1. */
        user_enter_a64(USER_TEXT_VADDR_A64,
                       USER_STACK_TOP_A64 - 16,
                       (uint64_t)trap_stack + TRAP_STACK_SIZE);
    }

    /* Landed from user_a64_leave: the code sits in the box.  IRQ is
     * masked across the unwind (the rv SPIE lesson's DAIF spelling);
     * reopen now that sp is this kernel stack again. */
    __asm__ volatile("msr daifclr, #2");
    (void)rc;
    paging_a64_unmap(USER_TEXT_VADDR_A64);
    paging_a64_unmap(USER_STACK_VADDR_A64);
    kfree_a64(trap_stack);
    pmm_a64_free_frame(text_frame);
    pmm_a64_free_frame(stack_frame);
    return exit_code_box;

fail_mapped:
    paging_a64_unmap(USER_TEXT_VADDR_A64);
    paging_a64_unmap(USER_STACK_VADDR_A64);
fail_setup:
    if (trap_stack)
        kfree_a64(trap_stack);
    if (text_frame)
        pmm_a64_free_frame(text_frame);
    if (stack_frame)
        pmm_a64_free_frame(stack_frame);
    return -1;
}

/* ---- the [user] gate ---------------------------------------------------------
 *
 * Two flat EL0 images, hand-assembled A64 (the prog_ok tradition:
 * encodings hand-checked, and the smoke test's exact-string assert
 * is the real reviewer).
 *
 * prog_ok:   write(1, msg@+0x48, 10); getpid(); exit(42)
 * prog_priv: mrs x0, sctlr_el1 -- privileged read from EL0, must be
 *            contained as 128 + EC 0x18 (trapped MSR/MRS... measured:
 *            QEMU raises EC 0x00 Unknown for this at EL0, which the
 *            result records -- the assertion tracks the measurement).
 */

/* Bytes MEASURED, not hand-rolled: assembled with the tree's own
 * clang from the .S in the comment, objcopy'd, od'd, pasted -- the
 * V0 pad-byte lesson says the assembler is the reviewer, and the
 * literal pool the assembler emitted (msg address at +0x30, ldr
 * literal) is kept exactly as it fell out. */
/* ---- A5c: ELF images from the initrd (user_rv_run_elf's contract) ---------
 *
 * spawn depth 0 = launched from kmain_a64, 1 = a child the shell
 * spawned.  Each nesting level gets its own user stack page (stepping
 * down from the ELF window's ceiling -- the address-range treaty) and
 * its own trap stack (the I7 lesson: the parent's trap stack holds
 * ITS live frames while the child runs), and the parent's
 * exit-trampoline context is saved around the child.  All of it in
 * TTBR0's tree (the A4 two-trees fact) -- and every unmap on the way
 * out is a precise TLBI VAE1IS now ([AMEND-4], paging_a64_unmap). */

static uint64_t spawn_depth;

int user_a64_run_elf(const char *path)
{
    const uint8_t *image;
    uint64_t size;

    if (spawn_depth >= 2) {
        pl011_puts("[user] spawn depth limit (2) -- refusing\n");
        return -1;
    }

    if (!initrd_a64_find(path, &image, &size)) {
        pl011_puts("[user] /");
        pl011_puts(path);
        pl011_puts(": not found in the initrd\n");
        return -1;
    }

    /* Checkpoint the parent's mappings (no-op at depth 0) and its
     * exit-trampoline context. */
    elfa64load_mark();
    uint64_t p_jmpbuf[14];
    for (int i = 0; i < 14; i++)
        p_jmpbuf[i] = exit_jmpbuf[i];
    int p_active = user_active;
    /* THE fix this phase paid a debugging session for: the trap frame
     * does not carry SP_EL0 (the vectors spill GPRs+elr+spsr only), so
     * a nested user_enter_a64 silently re-points the PARENT's EL0
     * stack at the child's -- which the child's exit then unmaps.  The
     * parent resumes with its sp inside a dead page and every read()
     * floods -EFAULT (measured: 20 MB of prompts in 40 s, buf equal to
     * the CHILD's stack top minus its frame -- the "impossible" 0x500C
     * depth was the two levels' top-to-top distance all along).  Save
     * the banked register with the rest of the parent's context. */
    uint64_t p_sp_el0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(p_sp_el0));

    uint64_t entry = elfa64load_map(image, size);
    if (!entry) {
        elfa64load_unmap();         /* releases to the mark */
        return -1;
    }

    /* User stack: FOUR pages per nesting level plus an unmapped guard
     * page between levels.  The rv64 one-page-per-level layout was
     * ported first and measured broken here: aarch64 frames are fatter
     * (AAPCS64 16-byte alignment, clang -O2 spills), the shell's SP
     * grew down THROUGH its single page onto the child's -- legal
     * while the child lived, vaporised when the child's exit unmapped
     * it, and every parent read() after that was an EFAULT flood of
     * prompts.  The guard hole turns a future overflow into a
     * contained EL0 fault instead of a silent lease on the
     * neighbour's page. */
    enum { USTACK_PAGES = 8, USTACK_STRIDE = USTACK_PAGES + 1 };
    /* 8 pages + a guard hole per level.  The "20 KiB deep shell" that
     * first justified widening this was the SP_EL0 bug wearing a
     * disguise (see below) -- but the width stays: stacks that share
     * no border with a neighbour turn a future overflow into a
     * contained EL0 fault instead of a lease on someone else's page. */
    uint64_t stack_top  = ELF_A64_USER_MAX -
        (uint64_t)PAGE_SIZE_A64 * USTACK_STRIDE * spawn_depth;
    uint64_t stack_lo   = stack_top - (uint64_t)PAGE_SIZE_A64 * USTACK_PAGES;
    uint64_t stack_frames[USTACK_PAGES];
    uint32_t stacked = 0;
    for (; stacked < USTACK_PAGES; stacked++) {
        uint64_t fr = pmm_a64_alloc_frame();
        if (!fr ||
            paging_a64_map(stack_lo + (uint64_t)PAGE_SIZE_A64 * stacked,
                           fr, A64_MAP_RW_USER) != 0) {
            if (fr)
                pmm_a64_free_frame(fr);
            break;
        }
        stack_frames[stacked] = fr;
    }
    if (stacked < USTACK_PAGES) {
        while (stacked--) {
            paging_a64_unmap(stack_lo + (uint64_t)PAGE_SIZE_A64 * stacked);
            pmm_a64_free_frame(stack_frames[stacked]);
        }
        elfa64load_unmap();
        return -1;
    }

    uint8_t *trap_stack = (uint8_t *)kmalloc_a64(TRAP_STACK_SIZE);
    if (!trap_stack) {
        for (uint32_t s = 0; s < USTACK_PAGES; s++) {
            paging_a64_unmap(stack_lo + (uint64_t)PAGE_SIZE_A64 * s);
            pmm_a64_free_frame(stack_frames[s]);
        }
        elfa64load_unmap();
        return -1;
    }

    pl011_puts("[user] running /");
    pl011_puts(path);
    pl011_puts(" (entry ");
    put_hex_(entry);
    pl011_puts(", ");
    put_udec_(size);
    pl011_puts(" bytes, depth ");
    put_udec_(spawn_depth);
    pl011_puts(")\n");

    spawn_depth++;
    user_active = 1;

    int rc = a64_setjmp(exit_jmpbuf);
    if (rc == 0) {
        user_enter_a64(entry, stack_top - 16,
                       (uint64_t)trap_stack + TRAP_STACK_SIZE);
    }

    /* Landed from user_a64_leave (setjmp returns 1; the code travels
     * in exit_code_box -- the A4 trampoline's convention, NOT the rv
     * rc-1 encoding; copying that cost this phase its first bug:
     * every child "exited 0").  IRQs are masked across the unwind --
     * re-open now that sp is this kernel stack again. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    (void)rc;
    int code = exit_code_box;
    spawn_depth--;

    kfree_a64(trap_stack);
    for (uint32_t s = 0; s < USTACK_PAGES; s++) {
        paging_a64_unmap(stack_lo + (uint64_t)PAGE_SIZE_A64 * s);
        pmm_a64_free_frame(stack_frames[s]);
    }
    elfa64load_unmap();             /* child's pages only, to the mark */

    /* Restore the parent's trampoline AND its EL0 stack pointer (see
     * the save above): the next SYS_EXIT must land in the OUTER frame,
     * and the eret back to the parent must resume on the PARENT's
     * stack. */
    for (int i = 0; i < 14; i++)
        exit_jmpbuf[i] = p_jmpbuf[i];
    user_active = p_active;
    __asm__ volatile("msr sp_el0, %0" :: "r"(p_sp_el0));

    return code;
}

static const uint8_t prog_ok[] = {
    /*  mov x8,#1; mov x0,#1; ldr x1,=0x40000048; mov x2,#10; svc #0
     *  mov x8,#39; svc #0;  mov x8,#60; mov x0,#42; svc #0; udf     */
    0x28, 0x00, 0x80, 0xD2,   /* mov  x8, #1            */
    0x20, 0x00, 0x80, 0xD2,   /* mov  x0, #1            */
    0x41, 0x01, 0x00, 0x58,   /* ldr  x1, [pc+0x28] = literal @+0x30 */
    0x42, 0x01, 0x80, 0xD2,   /* mov  x2, #10           */
    0x01, 0x00, 0x00, 0xD4,   /* svc  #0                */
    0xE8, 0x04, 0x80, 0xD2,   /* mov  x8, #39           */
    0x01, 0x00, 0x00, 0xD4,   /* svc  #0                */
    0x88, 0x07, 0x80, 0xD2,   /* mov  x8, #60           */
    0x40, 0x05, 0x80, 0xD2,   /* mov  x0, #42           */
    0x01, 0x00, 0x00, 0xD4,   /* svc  #0                */
    0x00, 0x00, 0x00, 0x00,   /* udf #0 (never runs)    */
    0x00, 0x00, 0x00, 0x00,   /* pad to the literal     */
    0x48, 0x00, 0x00, 0x40,   /* +0x30: .quad 0x40000048 (lo) */
    0x00, 0x00, 0x00, 0x00,   /*                        (hi) */
    0x00, 0x00, 0x00, 0x00,   /* pad: msg at +0x48      */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    /* +0x48: the message */
    'A', '6', '4', '-', 'U', '-', 'O', 'K', '!', '\n',
};

static const uint8_t prog_priv[] = {
    0x00, 0x10, 0x38, 0xD5,   /* mrs x0, sctlr_el1 -- EL1 register,
                               * from EL0: trap, never a value */
    0x88, 0x07, 0x80, 0xD2,   /* mov x8, #60 (not reached) */
    0x01, 0x00, 0x00, 0xD4,   /* svc #0 */
};

int user_a64_selftest(void)
{
    pl011_puts("[user] self-test: entering EL0...\n");

    int code = user_a64_run_image(prog_ok, sizeof(prog_ok));
    if (code != 42) {
        pl011_puts("[user] FAIL: expected exit 42, got ");
        put_udec_((uint64_t)code);
        pl011_puts("\n");
        return -1;
    }
    pl011_puts("[user] exit code 42 round-tripped\n");

    pl011_puts("[user] negative control: privileged mrs from EL0...\n");
    code = user_a64_run_image(prog_priv, sizeof(prog_priv));
    if (code < 128) {
        pl011_puts("[user] FAIL: privileged op NOT contained, code ");
        put_udec_((uint64_t)code);
        pl011_puts("\n");
        return -1;
    }
    pl011_puts("[user] PASS: privileged op contained (code ");
    put_udec_((uint64_t)code);
    pl011_puts("), kernel intact\n");
    return 0;
}
