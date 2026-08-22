/* kernel/arch/riscv64/user_rv.c -- U-mode + ecall (RISCV_PLAN V4).
 *
 * user32.c's privilege round trip, this ISA's spelling:
 *
 *   kernel thread -> user_rv_run_image
 *     copy the image to a fresh frame, map it at 0x40000000 with
 *     PTE_U|R|X -- REAL permissions: W^X holds in U-mode too, V3's
 *     machinery finally meets the user PTEs it was built for
 *     user stack below it (PTE_U|R|W, and pointedly NOT X)
 *     user_enter_rv: sscratch := dedicated trap stack, sret
 *   U-mode -> ecall (a7 number / a0-a5 args / a0 return, D4)
 *     trapentry.S swaps onto the trap stack, rv_trap routes here
 *   SYS_RV_EXIT longjmps the kernel thread out of the user context
 *     (rv_setjmp/rv_longjmp_entry -- V3's fault-probe pair IS the
 *     exit trampoline; one mechanism, two tenants)
 *
 * A U-mode fault (SPP=0 in the frame) terminates the image with
 * 128+scause through the same longjmp -- the kernel survives, the
 * negative control depends on it.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/user_rv.h"
#include "kernel/arch/riscv64/fsglue_rv.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/vfsmount.h"
#include "lib/abi/fsabi.h"
#include "kernel/arch/riscv64/elfrvload.h"
#include "kernel/arch/riscv64/initrd_rv.h"
#include "kernel/arch/riscv64/kheap_rv.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/thread_rv.h"
#include "kernel/arch/riscv64/uart_rv.h"
#include "kernel/arch/arch.h"

/* context_rv.S */
extern void user_enter_rv(uint64_t entry, uint64_t user_sp,
                          uint64_t trap_stack_top)
    __attribute__((noreturn));
/* trapentry.S */
extern int  rv_setjmp(uint64_t *buf);
extern void rv_longjmp_entry(void);

#define USER_STACK_TOP_RV   USER_TEXT_VADDR_RV
#define USER_STACK_VADDR_RV (USER_STACK_TOP_RV - PAGE_SIZE_RV)
#define TRAP_STACK_SIZE     8192

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}
static void put_hex_(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    sbi_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        sbi_putc(hex[(v >> shift) & 0xF]);
}

/* One image at a time (boot-hart bring-up, D5). */
static uint64_t exit_jmpbuf[14];
static volatile int user_active;

static void user_rv_leave(rv_trap_frame_t *f, int code)
{
    /* Rewrite the frame so sret lands in rv_longjmp_entry IN S-MODE:
     * a0 = jmpbuf, a1 = code (setjmp returns 1 + code).  SPP set.
     *
     * SPIE is CLEARED, and that line was bought with a debugging
     * session: sret restores SIE from SPIE, and this frame's exit
     * path momentarily runs rv_longjmp_entry with sp still holding
     * the trapped USER stack pointer (the x2 slot of a U-trap).  A
     * timer interrupt in that two-instruction window built its frame
     * on the user stack from S-mode -- store-page-fault recursion
     * descending 288 bytes per iteration, measured in -d int.  So:
     * interrupts stay off across the unwind; user_rv_run_image turns
     * them back on once sp is a kernel stack again. */
    user_active = 0;
    f->sepc     = (uint64_t)rv_longjmp_entry;
    f->sstatus |= (1UL << 8);              /* SPP = 1: sret stays in S */
    f->sstatus &= ~(1UL << 5);             /* SPIE = 0: no IRQ window  */
    f->regs[9]  = (uint64_t)exit_jmpbuf;   /* x10 = a0 */
    f->regs[10] = (uint64_t)code;          /* x11 = a1 */
}

/* ---- V5: the cooked console line (V7: interrupt-fed) -----------------------
 *
 * cons32_readline's shape: echo, backspace, CR->LF, the newline
 * stored.  Since V7 the byte source is the UART RX ring fed by the
 * PLIC (uart_rv_getc), with sbi_getchar kept as the fallback for
 * boots where the UART driver did not come up.  The blocking wait is
 * arch_wait_for_interrupt() -- wfi with SIE forced on.  The I7
 * cleared-IF deadlock has an sstatus.SIE twin: a bare wfi inside an
 * ecall trap window (SPIE off, SIE off after any nested trap) would
 * sleep with a prompt on screen exactly the way the i386 hlt did;
 * the helper's csrsi is the sti in sti;hlt, third spelling. */

uint64_t cons_rv_readline(char *buf, uint64_t cap)
{
    uint64_t n = 0;

    for (;;) {
        int ci = uart_rv_getc();
        if (ci < 0)
            ci = sbi_getchar();
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
                sbi_puts("\b \b");
            }
            continue;
        }

        if (c == '\n') {
            sbi_putc('\n');
            if (n < cap)
                buf[n++] = '\n';
            return n;
        }

        if (n + 1 < cap) {          /* leave room for the newline */
            buf[n++] = c;
            sbi_putc(c);            /* echo */
        }
    }
}

/* SUM window helpers: every user-memory access from the kernel goes
 * through these -- the V4-measured fact that sstatus.SUM is OFF by
 * default and a bare kernel load from a PTE_U page traps. */
static inline void sum_on(void)
{
    __asm__ volatile("csrs sstatus, %0" :: "r"(1UL << 18));
}
static inline void sum_off(void)
{
    __asm__ volatile("csrc sstatus, %0" :: "r"(1UL << 18));
}

/* Copy a NUL-terminated user string.  The bring-up stand-in for
 * strncpy_from_user: bounds inside the user window, SUM around the
 * byte loop.  0 on success. */
static int copy_user_path(uint64_t uptr, char *dst, uint64_t cap)
{
    if (uptr < ELF_RV_USER_MIN || uptr >= ELF_RV_USER_MAX)
        return -1;
    int rc = -1;                     /* unterminated within cap */
    sum_on();
    for (uint64_t i = 0; i < cap - 1; i++) {
        if (uptr + i >= ELF_RV_USER_MAX)
            break;
        if (paging_rv_probe(uptr + i) == ~0UL)
            break;
        dst[i] = *(const char *)(uptr + i);
        if (!dst[i]) { rc = 0; break; }
    }
    sum_off();
    return rc;
}

/* ---- PARITY P4: the fd layer -------------------------------------------- */
/* Dispatcher state, by the same right as the dispatcher itself is
 * arch code.  fd 0/1/2 stay the console; 3..10 are files on the
 * filesystem fsglue mounted (NULL ops = every file syscall says
 * -ENODEV, honestly).  One shell, no threads to race: a fixed table
 * with no locks is the truth of this bring-up port, not a shortcut. */

#define RVFD_BASE  3
#define RVFD_MAX   8

static struct {
    struct vnode *vn;
    uint64_t      pos;
    int           used;
} rvfds[RVFD_MAX];

/* Validate + copy INTO user memory (the READ case's probe loop,
 * factored): 0 on success. */
static int copy_out_user(uint64_t dst, const void *src, uint64_t len)
{
    if (len == 0 || dst < ELF_RV_USER_MIN || dst + len > ELF_RV_USER_MAX)
        return -1;
    for (uint64_t va = dst & ~(uint64_t)(PAGE_SIZE_RV - 1);
         va < dst + len; va += PAGE_SIZE_RV)
        if (paging_rv_probe(va) == ~0UL)
            return -1;
    sum_on();
    for (uint64_t i = 0; i < len; i++)
        ((char *)dst)[i] = ((const char *)src)[i];
    sum_off();
    return 0;
}

static struct vnode *rvfd_lookup(const char *path)
{
    /* R2: resolution goes through the SHARED mount table.  The
     * shell may pass relative names (P4's cat LINUX.TXT); absolute
     * them against the root mount. */
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

/* ---- trap.c hooks --------------------------------------------------------- */

void user_rv_syscall(rv_trap_frame_t *f)
{
    uint64_t nr = f->regs[16];             /* x17 = a7 */
    uint64_t a0 = f->regs[9];              /* x10 = a0 */
    uint64_t a1 = f->regs[10];
    uint64_t a2 = f->regs[11];

    if (!user_active) {                    /* ecall from S-mode is a bug */
        sbi_puts("[user] stray S-mode ecall -- ignored\n");
        return;
    }

    switch (nr) {
    case SYS_RV_READ: {
        /* read(fd=a0, buf=a1, len=a2): fd 0, cooked console line.
         * Probe every touched page BEFORE the blocking wait -- the
         * i386 rule: validate, then sleep, not the reverse. */
        /* P4: fd >= 3 reads a FILE through the mounted ops table
         * (pos advances); fd 0 keeps the cooked console path below. */
        if (a0 >= RVFD_BASE && a0 < RVFD_BASE + RVFD_MAX) {
            int i = (int)(a0 - RVFD_BASE);
            const struct vfs_ops *ops = rvfds[i].used ? rvfds[i].vn->ops : 0;
            if (!rvfds[i].used || !ops || !ops->read || a2 == 0) {
                f->regs[9] = (uint64_t)-9;             /* -EBADF */
                return;
            }
            static uint8_t fbuf[4096];
            uint64_t want = a2 > sizeof(fbuf) ? sizeof(fbuf) : a2;
            int64_t got = ops->read(rvfds[i].vn, rvfds[i].pos, fbuf, want);
            if (got < 0) {
                f->regs[9] = (uint64_t)-5;             /* -EIO */
                return;
            }
            if (got > 0 && copy_out_user(a1, fbuf, (uint64_t)got) != 0) {
                f->regs[9] = (uint64_t)-14;            /* -EFAULT */
                return;
            }
            rvfds[i].pos += (uint64_t)got;
            f->regs[9] = (uint64_t)got;
            return;
        }
        if (a0 != 0 || a2 == 0 || a2 > 4096 ||
            a1 < ELF_RV_USER_MIN || a1 + a2 > ELF_RV_USER_MAX) {
            f->regs[9] = (uint64_t)-1;
            return;
        }
        for (uint64_t va = a1 & ~(uint64_t)(PAGE_SIZE_RV - 1);
             va < a1 + a2; va += PAGE_SIZE_RV) {
            if (paging_rv_probe(va) == ~0UL) {
                f->regs[9] = (uint64_t)-14;    /* -EFAULT */
                return;
            }
        }
        static char kline[4096];
        uint64_t n = cons_rv_readline(kline, a2);
        sum_on();
        for (uint64_t i = 0; i < n; i++)
            ((char *)a1)[i] = kline[i];
        sum_off();
        f->regs[9] = n;
        return;
    }
    case SYS_RV_WRITE: {
        /* write(fd=a0, buf=a1, len=a2): fd 1, bounds inside the user
         * window, pages probed (V5 widened the window from V4's two
         * fixed pages to the ELF range, so presence is no longer
         * implied), then the copy through the SUM window.
         *
         * SUM is the mechanism here, measured the hard way in V4:
         * with sstatus.SUM=0 (the reset default -- SMAP's sibling,
         * on by default on this ISA) the kernel's OWN load from a
         * PTE_U page traps.  Bounds first (SUM makes user memory
         * reachable, not kernel memory safe), SUM on, copy to a
         * KERNEL buffer, SUM off, then print. */
        /* The window spans BOTH user worlds: ELF programs live in
         * [ELF_RV_USER_MIN, ELF_RV_USER_MAX) and the V4 flat-image
         * self-tests sit one page above it at USER_TEXT_VADDR_RV --
         * the probe loop is what actually vouches for presence. */
        if (a0 != 1 || a2 == 0 || a2 > 4096 ||
            a1 < ELF_RV_USER_MIN ||
            a1 + a2 > USER_TEXT_VADDR_RV + PAGE_SIZE_RV) {
            f->regs[9] = (uint64_t)-1;
            return;
        }
        for (uint64_t va = a1 & ~(uint64_t)(PAGE_SIZE_RV - 1);
             va < a1 + a2; va += PAGE_SIZE_RV) {
            if (paging_rv_probe(va) == ~0UL) {
                f->regs[9] = (uint64_t)-14;    /* -EFAULT */
                return;
            }
        }
        static char kbuf[4096];
        const char *p = (const char *)a1;
        sum_on();
        for (uint64_t i = 0; i < a2; i++)
            kbuf[i] = p[i];
        sum_off();
        for (uint64_t i = 0; i < a2; i++)
            sbi_putc(kbuf[i]);
        f->regs[9] = a2;
        return;
    }
    case SYS_RV_SPAWN: {
        /* spawn(path=a0): run another initrd ELF to completion and
         * return its exit code.  Nested inside the caller's own
         * user_rv_run context, so the parent's trampoline slot is
         * saved and restored by user_rv_run_elf itself. */
        char path[100];
        if (copy_user_path(a0, path, sizeof(path)) != 0) {
            f->regs[9] = (uint64_t)-14;        /* -EFAULT */
            return;
        }
        f->regs[9] = (uint64_t)(int64_t)user_rv_run_elf(path);
        return;
    }
    case SYS_RV_OPEN: {
        char path[100];
        if (copy_user_path(a0, path, sizeof(path)) != 0) {
            f->regs[9] = (uint64_t)-14;                /* -EFAULT */
            return;
        }
        struct vnode *vn = rvfd_lookup(path);
        if (!vn) {
            f->regs[9] = (uint64_t)-2;                 /* -ENOENT */
            return;
        }
        for (int i = 0; i < RVFD_MAX; i++) {
            if (!rvfds[i].used) {
                rvfds[i].vn = vn;
                rvfds[i].pos = 0;
                rvfds[i].used = 1;
                f->regs[9] = (uint64_t)(RVFD_BASE + i);
                return;
            }
        }
        f->regs[9] = (uint64_t)-24;                    /* -EMFILE */
        return;
    }

    case SYS_RV_CLOSE: {
        if (a0 < RVFD_BASE || a0 >= RVFD_BASE + RVFD_MAX ||
            !rvfds[a0 - RVFD_BASE].used) {
            f->regs[9] = (uint64_t)-9;                 /* -EBADF */
            return;
        }
        rvfds[a0 - RVFD_BASE].used = 0;
        f->regs[9] = 0;
        return;
    }

    case SYS_RV_LSEEK: {
        if (a0 < RVFD_BASE || a0 >= RVFD_BASE + RVFD_MAX ||
            !rvfds[a0 - RVFD_BASE].used) {
            f->regs[9] = (uint64_t)-9;                 /* -EBADF */
            return;
        }
        int i = (int)(a0 - RVFD_BASE);
        int64_t off = (int64_t)a1;
        uint64_t base;
        switch (a2) {
        case AURA_SEEK_SET: base = 0;                  break;
        case AURA_SEEK_CUR: base = rvfds[i].pos;       break;
        case AURA_SEEK_END: base = rvfds[i].vn->size;  break;
        default:
            f->regs[9] = (uint64_t)-22;                /* -EINVAL */
            return;
        }
        if (off < 0 && (uint64_t)(-off) > base) {
            f->regs[9] = (uint64_t)-22;
            return;
        }
        rvfds[i].pos = base + (uint64_t)off;
        f->regs[9] = rvfds[i].pos;
        return;
    }

    case SYS_RV_STAT: {
        char path[100];
        if (copy_user_path(a0, path, sizeof(path)) != 0) {
            f->regs[9] = (uint64_t)-14;
            return;
        }
        struct vnode *vn = rvfd_lookup(path);
        if (!vn) {
            f->regs[9] = (uint64_t)(rvfs_ops() ? -2 : -19);  /* -ENOENT / -ENODEV */
            return;
        }
        struct aura_stat st;
        st.size   = vn->size;
        st.is_dir = (vn->type == VFS_TYPE_DIR);
        st._pad   = 0;
        f->regs[9] = copy_out_user(a1, &st, sizeof(st)) == 0
                         ? 0 : (uint64_t)-14;
        return;
    }

    case SYS_RV_READDIR: {
        if (a0 < RVFD_BASE || a0 >= RVFD_BASE + RVFD_MAX ||
            !rvfds[a0 - RVFD_BASE].used) {
            f->regs[9] = (uint64_t)-9;
            return;
        }
        struct vnode *vn = rvfds[a0 - RVFD_BASE].vn;
        const struct vfs_ops *ops = vn ? vn->ops : 0;
        if (!ops || !ops->readdir || vn->type != VFS_TYPE_DIR) {
            f->regs[9] = (uint64_t)-20;                /* -ENOTDIR */
            return;
        }
        static struct vfs_dirent ents[32];
        int n = ops->readdir(vn, ents, 32);
        if (n < 0) {
            f->regs[9] = (uint64_t)-5;
            return;
        }
        if ((int64_t)a1 < 0 || (int64_t)a1 >= n) {
            f->regs[9] = 0;                            /* end */
            return;
        }
        struct aura_dirent de;
        for (unsigned k = 0; k < sizeof(de.name); k++)
            de.name[k] = 0;
        for (unsigned k = 0; k + 1 < sizeof(de.name) &&
                             ents[a1].name[k]; k++)
            de.name[k] = ents[a1].name[k];
        de.is_dir = (ents[a1].type == VFS_TYPE_DIR);
        f->regs[9] = copy_out_user(a2, &de, sizeof(de)) == 0
                         ? 1 : (uint64_t)-14;
        return;
    }

    case SYS_RV_GETPID:
        f->regs[9] = (uint64_t)thread_rv_current_tid();
        return;
    case SYS_RV_YIELD:
        f->regs[9] = 0;
        return;
    case SYS_RV_EXIT:
        sbi_puts("[user] exit(");
        put_udec_(a0);
        sbi_puts(") via ecall\n");
        user_rv_leave(f, (int)a0);
        return;
    default:
        sbi_puts("[user] unknown syscall ");
        put_udec_(nr);
        sbi_puts(" -> -ENOSYS\n");
        f->regs[9] = (uint64_t)-38;
        return;
    }
}

int user_rv_fault(rv_trap_frame_t *f, uint64_t scause, uint64_t stval)
{
    if (!user_active)
        return 0;
    sbi_puts("[user] U-mode fault: scause=");
    put_udec_(scause);
    sbi_puts(" stval=");
    put_hex_(stval);
    sbi_puts(" sepc=");
    put_hex_(f->sepc);
    sbi_puts(" -- terminating image (code ");
    put_udec_(128 + scause);
    sbi_puts(")\n");
    user_rv_leave(f, 128 + (int)scause);
    return 1;
}

/* ---- running an image ------------------------------------------------------ */

int user_rv_run_image(const uint8_t *code, uint64_t code_len)
{
    if (code_len > PAGE_SIZE_RV)
        return -1;

    uint64_t text_frame  = pmm_rv_alloc_frame();
    uint64_t stack_frame = pmm_rv_alloc_frame();
    uint8_t *trap_stack  = (uint8_t *)kmalloc_rv(TRAP_STACK_SIZE);
    if (!text_frame || !stack_frame || !trap_stack)
        goto fail_setup;

    /* Copy through the HHDM, then map with REAL permissions: text
     * PTE_U|R|X (no W -- V3's W^X extends to user PTEs the moment
     * they exist), stack PTE_U|R|W (no X). */
    uint8_t *dst = (uint8_t *)p2v_rv(text_frame);
    for (uint64_t i = 0; i < code_len; i++)
        dst[i] = code[i];

    if (paging_rv_map(USER_TEXT_VADDR_RV, text_frame,
                      PTE_U | PTE_R | PTE_X) != 0 ||
        paging_rv_map(USER_STACK_VADDR_RV, stack_frame,
                      PTE_U | PTE_R | PTE_W) != 0)
        goto fail_mapped;

    user_active = 1;

    int rc = rv_setjmp(exit_jmpbuf);
    if (rc == 0) {
        /* First arrival: down to U-mode.  Never returns -- the image
         * leaves through SYS_RV_EXIT or a contained fault, both of
         * which longjmp back here with rc = 1 + code. */
        user_enter_rv(USER_TEXT_VADDR_RV,
                      USER_STACK_TOP_RV - 16,
                      (uint64_t)trap_stack + TRAP_STACK_SIZE);
    }

    /* Landed from user_rv_leave: rc == 1 + exit code.  sscratch is 0
     * again (trapentry zeroed it on the way in; the sret that landed
     * here had SPP=1 so it was NOT re-armed).  Interrupts are OFF --
     * the unwind ran SPIE=0 (see user_rv_leave); back on now that sp
     * is this kernel stack. */
    __asm__ volatile("csrsi sstatus, 2");
    paging_rv_unmap(USER_TEXT_VADDR_RV);
    paging_rv_unmap(USER_STACK_VADDR_RV);
    kfree_rv(trap_stack);
    pmm_rv_free_frame(text_frame);
    pmm_rv_free_frame(stack_frame);
    return rc - 1;

fail_mapped:
    paging_rv_unmap(USER_TEXT_VADDR_RV);
    paging_rv_unmap(USER_STACK_VADDR_RV);
fail_setup:
    if (trap_stack)
        kfree_rv(trap_stack);
    if (text_frame)
        pmm_rv_free_frame(text_frame);
    if (stack_frame)
        pmm_rv_free_frame(stack_frame);
    return -1;
}

/* ---- V5: ELF images from the initrd -----------------------------------------
 *
 * user32_run_elf's shape: spawn depth 0 = launched from kmain_rv,
 * 1 = child spawned by a user image (the shell).  Each nesting level
 * needs its own user stack page address AND the parent's
 * exit-trampoline context (the jmpbuf + user_active) saved around
 * the child -- user_rv_leave must unwind to the INNERMOST run, and
 * the parent must be resumable afterwards. */

static uint64_t spawn_depth;

int user_rv_run_elf(const char *path)
{
    const uint8_t *image;
    uint64_t size;

    if (spawn_depth >= 2) {
        sbi_puts("[user] spawn depth limit (2) -- refusing\n");
        return -1;
    }

    if (!initrd_rv_find(path, &image, &size)) {
        sbi_puts("[user] /");
        sbi_puts(path);
        sbi_puts(": not found in the initrd\n");
        return -1;
    }

    /* Checkpoint the parent's mappings (no-op at depth 0) and its
     * exit-trampoline context. */
    elfrvload_mark();
    uint64_t p_jmpbuf[14];
    for (int i = 0; i < 14; i++)
        p_jmpbuf[i] = exit_jmpbuf[i];
    int p_active = user_active;

    uint64_t entry = elfrvload_map(image, size);
    if (!entry) {
        elfrvload_unmap();          /* releases to the mark */
        return -1;
    }

    /* User stack: one page per nesting level, stepping down from the
     * ELF window's ceiling so parent and child stacks never collide
     * (one shared root table at V5 -- the address-range treaty). */
    uint64_t stack_page  = ELF_RV_USER_MAX - PAGE_SIZE_RV * (spawn_depth + 1);
    uint64_t stack_frame = pmm_rv_alloc_frame();
    if (!stack_frame ||
        paging_rv_map(stack_page, stack_frame,
                      PTE_U | PTE_R | PTE_W) != 0) {
        if (stack_frame)
            pmm_rv_free_frame(stack_frame);
        elfrvload_unmap();
        return -1;
    }

    /* A DEDICATED trap stack per nesting level (the I7 lesson: the
     * parent's trap stack has ITS live trap frames on it while the
     * child runs -- the child needs an empty one). */
    uint8_t *trap_stack = (uint8_t *)kmalloc_rv(TRAP_STACK_SIZE);
    if (!trap_stack) {
        paging_rv_unmap(stack_page);
        pmm_rv_free_frame(stack_frame);
        elfrvload_unmap();
        return -1;
    }

    sbi_puts("[user] running /");
    sbi_puts(path);
    sbi_puts(" (entry ");
    put_hex_(entry);
    sbi_puts(", ");
    put_udec_(size);
    sbi_puts(" bytes, depth ");
    put_udec_(spawn_depth);
    sbi_puts(")\n");

    spawn_depth++;
    user_active = 1;

    int rc = rv_setjmp(exit_jmpbuf);
    if (rc == 0) {
        user_enter_rv(entry, stack_page + PAGE_SIZE_RV - 16,
                      (uint64_t)trap_stack + TRAP_STACK_SIZE);
    }

    /* Landed from user_rv_leave with rc = 1 + code; interrupts are
     * off across the unwind (the V4 SPIE fact), re-open now. */
    __asm__ volatile("csrsi sstatus, 2");
    int code = rc - 1;
    spawn_depth--;

    kfree_rv(trap_stack);
    paging_rv_unmap(stack_page);
    pmm_rv_free_frame(stack_frame);
    elfrvload_unmap();              /* child's pages only, to the mark */

    /* Restore the parent's trampoline: the next SYS_EXIT (the
     * parent's own) must land in the OUTER user_rv_run_elf frame. */
    for (int i = 0; i < 14; i++)
        exit_jmpbuf[i] = p_jmpbuf[i];
    user_active = p_active;

    return code;
}

/* ---- the boot self-test ------------------------------------------------------
 *
 * Hand-assembled U-mode program (clang -c equivalent, kept short
 * enough to read; rv64i only, no compressed encodings).  Assembly:
 *
 *     li   a7, 1          # SYS_RV_WRITE
 *     li   a0, 1          # fd
 *     lui  a1, 0x40000    # buf = 0x40000000 + 0x48 (msg below)
 *     addi a1, a1, 0x48
 *     li   a2, 10         # len("RING-U-OK\n")
 *     ecall
 *     li   a7, 39         # SYS_RV_GETPID
 *     ecall
 *     li   a7, 60         # SYS_RV_EXIT
 *     li   a0, 42
 *     ecall
 *     .word 0             # illegal -- must never run
 * msg: "RING-U-OK\n"
 *
 * Encodings hand-checked against the I-type/U-type formats; the V0
 * lesson (the i386 pad-byte incident's sibling) says the smoke test's
 * exact-string assert is the real reviewer.
 */
static const uint8_t prog_ok[] = {
    0x93, 0x08, 0x10, 0x00,   /* addi a7, x0, 1    */
    0x13, 0x05, 0x10, 0x00,   /* addi a0, x0, 1    */
    0xb7, 0x05, 0x00, 0x40,   /* lui  a1, 0x40000  */
    0x93, 0x85, 0x85, 0x04,   /* addi a1, a1, 0x48 */
    0x13, 0x06, 0xa0, 0x00,   /* addi a2, x0, 10   */
    0x73, 0x00, 0x00, 0x00,   /* ecall             */
    0x93, 0x08, 0x70, 0x02,   /* addi a7, x0, 39   */
    0x73, 0x00, 0x00, 0x00,   /* ecall             */
    0x93, 0x08, 0xc0, 0x03,   /* addi a7, x0, 60   */
    0x13, 0x05, 0xa0, 0x02,   /* addi a0, x0, 42   */
    0x73, 0x00, 0x00, 0x00,   /* ecall             */
    0x00, 0x00, 0x00, 0x00,   /* unimp (never runs) */
    0x00, 0x00, 0x00, 0x00,   /* pad: msg at +0x48 */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,   /* <- the word the first cut lacked: the
                               * console printed "-U-OK" + 4 NULs, msg
                               * had landed at +0x44.  The i386 port's
                               * pad-byte incident ("ING3-OK"), replayed
                               * verbatim at the third width -- and
                               * caught the same way, by the exact-
                               * string assert.  Hand-assembled bytes
                               * get reviewed by tests, not by eyes. */
    'R', 'I', 'N', 'G', '-', 'U', '-', 'O', 'K', '\n'
};

/* The negative control: csrr from an S-mode CSR.  In U-mode every
 * CSR above the U-level range is illegal-instruction -- the hlt
 * probe's sibling (scause=2 where i386 got #GP=13). */
static const uint8_t prog_priv[] = {
    0x73, 0x25, 0x00, 0x10,   /* csrr a0, sscratch (0x100 range: S) */
    0x93, 0x08, 0xc0, 0x03,   /* addi a7, x0, 60   */
    0x73, 0x00, 0x00, 0x00,   /* ecall (never reached) */
};

int user_rv_selftest(void)
{
    sbi_puts("[user] self-test: entering U-mode...\n");

    int code = user_rv_run_image(prog_ok, sizeof(prog_ok));
    if (code != 42) {
        sbi_puts("[user] FAIL: expected exit 42, got ");
        put_udec_((uint64_t)code);
        sbi_puts("\n");
        return -1;
    }
    sbi_puts("[user] exit code 42 round-tripped\n");

    sbi_puts("[user] negative control: privileged csrr from U-mode...\n");
    code = user_rv_run_image(prog_priv, sizeof(prog_priv));
    if (code != 128 + 2) {
        sbi_puts("[user] FAIL: expected 130 (128+Illegal Instruction), got ");
        put_udec_((uint64_t)code);
        sbi_puts("\n");
        return -1;
    }
    sbi_puts("[user] PASS: privileged op contained (code 130), "
             "kernel intact\n");
    return 0;
}
