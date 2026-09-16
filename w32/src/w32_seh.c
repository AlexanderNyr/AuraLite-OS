/* w32/src/w32_seh.c — W32APP_PLAN.md phase W32A-4.
 *
 * Table-driven Win64 structured exception handling: RtlVirtualUnwind and
 * family over the image's own .pdata/.xdata, __C_specific_handler with its
 * scope tables, RaiseException, the unhandled filter, and the named C++
 * terminations.  This file REPLACES the W32-6 shim (the longjmp stack in
 * w32_crt.c): faults are now dispatched through real unwind tables, and the
 * shim's push/pop/shadow-stack API is deleted, not kept beside this.
 *
 * Sharing (the W32-3 D2 pattern): the parser, the unwinder, the dispatcher
 * core, and __C_specific_handler are pure code over caller-supplied bytes
 * and run identically on host and guest.  The host unit test #includes this
 * file (like test_w32_pe.c does w32_pe.c) and drives dispatch over
 * synthetic images; only the fault entry (signals), the resume primitive
 * (asm context restore), and the death paths (box / re-raise) are guest.
 *
 * Control flow, because it is the whole file:
 *   - Entries (signal fault, RaiseException) capture a CONTEXT, then
 *     setjmp the per-thread dispatch frame and run the first pass.
 *   - The first pass walks frames with RtlVirtualUnwind and calls each
 *     frame's personality.  A personality that handles the exception calls
 *     RtlUnwind(target), which runs cleanups frame by frame and then
 *     longjmps the dispatch frame with the resume CONTEXT filled in.
 *   - The entry then transfers control: the signal path mutates the fault
 *     ucontext and returns (sigreturn restores the mask, so the second
 *     fault is still caught); the RaiseException path restores the CONTEXT
 *     with w32_seh_do_resume (raw asm, no libc context API exists).
 *   - Bottom of the walk with no handler: the unhandled path (top filter,
 *     serial dump, GUI box or console death, C++ named termination).
 *   - A nested fault during dispatch overwrites the single per-thread
 *     frame -- the outer dispatch is abandoned, as on Windows.  A nested
 *     fault during the SECOND pass is a collided unwind and terminates.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/w32_abi.h"
#include "w32/w32_seh.h"

#ifdef AURALITE_W32_HOST_TEST
#include <setjmp.h>
#include <signal.h>
#else
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include "w32/w32_module.h"
#endif

/* Exception codes stay declared in w32_crt.h (fixtures include it); the
 * shim machinery that used to live beside them is gone. */
#include "w32/w32_crt.h"

/* ---- tuning -------------------------------------------------------------- */

#define W32_SEH_CHAIN_MAX   8u      /* chained/indirect unwind-info hops */
#define W32_SEH_NEST_MAX    4u      /* nested dispatches before refusal */
#define W32_SEH_SCOPE_MAX   4096u   /* scope-table records (hostile cap) */
#define W32_SEH_WALK_MAX    ((uint64_t)1 << 21) /* frames per pass (2M) */

/* Frame state bits (struct w32_seh_dispatch.state). */
#define W32_SEH_ST_ACTIVE   0x01u   /* a dispatch is using this frame */
#define W32_SEH_ST_UNWIND   0x02u   /* second pass in progress */
#define W32_SEH_ST_CXX      0x04u   /* a _CxxThrowException unwind is active */
#define W32_SEH_ST_RAISE    0x08u   /* entered via RaiseException (software) */

/* MSVC C++ exception code (STATUS_MSCXX): the D7 shape. */
#define W32_CODE_MSCXX      0xE06D7363u

/* ---- the per-thread dispatch frame --------------------------------------- */

/* Birth-allocated (never in a signal handler: malloc there could deadlock
 * against a fault that struck inside the allocator), freed at thread death.
 * The heap thread object holds the pointer (NOT the mmap'd TEB: LSan-blind
 * memory would hide it); thr.c mirrors the tls_blocks sites. */
struct w32_seh_dispatch {
    /* Resume conduit back to the entry.  PLAIN setjmp/longjmp (NOT the sig
     * variants): sigsetjmp is a wrapper that CALLs setjmp, so the saved
     * rsp points into sigsetjmp's dead frame below the entry's rsp -- and
     * every direct call the entry makes afterwards (seh_first_pass,
     * RtlUnwindEx) pushes its return onto that dead frame, clobbering the
     * slots siglongjmp's landing depends on (the `ret` then resumes at
     * the last call's return, not the back path: try.exe survived this by
     * layout luck, cxthrow died in it with a fault in fprintf).  Plain
     * setjmp saves the entry's own rsp/rip (alive, inviolate: callees
     * only push below it), so the resume is layout-proof.  The signal
     * mask rides alongside explicitly (saved before setjmp, restored
     * before longjmp), preserving the old savemask=1 semantics. */
    jmp_buf jb;
    sigset_t jb_mask;           /* entry-time signal mask (see above) */
#ifdef AURALITE_W32_HOST_TEST
    sigjmp_buf harness_jb;      /* host test's catch point (owns resume) */
    int harness_set;
#endif
    w32_context_t resume;       /* filled by RtlUnwind / ContinueExecution */
    int resume_valid;
    /* RtlUnwind's cursor.  Set to the FAULT context at entry; the first
     * pass never touches it.  RtlUnwind starts the second pass here --
     * from the fault, not from the personality's frame -- so cleanups
     * in frames between the fault and the handler still run (starting
     * at the personality would skip them: the cursor would already sit
     * past the inner frames).  live_valid doubles as "dispatch active"
     * (RtlUnwind outside a dispatch refuses). */
    w32_context_t live;
    int live_valid;
    w32_exception_record_t records[W32_SEH_NEST_MAX];
    unsigned depth;             /* active (nested) dispatches */
    unsigned state;
};

void *w32_seh_frame_new(void) {
    return calloc(1, sizeof(struct w32_seh_dispatch));
}

void w32_seh_frame_free(void *frame) {
    free(frame);
}

#ifdef AURALITE_W32_HOST_TEST
/* ---- host module table ----------------------------------------------------
 * The guest walks the loader registry; the host test registers synthetic
 * images here.  Same query shape, no loader dependency.
 */
#define W32_SEH_HOST_IMAGES 8
static struct {
    uint64_t base;
    size_t span;
} seh_host_images[W32_SEH_HOST_IMAGES];
static int seh_host_nimages = 0;

void w32_seh_test_add_image(uint64_t base, size_t span) {
    if (seh_host_nimages < W32_SEH_HOST_IMAGES) {
        seh_host_images[seh_host_nimages].base = base;
        seh_host_images[seh_host_nimages].span = span;
        seh_host_nimages++;
    }
}

void w32_seh_test_reset(void) {
    seh_host_nimages = 0;
    memset(seh_host_images, 0, sizeof(seh_host_images));
}

/* The host harness's current frame (RaiseException/RtlUnwind find it
 * here; the guest uses the TEB). */
static struct w32_seh_dispatch *seh_host_frame = NULL;

void w32_seh_test_set_frame(void *frame) {
    seh_host_frame = (struct w32_seh_dispatch *)frame;
}
#endif /* AURALITE_W32_HOST_TEST */

/* ---- image queries -------------------------------------------------------- */

/* Resolve pc -> (base, span).  Guest: the loader registry (exe included,
 * registered by w32run).  Host: the test table above. */
static int seh_image_for_pc(uint64_t pc, uint64_t *base, size_t *span) {
#ifdef AURALITE_W32_HOST_TEST
    for (int i = 0; i < seh_host_nimages; i++) {
        if (pc >= seh_host_images[i].base &&
            pc - seh_host_images[i].base < seh_host_images[i].span) {
            *base = seh_host_images[i].base;
            *span = seh_host_images[i].span;
            return 0;
        }
    }
    return -1;
#else
    {
        uint8_t *b = NULL;
        size_t s = 0;
        if (w32_module_find_by_address((const void *)(uintptr_t)pc, &b, &s) != 0)
            return -1;
        *base = (uint64_t)(uintptr_t)b;
        *span = s;
        return 0;
    }
#endif
}

/* Resolve a known base -> span (RtlVirtualUnwind gets the base, needs the
 * span to validate chains). */
static int seh_span_for_base(uint64_t base, size_t *span) {
#ifdef AURALITE_W32_HOST_TEST
    for (int i = 0; i < seh_host_nimages; i++) {
        if (seh_host_images[i].base == base) {
            *span = seh_host_images[i].span;
            return 0;
        }
    }
    return -1;
#else
    {
        uint8_t *b = NULL;
        size_t s = 0;
        if (w32_module_find_by_address((const void *)(uintptr_t)base, &b,
                                       &s) != 0)
            return -1;
        if ((uint64_t)(uintptr_t)b != base)
            return -1;
        *span = s;
        return 0;
    }
#endif
}

/* Locate .pdata through the mapped headers.  The headers were validated at
 * load, but the unwinder re-checks the fields it reads: a corrupt in-memory
 * image must refuse, not wild-read.  Returns 0 and fills the outs,
 * else -1. */
static int seh_pdata_of(uint64_t base, size_t span, uint32_t *rva,
                        size_t *size) {
    /* DOS + PE signature + COFF + optional magic + dir[3].  All reads are
     * bounds-checked against span; anything short refuses. */
    if (span < 0x40)
        return -1;
    const uint8_t *img = (const uint8_t *)(uintptr_t)base;
    if (img[0] != 'M' || img[1] != 'Z')
        return -1;
    uint32_t pe = (uint32_t)img[0x3c] | ((uint32_t)img[0x3d] << 8) |
                  ((uint32_t)img[0x3e] << 16) | ((uint32_t)img[0x3f] << 24);
    if (pe > span || span - pe < 6 + 20 + 2)
        return -1;
    const uint8_t *p = img + pe;
    if (p[0] != 'P' || p[1] != 'E' || p[2] != 0 || p[3] != 0)
        return -1;
    uint16_t opt_size = (uint16_t)(p[20] | (p[21] << 8));
    const uint8_t *opt = p + 24;
    if (opt[0] != 0x0b || opt[1] != 0x02)   /* PE32+ only */
        return -1;
    /* PE32+ data directories follow the 112-byte fixed part (24-byte
     * standard fields + 88-byte Windows fields); the exception
     * directory is entry 3.  (96 is the PE32/32-bit figure -- using it
     * here reads the import directory as .pdata and refuses every
     * real image, which the first QEMU run demonstrated.) */
    if (opt_size < 112 + 3 * 8 + 8)
        return -1;
    if ((size_t)(opt - img) > span ||
        span - (size_t)(opt - img) < 112 + 3 * 8 + 8)
        return -1;
    const uint8_t *d = opt + 112 + 3 * 8;
    uint32_t drva = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                    ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    uint32_t dsize = (uint32_t)d[4] | ((uint32_t)d[5] << 8) |
                     ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    if (dsize == 0)
        return -1;
    if (drva >= span || dsize > span - drva)
        return -1;
    if (dsize % sizeof(w32_runtime_function_t) != 0)
        return -1;
    *rva = drva;
    *size = dsize;
    return 0;
}

/* ---- the parser ----------------------------------------------------------- */

const w32_runtime_function_t *w32_seh_lookup(const uint8_t *pdata,
                                             size_t pdata_bytes,
                                             uint32_t pc_rva) {
    size_t n;
    size_t lo, hi;
    if (!pdata || pdata_bytes % sizeof(w32_runtime_function_t) != 0)
        return NULL;
    n = pdata_bytes / sizeof(w32_runtime_function_t);
    if (n == 0)
        return NULL;
    /* .pdata is sorted by BeginAddress (the linker guarantees it); binary
     * search for the last entry with Begin <= pc, then check End. */
    lo = 0;
    hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const w32_runtime_function_t *e =
            (const w32_runtime_function_t *)(pdata +
                                             mid * sizeof(*e));
        if (e->begin_address <= pc_rva)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    {
        const w32_runtime_function_t *e =
            (const w32_runtime_function_t *)(pdata +
                                             (lo - 1) * sizeof(*e));
        if (pc_rva < e->end_address)
            return e;
        return NULL;
    }
}

/* Bounds-check one absolute read of @n bytes at image RVA @rva. */
static const uint8_t *seh_at(const uint8_t *image, size_t span, uint32_t rva,
                             size_t n) {
    if (rva >= span || n > span - rva)
        return NULL;
    return image + rva;
}

/* Validate the UNWIND_INFO chain at @unwind_rva: version, code counts,
 * chained/indirect recursion (capped), handler RVAs inside the image.
 * 0 = well-formed, -1 = hostile.  RtlVirtualUnwind refuses chains that
 * fail here (treats the frame as unknown: stop, not wild-read). */
int w32_seh_validate_chain(const uint8_t *image, size_t image_span,
                           uint32_t unwind_rva) {
    uint32_t rva = unwind_rva;
    unsigned hops;
    if (!image || image_span == 0)
        return -1;
    for (hops = 0; hops < W32_SEH_CHAIN_MAX; hops++) {
        const uint8_t *p;
        unsigned ver, flags, count;
        size_t hdr;
        if (rva & W32_RUNTIME_FUNCTION_INDIRECT) {
            /* Indirect: points at another RUNTIME_FUNCTION. */
            const w32_runtime_function_t *e;
            rva &= ~W32_RUNTIME_FUNCTION_INDIRECT;
            p = seh_at(image, image_span, rva, sizeof(*e));
            if (!p)
                return -1;
            e = (const w32_runtime_function_t *)p;
            if (e->begin_address >= e->end_address)
                return -1;
            rva = e->unwind_data;
            continue;
        }
        p = seh_at(image, image_span, rva, sizeof(w32_unwind_info_t));
        if (!p)
            return -1;
        ver = p[0] & W32_UNW_VERSION_MASK;
        flags = p[0] & ~W32_UNW_VERSION_MASK;
        if (ver != W32_UNW_VERSION_1)
            return -1;      /* v2 (epilog ops): refused, never misread */
        count = p[2];
        /* count==0 is legal (no frame established); >255 is impossible
         * for a byte, and the comparison documents the bound. */
        if (count > 255)
            return -1;
        /* Codes occupy ceil(count/2)*2 slots of 2 bytes. */
        hdr = sizeof(w32_unwind_info_t) + ((size_t)((count + 1) & ~1u)) * 2;
        if (seh_at(image, image_span, rva, hdr) == NULL)
            return -1;
        /* The codes themselves (count==0 means no frame: nothing to walk);
         * EPILOG/SPARE and the multi-slot ops are range-checked by the
         * unwinder per code (it walks with the same bounds).  Here, check
         * the tail. */
        if (flags & W32_UNW_FLAG_CHAININFO) {
            /* Tail is a chained RUNTIME_FUNCTION triple. */
            const w32_runtime_function_t *e;
            p = seh_at(image, image_span, rva,
                       hdr + sizeof(w32_runtime_function_t));
            if (!p)
                return -1;
            e = (const w32_runtime_function_t *)(p + hdr);
            if (e->begin_address >= e->end_address)
                return -1;
            rva = e->unwind_data;
            continue;
        }
        if (flags & (W32_UNW_FLAG_EHANDLER | W32_UNW_FLAG_UHANDLER)) {
            /* Tail is handler RVA + handler data.  The handler must be a
             * plausible code RVA; the data is the personality's business,
             * but __C_specific_handler validates its scope table. */
            const uint8_t *h;
            uint32_t hrva;
            p = seh_at(image, image_span, rva, hdr + 4);
            if (!p)
                return -1;
            hrva = (uint32_t)p[hdr] | ((uint32_t)p[hdr + 1] << 8) |
                   ((uint32_t)p[hdr + 2] << 16) |
                   ((uint32_t)p[hdr + 3] << 24);
            if (hrva == 0 || hrva >= image_span)
                return -1;
            (void)h;
        }
        return 0;
    }
    return -1;
}

/* ---- RtlCaptureContext (pure asm: no prologue may run first) --------------
 *
 * On entry RSP = caller_RSP - 8 and (RSP) = return RIP.  Every register
 * still holds the CALLER's value, which is exactly what the CONTEXT must
 * capture -- so this function is raw asm start to end (the thr_child_entry
 * precedent).  MS ABI: RCX = w32_context_t*.
 */
__asm__(
".globl RtlCaptureContext\n"
"RtlCaptureContext:\n"
"    push %rax\n"                 /* caller's rax (rep stosq eats eax) */
"    push %rcx\n"                 /* ctx */
"    push %r11\n"
"    push %rdi\n"
"    mov %rcx, %rdi\n"
"    xor %eax, %eax\n"
"    mov $154, %ecx\n"            /* 154 qwords = 1232 bytes */
"    rep stosq\n"
"    pop %rdi\n"                  /* caller's rdi */
"    pop %r11\n"                  /* caller's r11 */
"    pop %rcx\n"                  /* ctx */
"    pop %rax\n"                  /* caller's rax */
"    movl $0x10001f, 0x30(%rcx)\n" /* ContextFlags (see note) */
"    mov %rax, 0x78(%rcx)\n"      /* caller's rax */
"    mov %rcx, %rax\n"            /* scratch = ctx */
"    mov %rax, 0x80(%rax)\n"      /* caller's rcx IS ctx (the argument) */
"    mov %rdi, 0xb0(%rax)\n"
"    mov %rsi, 0xa8(%rax)\n"
"    mov %rbp, 0xa0(%rax)\n"
"    mov %rbx, 0x90(%rax)\n"
"    mov %rdx, 0x88(%rax)\n"
"    mov %r11, 0xd0(%rax)\n"
"    mov %r10, 0xc8(%rax)\n"
"    mov %r9, 0xc0(%rax)\n"
"    mov %r8, 0xb8(%rax)\n"
"    mov %r12, 0xd8(%rax)\n"
"    mov %r13, 0xe0(%rax)\n"
"    mov %r14, 0xe8(%rax)\n"
"    mov %r15, 0xf0(%rax)\n"
"    mov (%rsp), %r11\n"
"    mov %r11, 0xf8(%rax)\n"      /* rip = return address */
"    lea 8(%rsp), %r11\n"
"    mov %r11, 0x98(%rax)\n"      /* rsp = caller's rsp */
"    pushfq\n"
"    pop %r11\n"
"    mov %r11d, 0x44(%rax)\n"     /* eflags */
"    mov %cs, %r11w\n"
"    mov %r11w, 0x38(%rax)\n"     /* seg_cs */
"    mov %ds, %r11w\n"
"    mov %r11w, 0x3a(%rax)\n"
"    mov %es, %r11w\n"
"    mov %r11w, 0x3c(%rax)\n"
"    mov %fs, %r11w\n"
"    mov %r11w, 0x3e(%rax)\n"
"    mov %gs, %r11w\n"
"    mov %r11w, 0x40(%rax)\n"
"    mov %ss, %r11w\n"
"    mov %r11w, 0x42(%rax)\n"
"    stmxcsr 0x34(%rax)\n"        /* mx_csr */
"    movdqu %xmm0, 0x1a0(%rax)\n"
"    movdqu %xmm1, 0x1b0(%rax)\n"
"    movdqu %xmm2, 0x1c0(%rax)\n"
"    movdqu %xmm3, 0x1d0(%rax)\n"
"    movdqu %xmm4, 0x1e0(%rax)\n"
"    movdqu %xmm5, 0x1f0(%rax)\n"
"    movdqu %xmm6, 0x200(%rax)\n"
"    movdqu %xmm7, 0x210(%rax)\n"
"    movdqu %xmm8, 0x220(%rax)\n"
"    movdqu %xmm9, 0x230(%rax)\n"
"    movdqu %xmm10, 0x240(%rax)\n"
"    movdqu %xmm11, 0x250(%rax)\n"
"    movdqu %xmm12, 0x260(%rax)\n"
"    movdqu %xmm13, 0x270(%rax)\n"
"    movdqu %xmm14, 0x280(%rax)\n"
"    movdqu %xmm15, 0x290(%rax)\n"
"    mov %rax, %r11\n"            /* restore scratch to caller's r11 below */
"    mov 0xd0(%rax), %r11\n"      /* (already stored; reload for return) */
"    mov 0x78(%rax), %rax\n"      /* caller's rax back (we stored 0!) */
"    ret\n"
);
/* ---- RaiseException/_CxxThrowException trampolines -------------------------
 * The C cores dispatch from the CALLER's context (the guest frame that
 * raised), not from inside w32run: these host frames carry no .pdata, so a
 * walk starting here ends before it begins.  Raw asm start to end -- no
 * prologue may run before the capture (RtlCaptureContext's discipline).
 *
 * Fixed frame (rsp is constant after the sub; rsp%16==8 at the C call):
 *   [rsp+0x00,0x20)  32-byte shadow (doubles as volatile stash pre-call)
 *   [rsp+0x20,0x28)  5th-arg slot (RaiseException_c only)
 *   [rsp+0x28,0x38)  pad
 *   [rsp+0x38,+0x4D0)  the 1232-byte w32_context_t
 */
__asm__(
".macro SEH_TRAMP_CAPTURE\n"
"    sub $0x508, %rsp\n"
"    mov %rax, 0x00(%rsp)\n"          /* stash volatile pre-zeroing */
"    mov %rcx, 0x08(%rsp)\n"
"    mov %rdi, 0x10(%rsp)\n"
"    lea 0x38(%rsp), %rax\n"          /* rax = ctx */
"    mov %r11, 0xd0(%rax)\n"          /* caller's r11 (scratch below) */
"    mov %rax, %rdi\n"
"    xor %eax, %eax\n"
"    mov $154, %ecx\n"
"    rep stosq\n"                     /* zero the whole CONTEXT */
"    lea 0x38(%rsp), %rax\n"         /* rax = ctx again */
"    mov 0x00(%rsp), %r11\n"
"    mov %r11, 0x78(%rax)\n"          /* caller's rax */
"    mov 0x08(%rsp), %r11\n"
"    mov %r11, 0x80(%rax)\n"          /* caller's rcx */
"    mov 0x10(%rsp), %r11\n"
"    mov %r11, 0xb0(%rax)\n"          /* caller's rdi */
"    mov %rdx, 0x88(%rax)\n"
"    mov %rbx, 0x90(%rax)\n"
"    lea 0x510(%rsp), %r11\n"
"    mov %r11, 0x98(%rax)\n"          /* rsp = entry rsp + 8 */
"    mov %rbp, 0xa0(%rax)\n"
"    mov %rsi, 0xa8(%rax)\n"
"    mov %r8, 0xb8(%rax)\n"
"    mov %r9, 0xc0(%rax)\n"
"    mov %r10, 0xc8(%rax)\n"
"    mov %r12, 0xd8(%rax)\n"
"    mov %r13, 0xe0(%rax)\n"
"    mov %r14, 0xe8(%rax)\n"
"    mov %r15, 0xf0(%rax)\n"
"    mov 0x508(%rsp), %r11\n"
"    mov %r11, 0xf8(%rax)\n"          /* rip = entry return address */
"    pushfq\n"
"    pop %r11\n"
"    mov %r11d, 0x44(%rax)\n"         /* eflags */
"    movl $0x10001f, 0x30(%rax)\n"    /* ContextFlags (as in capture) */
"    mov %cs, %r11w\n"
"    mov %r11w, 0x38(%rax)\n"
"    mov %ds, %r11w\n"
"    mov %r11w, 0x3a(%rax)\n"
"    mov %es, %r11w\n"
"    mov %r11w, 0x3c(%rax)\n"
"    mov %fs, %r11w\n"
"    mov %r11w, 0x3e(%rax)\n"
"    mov %gs, %r11w\n"
"    mov %r11w, 0x40(%rax)\n"
"    mov %ss, %r11w\n"
"    mov %r11w, 0x42(%rax)\n"
"    stmxcsr 0x34(%rax)\n"
"    movdqu %xmm0, 0x1a0(%rax)\n"
"    movdqu %xmm1, 0x1b0(%rax)\n"
"    movdqu %xmm2, 0x1c0(%rax)\n"
"    movdqu %xmm3, 0x1d0(%rax)\n"
"    movdqu %xmm4, 0x1e0(%rax)\n"
"    movdqu %xmm5, 0x1f0(%rax)\n"
"    movdqu %xmm6, 0x200(%rax)\n"
"    movdqu %xmm7, 0x210(%rax)\n"
"    movdqu %xmm8, 0x220(%rax)\n"
"    movdqu %xmm9, 0x230(%rax)\n"
"    movdqu %xmm10, 0x240(%rax)\n"
"    movdqu %xmm11, 0x250(%rax)\n"
"    movdqu %xmm12, 0x260(%rax)\n"
"    movdqu %xmm13, 0x270(%rax)\n"
"    movdqu %xmm14, 0x280(%rax)\n"
"    movdqu %xmm15, 0x290(%rax)\n"
".endm\n"
".globl RaiseException\n"
"RaiseException:\n"
"    SEH_TRAMP_CAPTURE\n"
"    mov 0x80(%rax), %rcx\n"          /* arg1 = code */
"    mov 0x88(%rax), %rdx\n"          /* arg2 = flags */
"    mov 0xb8(%rax), %r8\n"           /* arg3 = num */
"    mov 0xc0(%rax), %r9\n"           /* arg4 = args */
"    mov %rax, 0x20(%rsp)\n"          /* arg5 = ctx */
"    call RaiseException_c\n"
#ifdef AURALITE_W32_HOST_TEST
"    add $0x508, %rsp\n"    /* release the CONTEXT frame */
"    ret\n"
#else
"    ud2\n"
#endif
".globl _CxxThrowException\n"
"_CxxThrowException:\n"
"    SEH_TRAMP_CAPTURE\n"
"    mov 0x80(%rax), %rcx\n"          /* arg1 = obj */
"    mov 0x88(%rax), %rdx\n"          /* arg2 = throw_info */
"    mov %rax, %r8\n"                 /* arg3 = ctx */
"    call _CxxThrowException_c\n"
#ifdef AURALITE_W32_HOST_TEST
"    add $0x508, %rsp\n"    /* release the CONTEXT frame */
"    ret\n"
#else
"    ud2\n"
#endif
);
/* ContextFlags note: 0x10001f = AMD64 | CONTROL | INTEGER | SEGMENTS |
 * FLOATING_POINT.  Debug registers are not captured (no user access).
 * RAX note: rep stosq zeroes EAX, so the caller's RAX is pushed first
 * and popped into the store -- the trap this comment exists to record. */

/* The asm above and below hardcodes CONTEXT offsets: pin them. */
_Static_assert(offsetof(w32_context_t, context_flags) == 0x30, "ctx flags");
_Static_assert(offsetof(w32_context_t, mx_csr) == 0x34, "ctx mxcsr");
_Static_assert(offsetof(w32_context_t, seg_cs) == 0x38, "ctx cs");
_Static_assert(offsetof(w32_context_t, seg_ss) == 0x42, "ctx ss");
_Static_assert(offsetof(w32_context_t, eflags) == 0x44, "ctx eflags");
_Static_assert(offsetof(w32_context_t, rax) == 0x78, "ctx rax");
_Static_assert(offsetof(w32_context_t, rsp) == 0x98, "ctx rsp");
_Static_assert(offsetof(w32_context_t, rdi) == 0xb0, "ctx rdi");
_Static_assert(offsetof(w32_context_t, r15) == 0xf0, "ctx r15");
_Static_assert(offsetof(w32_context_t, rip) == 0xf8, "ctx rip");
_Static_assert(offsetof(w32_context_t, xmm) == 0x1a0, "ctx xmm");
_Static_assert(sizeof(w32_context_t) == 1232, "ctx size");
_Static_assert(sizeof(w32_context_t) == 1232, "ctx size");

/* ---- the resume primitive ------------------------------------------------- */

/* Restore a full CONTEXT and jump to it.  SysV internal (RDI = ctx).
 * No libc context API exists (and setcontext would be wrong anyway: the
 * fault path must return through sigreturn for the mask).  movdqu
 * everywhere: the caller's CONTEXT alignment is not guaranteed.
 * Clobbers the red zone below the target RSP by 8 bytes -- that stack is
 * the abandoned frame's, dead by definition. */
#ifndef AURALITE_W32_HOST_TEST
__asm__(
".globl w32_seh_do_resume\n"
"w32_seh_do_resume:\n"
"    mov 0x44(%rdi), %eax\n"
"    push %rax\n"
"    popfq\n"
"    mov 0x80(%rdi), %rcx\n"
"    mov 0x88(%rdi), %rdx\n"
"    mov 0x90(%rdi), %rbx\n"
"    mov 0xa0(%rdi), %rbp\n"
"    mov 0xa8(%rdi), %rsi\n"
"    mov 0xb8(%rdi), %r8\n"
"    mov 0xc0(%rdi), %r9\n"
"    mov 0xc8(%rdi), %r10\n"
"    mov 0xd8(%rdi), %r12\n"
"    mov 0xe0(%rdi), %r13\n"
"    mov 0xe8(%rdi), %r14\n"
"    mov 0xf0(%rdi), %r15\n"
"    movdqu 0x1a0(%rdi), %xmm0\n"
"    movdqu 0x1b0(%rdi), %xmm1\n"
"    movdqu 0x1c0(%rdi), %xmm2\n"
"    movdqu 0x1d0(%rdi), %xmm3\n"
"    movdqu 0x1e0(%rdi), %xmm4\n"
"    movdqu 0x1f0(%rdi), %xmm5\n"
"    movdqu 0x200(%rdi), %xmm6\n"
"    movdqu 0x210(%rdi), %xmm7\n"
"    movdqu 0x220(%rdi), %xmm8\n"
"    movdqu 0x230(%rdi), %xmm9\n"
"    movdqu 0x240(%rdi), %xmm10\n"
"    movdqu 0x250(%rdi), %xmm11\n"
"    movdqu 0x260(%rdi), %xmm12\n"
"    movdqu 0x270(%rdi), %xmm13\n"
"    movdqu 0x280(%rdi), %xmm14\n"
"    movdqu 0x290(%rdi), %xmm15\n"
"    mov 0x34(%rdi), %eax\n"
"    and $0xffbf, %eax\n"         /* mask MXCSR reserved bits (fault-safe) */
"    push %rax\n"                 /* ldmxcsr needs a memory operand */
"    ldmxcsr (%rsp)\n"
"    pop %rax\n"
"    mov 0x98(%rdi), %r11\n"      /* r11 = target rsp (scratch) */
"    mov 0xf8(%rdi), %rax\n"      /* rax = target rip */
"    mov %rax, -8(%r11)\n"        /* [t_rsp-8] = t_rip */
"    push %r11\n"                 /* t_rsp */
"    mov 0x78(%rdi), %rax\n"
"    push %rax\n"                 /* t_rax */
"    mov 0xb0(%rdi), %rax\n"
"    push %rax\n"                 /* t_rdi */
"    mov 0xd0(%rdi), %rax\n"
"    push %rax\n"                 /* t_r11 (top) */
"    pop %r11\n"
"    pop %rdi\n"
"    pop %rax\n"
"    mov (%rsp), %rsp\n"          /* switch (abandons this stack) */
"    sub $8, %rsp\n"
"    ret\n"
);
#endif

/* ---- RtlVirtualUnwind ----------------------------------------------------- */

/* Read the frame register's current value by number. */
static uint64_t seh_frame_reg(const w32_context_t *ctx, unsigned reg) {
    switch (reg) {
    case 0: return ctx->rax;
    case 1: return ctx->rcx;
    case 2: return ctx->rdx;
    case 3: return ctx->rbx;
    case 4: return ctx->rsp;
    case 5: return ctx->rbp;
    case 6: return ctx->rsi;
    case 7: return ctx->rdi;
    case 8: return ctx->r8;
    case 9: return ctx->r9;
    case 10: return ctx->r10;
    case 11: return ctx->r11;
    case 12: return ctx->r12;
    case 13: return ctx->r13;
    case 14: return ctx->r14;
    case 15: return ctx->r15;
    default: return ctx->rsp;
    }
}

/* Apply one frame's unwind codes to @ctx.  @image/@span bound every read;
 * @codes/@count is the code array (already validated); @prolog_off is
 * ControlPc - BeginAddress (codes past it are skipped: the fault struck
 * mid-prolog).  @fpreg/@fpoff serve SET_FPREG (from the UNWIND_INFO
 * header).  Sets *saw_machframe when a PUSH_MACHFRAME consumed the stack
 * (RIP/RSP final, no return address to pop).  Returns 0, or -1 when a code
 * is hostile (unknown op, overrun read). */
static int seh_apply_codes(w32_context_t *ctx, const uint8_t *image,
                           size_t span, const w32_unwind_code_t *codes,
                           unsigned count, uint32_t prolog_off,
                           unsigned fpreg, unsigned fpoff,
                           int *saw_machframe) {
    unsigned i = 0;
    (void)image;
    (void)span;
    while (i < count) {
        uint8_t off = codes[i].code_offset;
        uint8_t op = codes[i].op_info_op & 0x0f;
        uint8_t info = codes[i].op_info_op >> 4;
        i++;
        if (off > prolog_off)
            continue;   /* not yet executed at the fault pc */
        switch (op) {
        case W32_UWOP_PUSH_NONVOL: {
            /* OpInfo = register; pop it. */
            uint64_t v;
            uint64_t *dst;
            if (info > 15)
                return -1;
            v = *(volatile uint64_t *)(uintptr_t)ctx->rsp;
            ctx->rsp += 8;
            switch (info) {
            case 0: dst = &ctx->rax; break;
            case 1: dst = &ctx->rcx; break;
            case 2: dst = &ctx->rdx; break;
            case 3: dst = &ctx->rbx; break;
            case 5: dst = &ctx->rbp; break;
            case 6: dst = &ctx->rsi; break;
            case 7: dst = &ctx->rdi; break;
            case 8: dst = &ctx->r8; break;
            case 9: dst = &ctx->r9; break;
            case 10: dst = &ctx->r10; break;
            case 11: dst = &ctx->r11; break;
            case 12: dst = &ctx->r12; break;
            case 13: dst = &ctx->r13; break;
            case 14: dst = &ctx->r14; break;
            case 15: dst = &ctx->r15; break;
            default: return -1;  /* RSP (4) is never pushed */
            }
            *dst = v;
            break;
        }
        case W32_UWOP_ALLOC_SMALL:
            ctx->rsp += (uint64_t)(info * 8 + 8);
            break;
        case W32_UWOP_ALLOC_LARGE:
            if (i >= count)
                return -1;
            if (info == 0) {
                uint16_t sz = (uint16_t)(codes[i].code_offset |
                                         (codes[i].op_info_op << 8));
                i++;
                ctx->rsp += (uint64_t)sz * 8;
            } else if (info == 1) {
                uint32_t sz;
                if (i + 1 >= count)
                    return -1;
                sz = (uint32_t)codes[i].code_offset |
                     ((uint32_t)codes[i].op_info_op << 8) |
                     ((uint32_t)codes[i + 1].code_offset << 16) |
                     ((uint32_t)codes[i + 1].op_info_op << 24);
                i += 2;
                ctx->rsp += sz;
            } else {
                return -1;
            }
            break;
        case W32_UWOP_SET_FPREG:
            /* RSP = frame_reg - scaled_offset, in sequence (it sits
             * mid-array between the alloc undo and the push pops). */
            if (fpreg == 0 || fpreg == 4 || fpreg > 15)
                return -1;
            ctx->rsp = seh_frame_reg(ctx, fpreg) - fpoff;
            break;
        case W32_UWOP_SAVE_NONVOL:
        case W32_UWOP_SAVE_XMM128: {
            uint64_t *dst;
            uint32_t disp;
            if (i >= count)
                return -1;
            disp = (uint32_t)(codes[i].code_offset |
                              (codes[i].op_info_op << 8));
            i++;
            if (op == W32_UWOP_SAVE_NONVOL) {
                if (info > 15)
                    return -1;
                disp *= 8;
                switch (info) {
                case 0: dst = &ctx->rax; break;
                case 1: dst = &ctx->rcx; break;
                case 2: dst = &ctx->rdx; break;
                case 3: dst = &ctx->rbx; break;
                case 5: dst = &ctx->rbp; break;
                case 6: dst = &ctx->rsi; break;
                case 7: dst = &ctx->rdi; break;
                case 8: dst = &ctx->r8; break;
                case 9: dst = &ctx->r9; break;
                case 10: dst = &ctx->r10; break;
                case 11: dst = &ctx->r11; break;
                case 12: dst = &ctx->r12; break;
                case 13: dst = &ctx->r13; break;
                case 14: dst = &ctx->r14; break;
                case 15: dst = &ctx->r15; break;
                default: return -1;
                }
                *dst = *(volatile uint64_t *)(uintptr_t)(ctx->rsp + disp);
            } else {
                if (info > 15)
                    return -1;
                disp *= 16;
                memcpy(&ctx->xmm[info],
                       (const void *)(uintptr_t)(ctx->rsp + disp), 16);
            }
            break;
        }
        case W32_UWOP_SAVE_NONVOL_FAR:
        case W32_UWOP_SAVE_XMM128_FAR: {
            uint64_t *dst;
            uint32_t disp;
            if (i + 1 >= count)
                return -1;
            disp = (uint32_t)codes[i].code_offset |
                   ((uint32_t)codes[i].op_info_op << 8) |
                   ((uint32_t)codes[i + 1].code_offset << 16) |
                   ((uint32_t)codes[i + 1].op_info_op << 24);
            i += 2;
            if (op == W32_UWOP_SAVE_NONVOL_FAR) {
                if (info > 15)
                    return -1;
                switch (info) {
                case 0: dst = &ctx->rax; break;
                case 1: dst = &ctx->rcx; break;
                case 2: dst = &ctx->rdx; break;
                case 3: dst = &ctx->rbx; break;
                case 5: dst = &ctx->rbp; break;
                case 6: dst = &ctx->rsi; break;
                case 7: dst = &ctx->rdi; break;
                case 8: dst = &ctx->r8; break;
                case 9: dst = &ctx->r9; break;
                case 10: dst = &ctx->r10; break;
                case 11: dst = &ctx->r11; break;
                case 12: dst = &ctx->r12; break;
                case 13: dst = &ctx->r13; break;
                case 14: dst = &ctx->r14; break;
                case 15: dst = &ctx->r15; break;
                default: return -1;
                }
                *dst = *(volatile uint64_t *)(uintptr_t)(ctx->rsp + disp);
            } else {
                if (info > 15)
                    return -1;
                memcpy(&ctx->xmm[info],
                       (const void *)(uintptr_t)(ctx->rsp + disp), 16);
            }
            break;
        }
        case W32_UWOP_PUSH_MACHFRAME: {
            /* Trap frame at RSP: [RIP,CS,EFLAGS,(ERR),RSP,SS]. */
            uint64_t *mf = (uint64_t *)(uintptr_t)ctx->rsp;
            uint64_t rip, rsp;
            if (info != 0 && info != 1)
                return -1;
            rip = *(volatile uint64_t *)&mf[0];
            if (info == 0) {
                ctx->eflags = (uint32_t)(*(volatile uint64_t *)&mf[2]);
                rsp = *(volatile uint64_t *)&mf[3];
            } else {
                ctx->eflags = (uint32_t)(*(volatile uint64_t *)&mf[3]);
                rsp = *(volatile uint64_t *)&mf[4];
            }
            ctx->rip = rip;
            ctx->rsp = rsp;
            *saw_machframe = 1;
            break;
        }
        case W32_UWOP_EPILOG:
        case W32_UWOP_SPARE:
        default:
            return -1;  /* v2 / never-valid / unknown: refuse */
        }
    }
    return 0;
}

/* Read the frame register's current value by number. */
void *W32ABI RtlVirtualUnwind(uint32_t handler_type,
                              uint64_t image_base,
                              uint64_t control_pc,
                              w32_runtime_function_t *function_entry,
                              w32_context_t *context,
                              void **handler_data,
                              uint64_t *establisher_frame,
                              void *context_pointers) {
    const uint8_t *image = (const uint8_t *)(uintptr_t)image_base;
    size_t span = 0;
    uint32_t begin, end, unwind_rva;
    uint32_t pc_rva;
    unsigned hops;
    void *language_handler = NULL;
    void *hdata = NULL;
    int saw_machframe = 0;
    uint64_t pre_rsp = context->rsp;

    (void)handler_type;     /* both passes fetch handlers identically */
    (void)context_pointers; /* nonvolatile-pointer tracking: not kept */

    if (!function_entry || !context || !establisher_frame)
        return NULL;
    if (handler_data)
        *handler_data = NULL;
    *establisher_frame = 0;
    if (seh_span_for_base(image_base, &span) != 0 || span == 0)
        return NULL;
    if (control_pc < image_base || control_pc - image_base >= span)
        return NULL;
    pc_rva = (uint32_t)(control_pc - image_base);

    begin = function_entry->begin_address;
    end = function_entry->end_address;
    unwind_rva = function_entry->unwind_data;
    if (begin >= end || pc_rva < begin || pc_rva >= end)
        return NULL;

    /* Chase indirect entries and chained info, applying each level's
     * codes.  Every hop re-validates; anything hostile stops the unwind
     * (NULL handler, context untouched past this point). */
    for (hops = 0; hops < W32_SEH_CHAIN_MAX; hops++) {
        const uint8_t *p;
        unsigned ver, flags, count, fpreg, fpoff;
        const w32_unwind_code_t *codes;
        size_t hdr;
        uint32_t prolog_off;

        pre_rsp = context->rsp;
        if (unwind_rva & W32_RUNTIME_FUNCTION_INDIRECT) {
            const w32_runtime_function_t *e;
            unwind_rva &= ~W32_RUNTIME_FUNCTION_INDIRECT;
            p = seh_at(image, span, unwind_rva, sizeof(*e));
            if (!p)
                return language_handler;
            e = (const w32_runtime_function_t *)p;
            begin = e->begin_address;
            end = e->end_address;
            unwind_rva = e->unwind_data;
            if (begin >= end || pc_rva < begin || pc_rva >= end)
                return language_handler;
            continue;
        }
        p = seh_at(image, span, unwind_rva, sizeof(w32_unwind_info_t));
        if (!p)
            return language_handler;
        ver = p[0] & W32_UNW_VERSION_MASK;
        flags = p[0] & ~W32_UNW_VERSION_MASK;
        if (ver != W32_UNW_VERSION_1)
            return language_handler;
        count = p[2];
        fpreg = p[3] & 0x0f;
        fpoff = (unsigned)(p[3] >> 4) * 16;
        /* count==0 flows through: no codes to apply, but the tail
         * (handler/chained) and the establisher+pop below still run. */
        hdr = sizeof(w32_unwind_info_t) + ((size_t)((count + 1) & ~1u)) * 2;
        p = seh_at(image, span, unwind_rva, hdr);
        if (!p)
            return language_handler;
        codes = (const w32_unwind_code_t *)(p + sizeof(w32_unwind_info_t));
        prolog_off = pc_rva - begin;

        if (seh_apply_codes(context, image, span, codes, count,
                            prolog_off, fpreg, fpoff,
                            &saw_machframe) != 0)
            return language_handler;

        if (flags & W32_UNW_FLAG_CHAININFO) {
            const w32_runtime_function_t *e;
            p = seh_at(image, span, unwind_rva,
                       hdr + sizeof(w32_runtime_function_t));
            if (!p)
                return language_handler;
            e = (const w32_runtime_function_t *)(p + hdr);
            begin = e->begin_address;
            end = e->end_address;
            unwind_rva = e->unwind_data;
            if (begin >= end || pc_rva < begin || pc_rva >= end)
                return language_handler;
            continue;
        }
        if (flags & (W32_UNW_FLAG_EHANDLER | W32_UNW_FLAG_UHANDLER)) {
            uint32_t hrva;
            p = seh_at(image, span, unwind_rva, hdr + 4);
            if (!p)
                return language_handler;
            hrva = (uint32_t)p[hdr] | ((uint32_t)p[hdr + 1] << 8) |
                   ((uint32_t)p[hdr + 2] << 16) |
                   ((uint32_t)p[hdr + 3] << 24);
            if (hrva == 0 || hrva >= span)
                return language_handler;
            language_handler = (void *)(uintptr_t)(image_base + hrva);
            hdata = (void *)(uintptr_t)(image_base + unwind_rva + hdr + 4);
        }
        break;
    }

    /* Establisher = RSP now (pointing at the return address), or the
     * pre-level RSP when a MACHFRAME consumed the stack (pointing at the
     * trap frame).  Then the normal epilogue pops the return address --
     * unless a MACHFRAME already set RIP/RSP final. */
    *establisher_frame = saw_machframe ? pre_rsp : context->rsp;
    if (!saw_machframe) {
        context->rip = *(volatile uint64_t *)(uintptr_t)context->rsp;
        context->rsp += 8;
    }
    if (handler_data)
        *handler_data = hdata;
    return language_handler;
}

/* ---- module-level queries ------------------------------------------------- */

w32_runtime_function_t *W32ABI RtlLookupFunctionEntry(uint64_t control_pc,
                                                      uint64_t *image_base,
                                                      void *history_table) {
    uint64_t base = 0;
    size_t span = 0;
    uint32_t pdata_rva = 0;
    size_t pdata_size = 0;
    const w32_runtime_function_t *e;

    (void)history_table;    /* a caching hint, not a correctness input */
    if (seh_image_for_pc(control_pc, &base, &span) != 0)
        return NULL;
    if (seh_pdata_of(base, span, &pdata_rva, &pdata_size) != 0)
        return NULL;        /* no .pdata: every frame is a leaf */
    e = w32_seh_lookup((const uint8_t *)(uintptr_t)(base + pdata_rva),
                       pdata_size, (uint32_t)(control_pc - base));
    if (!e)
        return NULL;
    if (image_base)
        *image_base = base;
    /* The entry lives in the mapped image (stable for the process). */
    return (w32_runtime_function_t *)e;
}

void *W32ABI RtlPcToFileHeader(void *pc, void **image_base) {
    uint64_t base = 0;
    size_t span = 0;
    if (seh_image_for_pc((uint64_t)(uintptr_t)pc, &base, &span) != 0)
        return NULL;
    if (image_base)
        *image_base = (void *)(uintptr_t)base;
    return (void *)(uintptr_t)base;
}

/* ---- the dispatch frame --------------------------------------------------- */

/* The current thread's dispatch frame: TEB on guest, harness-owned on host.
 * NULL before process/thread init (a fault there dies on its signal). */
static struct w32_seh_dispatch *seh_current_frame(void) {
#ifdef AURALITE_W32_HOST_TEST
    return seh_host_frame;
#else
    return (struct w32_seh_dispatch *)w32_thr_current_seh_frame();
#endif
}

/* ---- death ---------------------------------------------------------------- */

/* Named termination: every crash the unwinder owns prints one of the W32-*
 * messages (header-defined) and then dies loudly.  Greppable residue: a
 * crash shaped like a mystery prints none of these.  Guest: ExitProcess
 * for software deaths (no signal exists to raise).  Host: abort (a host
 * test reaching here has gone wrong; abort dumps core loudly). */
static void seh_die(const char *msg, uint32_t code, uint64_t pc) {
    fprintf(stderr, "%s code=0x%08x pc=0x%llx\n",
            msg, code, (unsigned long long)pc);
    fflush(stderr);
#ifdef AURALITE_W32_HOST_TEST
    abort();
#else
    /* ExitProcess is a fellow w32 export (same binary). */
    extern void W32ABI ExitProcess(unsigned int code);
    ExitProcess(code == 0 ? 99u : code);
    /* If ExitProcess ever returns (it must not), fall off loudly. */
    abort();
#endif
}

/* The process-wide unhandled filter (SetUnhandledExceptionFilter). */
static w32_top_filter_fn seh_top_filter = NULL;

w32_top_filter_fn W32ABI SetUnhandledExceptionFilter(w32_top_filter_fn filter) {
    w32_top_filter_fn prev = seh_top_filter;
    seh_top_filter = filter;
    return prev;
}

/* The export CRTs call: run the top filter, or CONTINUE_SEARCH when none
 * is set (the OS unhandled path -- seh_unhandled, not this function --
 * owns the dialog and the death). */
int32_t W32ABI UnhandledExceptionFilter(w32_exception_pointers_t *info) {
    if (seh_top_filter != NULL)
        return seh_top_filter(info);
    return 0;   /* EXCEPTION_CONTINUE_SEARCH */
}

/* GUI-or-console: the unhandled box shows only when this process owns a
 * live window.  A console crasher has none -- serial dump + signal death,
 * which is what the old receipts gate asserts.  A GUI crasher (the dialog
 * fixture creates its window first) gets the modal box. */
#ifndef AURALITE_W32_HOST_TEST
extern int w32_user_window_count(void);
extern int W32ABI MessageBoxA(void *wnd, const char *text,
                              const char *caption, unsigned type);
#define W32_MB_OK 0x00000000u
#define W32_MB_ICONERROR 0x00000010u
#endif

/* The unhandled path, entered with no handler found.
 * Returns 1 with frame->resume filled when execution continues (the top
 * filter said CONTINUE); otherwise dies and does not return. */
/* Host tests assert the core's "would be unhandled" answer instead of
 * dying, so nothing calls this on host -- the attribute keeps -Werror
 * quiet there without splitting the function. */
static int __attribute__((unused))
seh_unhandled(struct w32_seh_dispatch *f,
                         w32_exception_record_t *rec, w32_context_t *ctx) {
    w32_exception_pointers_t eptrs;
    eptrs.record = rec;
    eptrs.context = ctx;

    /* 1. The top filter's last chance. */
    if (seh_top_filter != NULL) {
        int32_t v = seh_top_filter(&eptrs);
        if (v == 1 /* EXCEPTION_EXECUTE_HANDLER */) {
#ifdef AURALITE_W32_HOST_TEST
            fprintf(stderr, "W32-SEH-FILTER-EXECUTE code=0x%08x\n",
                    rec->code);
            abort();
#else
            extern void W32ABI ExitProcess(unsigned int code);
            fprintf(stderr, "W32-SEH-FILTER-EXECUTE code=0x%08x\n",
                    rec->code);
            fflush(stderr);
            ExitProcess(rec->code);
            abort();
#endif
        }
        if (v == -1 /* EXCEPTION_CONTINUE_EXECUTION */) {
            f->resume = *ctx;
            f->resume_valid = 1;
            return 1;
        }
        /* CONTINUE_SEARCH: fall through. */
    }

    /* 2. Always: the serial dump. */
    fprintf(stderr, "%s code=0x%08x pc=0x%llx\n", W32_MSG_UNHANDLED,
            rec->code, (unsigned long long)ctx->rip);
    fflush(stderr);

    /* 3. Uncaught MSVC-shaped C++: the D7 named termination. */
    if (rec->code == W32_CODE_MSCXX) {
        seh_die(W32_MSG_CXX_TYPED_CATCH_GAP, rec->code, ctx->rip);
        return 0;   /* unreachable (host abort / guest ExitProcess) */
    }

#ifdef AURALITE_W32_HOST_TEST
    /* Host tests assert the core's "would be unhandled" answer and never
     * call this; reaching here means the test went wrong. */
    abort();
    return 0;
#else
    /* 4a. GUI session: the modal box, then exit with the exception code. */
    if (w32_user_window_count() > 0) {
        char text[192];
        snprintf(text, sizeof(text),
                 "Unhandled exception 0x%08x at 0x%llx.\n"
                 "Press OK to terminate the process.",
                 rec->code, (unsigned long long)ctx->rip);
        MessageBoxA(NULL, text, "AuraLite", W32_MB_OK | W32_MB_ICONERROR);
        {
            extern void W32ABI ExitProcess(unsigned int code);
            ExitProcess(rec->code);
        }
        abort();
    }
    /* 4b. Console: software deaths exit; fault deaths die on the signal
     * they came in on (receipt-preserving: the gate asserts the signal). */
    if (f->state & W32_SEH_ST_RAISE) {
        extern void W32ABI ExitProcess(unsigned int code);
        ExitProcess(rec->code);
        abort();
    } else {
        extern int seh_fault_signo(struct w32_seh_dispatch *f);
        int signo = seh_fault_signo(f);
        struct sigaction sa;
        sigset_t unblock;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(signo, &sa, NULL);
        /* The fault signal is blocked in this handler's context; unblock
         * it or the re-raise below stays pending and the process exits
         * instead of dying on the signal (POSIX 128+signo convention). */
        sigemptyset(&unblock);
        sigaddset(&unblock, signo);
        sigprocmask(SIG_UNBLOCK, &unblock, NULL);
        raise(signo);
        /* If the signal did not kill us (blocked/ignored?), exit hard. */
        {
            extern void W32ABI ExitProcess(unsigned int code);
            ExitProcess(rec->code);
        }
        abort();
    }
    return 0;
#endif
}

/* ---- the first pass ------------------------------------------------------- */

/* Walk frames calling personalities.  A personality that handles the
 * exception calls RtlUnwind, which longjmps out (this function never
 * sees it return).  A ContinueExecution return fills frame->resume and
 * returns 1.  Bottom of the walk returns 0 (the entry runs seh_unhandled).
 * The walk cap bounds hostile stacks; exceeding it looks like bottom. */
static int seh_first_pass(struct w32_seh_dispatch *f,
                          w32_exception_record_t *rec, w32_context_t *ctx) {
    uint64_t walked = 0;
    for (;;) {
        uint64_t base = 0;
        void *handler_data = NULL;
        uint64_t establisher = 0;
        w32_runtime_function_t *entry;
        void *handler;
        w32_context_t caller;
        w32_dispatcher_context_t dc;

        if (++walked > W32_SEH_WALK_MAX)
            return 0;
        entry = RtlLookupFunctionEntry(ctx->rip, &base, NULL);
        if (!entry) {
            /* Leaf: pop the return address -- but only inside a known
             * image.  Past every image is bottom (no caller to find). */
            uint64_t b2 = 0;
            size_t s2 = 0;
            if (seh_image_for_pc(ctx->rip, &b2, &s2) != 0)
                return 0;
            ctx->rip = *(volatile uint64_t *)(uintptr_t)ctx->rsp;
            ctx->rsp += 8;
            continue;
        }
        caller = *ctx;
        handler = RtlVirtualUnwind(0, base, ctx->rip, entry, &caller,
                                   &handler_data, &establisher, NULL);
        if (establisher == 0)
            return 0;   /* hostile chain: stop, not wild-read */
        if (handler != NULL) {
            /* The personality may call RtlUnwind, which starts its pass
             * from f->live -- the fault context, set at entry (see the
             * struct note).  Nothing to publish here. */
            /* Scopes cover the CALL: past the first frame ControlPc is
             * a return address, which lands exactly on EndAddress when
             * the call ends the __try (MSVC emits that shape too), so
             * step one byte back into the call for the end-exclusive
             * compare.  A software raise starts life as a return
             * address even on its first frame. */
            dc.control_pc = ctx->rip;
            if (walked > 1 || (f->state & W32_SEH_ST_RAISE))
                dc.control_pc -= 1;
            dc.image_base = base;
            dc.function_entry = entry;
            dc.establisher_frame = establisher;
            dc.target_ip = 0;
            dc.context_record = ctx;
            dc.language_handler = handler;
            dc.handler_data = handler_data;
            dc.history_table = NULL;
            dc.scope_index = 0;
            dc.control_pc_is_unwinding = 0;
            {
                int32_t disp;
                disp = ((w32_language_handler_fn)handler)(rec, establisher,
                                                          ctx, &dc);
                if (disp == W32_EXCEPTION_CONTINUE_EXECUTION_NT) {
                    f->resume = *ctx;
                    f->resume_valid = 1;
                    return 1;
                }
                /* NESTED/COLLIDED returns from a personality are
                 * reserved for the OS; a personality returning them
                 * is misbehaving -- treat as CONTINUE_SEARCH. */
            }
        }
        *ctx = caller;
    }
}

/* ---- the second pass ------------------------------------------------------ */

/* Unwind toward (target_frame, target_ip), running termination handlers.
 * Longjmps the dispatch frame (the entry transfers control) -- except a
 * resume-driven sweep to bottom (resume_driven, no target), which RETURNS
 * to _Unwind_Resume so it can terminate.  target_frame==NULL +
 * target_ip==NULL unwinds to bottom and resumes nowhere: the entry then
 * runs seh_unhandled (which is how _CxxThrowException's cleanup sweep ends
 * in D7). */
static void seh_unwind_to(struct w32_seh_dispatch *f, uint64_t target_frame,
                          uint64_t target_ip, w32_exception_record_t *rec,
                          int resume_driven, uint64_t ret_value) {
    uint64_t walked = 0;
    int had_target = (target_frame != 0 || target_ip != 0);

    f->state |= W32_SEH_ST_UNWIND;
    rec->flags |= W32_EXCEPTION_UNWINDING;
    if (had_target)
        rec->flags |= W32_EXCEPTION_TARGET_UNWIND;

    for (;;) {
        uint64_t base = 0;
        void *handler_data = NULL;
        uint64_t establisher = 0;
        w32_runtime_function_t *entry;
        void *handler;
        w32_context_t caller;
        w32_context_t *ctx = &f->live;

        if (++walked > W32_SEH_WALK_MAX)
            break;      /* hostile stack: bottom */
        if (had_target && ctx->rsp == target_frame && target_ip != 0) {
            /* Already there (leaf-shaped target): resume. */
            break;
        }
        entry = RtlLookupFunctionEntry(ctx->rip, &base, NULL);
        if (!entry) {
            uint64_t b2 = 0;
            size_t s2 = 0;
            if (seh_image_for_pc(ctx->rip, &b2, &s2) != 0)
                break;  /* bottom */
            ctx->rip = *(volatile uint64_t *)(uintptr_t)ctx->rsp;
            ctx->rsp += 8;
            if (had_target && ctx->rsp - 8 == target_frame)
                break;
            continue;
        }
        caller = *ctx;
        handler = RtlVirtualUnwind(1, base, ctx->rip, entry, &caller,
                                   &handler_data, &establisher, NULL);
        if (establisher == 0)
            break;      /* hostile chain: stop */
        if (had_target && establisher == target_frame)
            break;      /* the target keeps its frame: do not run it */
        if (handler != NULL) {
            w32_dispatcher_context_t dc;
            /* Scopes cover the CALL: past the first frame ControlPc is
             * a return address, which lands exactly on EndAddress when
             * the call ends the __try (MSVC emits that shape too), so
             * step one byte back into the call for the end-exclusive
             * compare.  A software raise starts life as a return
             * address even on its first frame. */
            dc.control_pc = ctx->rip;
            if (walked > 1 || (f->state & W32_SEH_ST_RAISE))
                dc.control_pc -= 1;
            dc.image_base = base;
            dc.function_entry = entry;
            dc.establisher_frame = establisher;
            dc.target_ip = target_ip;
            dc.context_record = ctx;
            dc.language_handler = handler;
            dc.handler_data = handler_data;
            dc.history_table = NULL;
            dc.scope_index = 0;
            dc.control_pc_is_unwinding = 1;
            /* Handlers on the unwind pass run cleanups and return
             * CONTINUE_SEARCH; any other return is ignored (there is
             * no other legal move mid-unwind). */
            (void)((w32_language_handler_fn)handler)(rec, establisher,
                                                     ctx, &dc);
        }
        *ctx = caller;
    }

    f->live_valid = 0;
    f->state &= ~(unsigned)W32_SEH_ST_UNWIND;
    if (had_target) {
        f->resume = f->live;
        f->resume.rip = target_ip;
        /* RESIDUE2 fix (W32A-4 runtime): a FUNCLET transfer (target_ip != 0)
         * resumes with RSP = the establisher frame, per the documented x64
         * funclet-entry convention this image set was written against (see
         * w32a4_try.asm's header): the faulted frame's return address sits
         * at [RSP], RSP%16 == 8 like any post-call entry, the funclet's own
         * `ret` returns straight into the target frame's caller, and parent
         * locals are reached through the establisher value the personality
         * passes as an argument -- never through the raw RSP.  Resuming with
         * the live (mid-frame) RSP instead -- what this code used to do,
         * rationalised by a comment about libgcc cleanup pads -- put the
         * funclet 8 bytes out of ABI alignment: its call chain reached
         * WriteFile with RSP%16 == 0 and the callee-saved xmm spill
         * (`movaps`) #GP'd before the handler could print a single receipt
         * (that was the w32a4_try/crash/filter gui-shard failure).  The
         * libgcc paths are untouched by construction: their cleanup sweeps
         * run with target_ip == 0 (RtlUnwindEx returns to _Unwind_Resume),
         * and the sweep-to-bottom has no target at all. */
        if (target_ip != 0)
            f->resume.rsp = target_frame;
        if (ret_value)
            f->resume.rax = ret_value;
        f->resume_valid = 1;
    } else {
        f->resume_valid = 0;    /* the entry runs seh_unhandled */
    }
    if (!had_target && resume_driven) {
        /* _Unwind_Resume's sweep is spent: return to the resumer. */
        return;
    }
    sigprocmask(SIG_SETMASK, &f->jb_mask, NULL);
    longjmp(f->jb, 1);
    /* NOTREACHED */
}

/* No active dispatch: Windows would unwind from garbage; we refuse
 * with a name. */
static void seh_outside(void) {
    fprintf(stderr, "%s (RtlUnwind outside dispatch)\n",
            W32_MSG_UNHANDLED);
    fflush(stderr);
#ifdef AURALITE_W32_HOST_TEST
    abort();
#else
    {
        extern void W32ABI ExitProcess(unsigned int code);
        ExitProcess(99u);
        abort();
    }
#endif
}

void W32ABI RtlUnwindEx(void *target_frame, void *target_ip,
                        w32_exception_record_t *record, void *return_value,
                        w32_context_t *context, void *history_table) {
    struct w32_seh_dispatch *f = seh_current_frame();
    (void)return_value;
    (void)history_table;
    if (!f || !(f->state & W32_SEH_ST_ACTIVE))
        seh_outside();
    if (context != NULL) {
        /* An explicit cursor (_Unwind_Resume): adopt it FIRST, so the
         * check below sees the resumed position rather than the stale
         * (possibly spent) live cursor. */
        f->live = *context;
        f->live_valid = 1;
    }
    if (!f->live_valid)
        seh_outside();
    if (!record) {
        if (context != NULL && f->depth > 0) {
            /* Resume-driven without paperwork (_Unwind_Resume passes
             * the cursor, not the record): the dispatch's live record
             * is the unwind's record. */
            record = &f->records[f->depth - 1];
        } else {
            seh_outside();
        }
    }
    if ((f->state & W32_SEH_ST_UNWIND) &&
        (f->depth == 0 || record != &f->records[f->depth - 1])) {
        /* RtlUnwind during RtlUnwind with a FOREIGN record: a true
         * collided unwind (one dispatch interrupting another's sweep).
         * The SAME record means the personality is chaining to a
         * cleanup pad (libgcc's C++ flow does this): allowed -- the
         * walk continues from the live cursor and the outer sweep is
         * abandoned by the longjmp below. */
        seh_die((f->state & W32_SEH_ST_CXX) ? W32_MSG_CXX_DTOR_THROW
                                            : W32_MSG_COLLIDED_UNWIND,
                record->code, f->live.rip);
    }
    /* A resume-driven sweep to bottom (no target) hands control back to
     * the resumer (which terminates) instead of longjmping: no entry
     * frame waits below _Unwind_Resume. */
    seh_unwind_to(f, (uint64_t)(uintptr_t)target_frame,
                  (uint64_t)(uintptr_t)target_ip, record,
                  context != NULL && target_frame == NULL &&
                      target_ip == NULL,
                  (uint64_t)(uintptr_t)return_value);
}

void W32ABI RtlUnwind(void *target_frame, void *target_ip,
                      w32_exception_record_t *record, void *return_value) {
    RtlUnwindEx(target_frame, target_ip, record, return_value, NULL, NULL);
}

/* ---- __C_specific_handler ------------------------------------------------- */

/* MSVC's personality.  HandlerData is a scope table; scopes run innermost
 * LAST (the MSVC emission order), so both passes iterate backward.
 * Filters: LONG f(EXCEPTION_POINTERS*, establisher).  Cleanups: void
 * f(establisher).  Passing establisher to cleanups is this ABI's rule
 * (documented HERE, in the one function that implements it): funclets
 * reach parent locals through it.
 *
 * On EXECUTE_HANDLER this function calls RtlUnwind and never returns. */
int32_t W32ABI __C_specific_handler(w32_exception_record_t *record,
                                    uint64_t establisher_frame,
                                    w32_context_t *context,
                                    w32_dispatcher_context_t *dispatch) {
    const uint8_t *tab;
    uint32_t count, i;
    uint64_t image_base;
    size_t span = 0;
    uint32_t pc_rva;
    int unwinding;

    if (!record || !context || !dispatch || !dispatch->handler_data)
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    image_base = dispatch->image_base;
    if (seh_span_for_base(image_base, &span) != 0 || span == 0)
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    tab = (const uint8_t *)dispatch->handler_data;
    if ((uint64_t)(uintptr_t)tab < image_base ||
        (uint64_t)(uintptr_t)tab - image_base >= span)
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    if ((uint64_t)(uintptr_t)tab - image_base > span - 4)
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    count = (uint32_t)tab[0] | ((uint32_t)tab[1] << 8) |
            ((uint32_t)tab[2] << 16) | ((uint32_t)tab[3] << 24);
    if (count > W32_SEH_SCOPE_MAX)
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    if (count > 0 && (uint64_t)(uintptr_t)tab - image_base >
        span - 4 - (uint64_t)count * sizeof(w32_scope_record_t))
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    if (dispatch->control_pc < image_base ||
        dispatch->control_pc - image_base >= span)
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    pc_rva = (uint32_t)(dispatch->control_pc - image_base);
    unwinding = (record->flags & W32_EXCEPTION_UNWINDING) != 0;

    if (unwinding) {
        /* Second pass: run the covering cleanups, innermost first. */
        for (i = count; i-- > 0;) {
            const w32_scope_record_t *s =
                (const w32_scope_record_t *)(tab + 4 +
                                             i * sizeof(*s));
            if (pc_rva < s->begin_address || pc_rva >= s->end_address)
                continue;
            if (s->handler_address != W32_SCOPE_CLEANUP)
                continue;
            if (s->jump_target == 0 ||
                s->jump_target >= span)
                continue;   /* hostile scope: skip, keep unwinding */
            {
                typedef void (W32ABI *cleanup_fn)(uint64_t establisher);
                cleanup_fn fn =
                    (cleanup_fn)(uintptr_t)(image_base + s->jump_target);
                fn(establisher_frame);
            }
        }
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    }

    /* First pass: innermost covering filter decides. */
    for (i = count; i-- > 0;) {
        const w32_scope_record_t *s =
            (const w32_scope_record_t *)(tab + 4 + i * sizeof(*s));
        if (pc_rva < s->begin_address || pc_rva >= s->end_address)
            continue;
        if (s->handler_address == W32_SCOPE_CLEANUP)
            continue;
        if (s->handler_address == 0 || s->handler_address >= span)
            continue;       /* hostile scope: skip */
        if (s->jump_target >= span)
            continue;
        {
            typedef int32_t (W32ABI *filter_fn)(
                w32_exception_pointers_t *eptrs, uint64_t establisher);
            w32_exception_pointers_t eptrs;
            int32_t v;
            eptrs.record = record;
            eptrs.context = context;
            v = ((filter_fn)(uintptr_t)(image_base +
                                        s->handler_address))(&eptrs,
                                                             establisher_frame);
            if (v == 1 /* EXCEPTION_EXECUTE_HANDLER */) {
                dispatch->scope_index = i;
                RtlUnwind((void *)(uintptr_t)establisher_frame,
                          (void *)(uintptr_t)(image_base + s->jump_target),
                          record, NULL);
                /* NOTREACHED (RtlUnwind longjmps). */
                return W32_EXCEPTION_CONTINUE_SEARCH_NT;
            }
            if (v == -1 /* EXCEPTION_CONTINUE_EXECUTION */)
                return W32_EXCEPTION_CONTINUE_EXECUTION_NT;
            /* CONTINUE_SEARCH: try the next scope outward. */
        }
    }
    return W32_EXCEPTION_CONTINUE_SEARCH_NT;
}

int32_t W32ABI _XcptFilter(uint32_t code, w32_exception_pointers_t *info) {
    (void)info;
    /* The default CRT filter: hardware faults execute the handler, the
     * exotic codes keep searching.  (Never CONTINUE_EXECUTION, like the
     * MSVCRT original.) */
    switch (code) {
    case W32_EXCEPTION_ACCESS_VIOLATION:
    case W32_EXCEPTION_DATATYPE_MISALIGNMENT:
    case W32_EXCEPTION_BREAKPOINT:
    case W32_EXCEPTION_SINGLE_STEP:
    case W32_EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case W32_EXCEPTION_FLT_DENORMAL_OPERAND:
    case W32_EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case W32_EXCEPTION_FLT_INEXACT_RESULT:
    case W32_EXCEPTION_FLT_INVALID_OPERATION:
    case W32_EXCEPTION_FLT_OVERFLOW:
    case W32_EXCEPTION_FLT_STACK_CHECK:
    case W32_EXCEPTION_FLT_UNDERFLOW:
    case W32_EXCEPTION_INT_DIVIDE_BY_ZERO:
    case W32_EXCEPTION_INT_OVERFLOW:
    case W32_EXCEPTION_PRIV_INSTRUCTION:
    case W32_EXCEPTION_ILLEGAL_INSTRUCTION:
    case W32_EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case W32_EXCEPTION_STACK_OVERFLOW:
        return 1;   /* EXCEPTION_EXECUTE_HANDLER */
    default:
        return 0;   /* EXCEPTION_CONTINUE_SEARCH */
    }
}

/* ---- RaiseException ------------------------------------------------------- */

/* The RaiseException core: the trampoline above captured the CALLER
 * (the guest frame that raised) and tailcalls here.  Kept (not folded
 * into the trampoline) so the host test drives the same dispatch. */
static void W32ABI __attribute__((used))
RaiseException_c(uint32_t code, uint32_t flags, uint32_t num_args,
                 const uint64_t *args, const w32_context_t *caller) {
    struct w32_seh_dispatch *f = seh_current_frame();
    w32_context_t ctx;
    w32_exception_record_t rec;
    uint32_t i;

    if (!f) {
        seh_die(W32_MSG_UNHANDLED, code, 0);
        return;
    }
    if (f->depth >= W32_SEH_NEST_MAX) {
        seh_die(W32_MSG_UNHANDLED, code, 0);
        return;
    }
    ctx = *caller;   /* the trampoline captured the guest frame */
    memset(&rec, 0, sizeof(rec));
    rec.code = code;
    rec.flags = flags & (W32_EXCEPTION_NONCONTINUABLE |
                         W32_EXCEPTION_UNWINDING);
    rec.address = (void *)(uintptr_t)ctx.rip;
    rec.num_params = num_args > W32_EXCEPTION_MAXIMUM_PARAMETERS
                         ? W32_EXCEPTION_MAXIMUM_PARAMETERS
                         : num_args;
    for (i = 0; i < rec.num_params; i++)
        rec.params[i] = args ? args[i] : 0;

    f->records[f->depth] = rec;
    if (f->depth > 0)
        f->records[f->depth].nested = &f->records[f->depth - 1];
    f->depth++;
    f->state |= W32_SEH_ST_ACTIVE | W32_SEH_ST_RAISE;
    if ((f->state & W32_SEH_ST_UNWIND) && f->live_valid) {
        /* Nested during a sweep (libgcc's cancel raise): the captured
         * ctx is host-stack (inside the personality) and unusable as a
         * search start -- the walk would bottom out in host frames
         * before reaching the guest target.  Keep the outer sweep's
         * guest cursor (already in f->live): the raising personality
         * runs there, so its target check matches on the first frame. */
    } else {
        f->live = ctx;
    }
    f->live_valid = 1;
    f->resume_valid = 0;

    sigprocmask(0 /*SIG_BLOCK with NULL set: query*/, NULL, &f->jb_mask);
    if (setjmp(f->jb) != 0) {
        /* Back from RtlUnwind: transfer (guest) or bounce to the
         * harness (host, which owns the resume decision). */
#ifdef AURALITE_W32_HOST_TEST
        f->depth--;
        if (f->depth == 0)
            f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE | W32_SEH_ST_RAISE);
        if (f->harness_set)
            siglongjmp(f->harness_jb, 1);
        abort();    /* host resume with no harness: the test is wrong */
#else
        if (f->resume_valid) {
            extern void w32_seh_do_resume(const w32_context_t *ctx);
            f->depth--;
            if (f->depth == 0)
                f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE |
                                        W32_SEH_ST_RAISE);
            w32_seh_do_resume(&f->resume);
        }
        /* Unwind-to-bottom (no resume): the unhandled path. */
        {
            w32_context_t uctx = f->live;
            if (seh_unhandled(f, &f->records[f->depth - 1], &uctx)) {
                extern void w32_seh_do_resume(const w32_context_t *ctx);
                f->depth--;
                if (f->depth == 0)
                    f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE |
                                            W32_SEH_ST_RAISE);
                w32_seh_do_resume(&f->resume);
            }
            /* seh_unhandled dies; reaching here is impossible. */
            abort();
        }
#endif
    }

    /* First pass over the live context (f->live, not the raw capture:
     * a raise nested inside a sweep keeps the outer guest cursor there). */
    {
        w32_context_t cur = f->live;
#ifdef AURALITE_W32_HOST_TEST
        int cont = seh_first_pass(f, &f->records[f->depth - 1], &cur);
        if (cont) {
            f->depth--;
            if (f->depth == 0)
                f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE |
                                        W32_SEH_ST_RAISE);
            if (f->harness_set)
                siglongjmp(f->harness_jb, 1);
            abort();
        }
        /* Bottom: the host test inspects f->records instead of dying. */
        f->depth--;
        if (f->depth == 0)
            f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE | W32_SEH_ST_RAISE);
        return;
#else
        int cont = seh_first_pass(f, &f->records[f->depth - 1], &cur);
        if (cont) {
            extern void w32_seh_do_resume(const w32_context_t *ctx);
            f->depth--;
            if (f->depth == 0)
                f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE |
                                        W32_SEH_ST_RAISE);
            w32_seh_do_resume(&f->resume);
        }
        /* Bottom: a continuable GCC throw is libgcc's uncaught path,
         * not a death.  _Unwind_RaiseException calls RaiseException
         * and EXPECTS it to return (it then reports END_OF_STACK so
         * __cxa_throw runs terminate) -- but the C++ cleanups must
         * run first, so sweep them here (the sweep transfers through
         * each frame's landing pad via the cancel protocol and only
         * returns when no cleanup claimed the walk).  Anything else
         * at bottom is unhandled: depth/state stay set across the
         * call (seh_unhandled's re-raise/exit choice reads them);
         * only a CONTINUE return unwinds them. */
        if (rec.code == 0x20474343u /*STATUS_GCC_THROW*/ &&
            !(rec.flags & W32_EXCEPTION_NONCONTINUABLE)) {
            seh_unwind_to(f, 0, 0, &rec, 1, 0);
            f->depth--;
            if (f->depth == 0)
                f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE |
                                        W32_SEH_ST_RAISE);
            return;
        }
        {
            int k;
            w32_context_t uctx = f->live;
            k = seh_unhandled(f, &rec, &uctx);
            if (k) {
                extern void w32_seh_do_resume(const w32_context_t *ctx);
                f->depth--;
                if (f->depth == 0)
                    f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE |
                                            W32_SEH_ST_RAISE);
                w32_seh_do_resume(&f->resume);
            }
            abort();
        }
#endif
    }
}

/* ---- the fault entry (guest) ------------------------------------------------
 * Replaces the W32-6 setjmp arming with the same call shape, so the
 * w32run startup sequence is unchanged.
 */
#ifndef AURALITE_W32_HOST_TEST

static int seh_armed = 0;
/* The signal each dispatch came in on (for the console re-raise). */
static int seh_nest_signo[W32_SEH_NEST_MAX];

int seh_fault_signo(struct w32_seh_dispatch *f) {
    if (!f || f->depth == 0)
        return SIGSEGV;
    return seh_nest_signo[f->depth - 1];
}

static uint32_t seh_signal_to_code(int signo, int si_code) {
    switch (signo) {
    case SIGFPE:
        /* (The W32-6 note still applies: AuraLite raises FPE_INTDIV for
         * #DE; the switch stays total so a new kernel code cannot slip
         * through mislabelled.) */
        if (si_code == FPE_INTDIV) return W32_EXCEPTION_INT_DIVIDE_BY_ZERO;
        return W32_EXCEPTION_INT_DIVIDE_BY_ZERO;
    case SIGILL:  return W32_EXCEPTION_ILLEGAL_INSTRUCTION;
    case SIGBUS:  return W32_EXCEPTION_DATATYPE_MISALIGNMENT;
    case SIGTRAP: return W32_EXCEPTION_BREAKPOINT;
    case SIGSEGV: /* fall through */
    default:      return W32_EXCEPTION_ACCESS_VIOLATION;
    }
}

/* The kernel's struct signal_frame, mirrored (guest-only).  Layout MUST
 * match kernel/proc/signal.h: fxsave, GPRs, rip/rflags/rsp, cs/ss, the
 * saved mask and signo.  do_sigreturn() restores the interrupted state
 * from the FRAME (never from the ucontext), so a fault resume writes
 * here; the frame sits exactly at handler-entry-RSP + 8 (the trampoline
 * slot is at entry RSP), which the entry stub below passes in. */
struct seh_signal_frame {
    uint8_t  fxsave_area[512];
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
    uint64_t cs, ss;
    uint32_t saved_mask;
    uint32_t signo;
} __attribute__((aligned(16)));
_Static_assert(sizeof(struct seh_signal_frame) == 688, "signal frame size");
_Static_assert(offsetof(struct seh_signal_frame, rip) == 632, "sf rip");
_Static_assert(offsetof(struct seh_signal_frame, rsp) == 648, "sf rsp");

/* Snapshots of the fault siginfo/ucontext, taken by the entry stub before
 * any stack use.  The kernel reserves both structs BELOW the handler's
 * entry RSP, so a C handler's own prolog pushes and locals overwrite the
 * top of the ucontext (RSP/CS/SS/RFLAGS/RIP, in that order) before the C
 * code reads a single field -- the first QEMU run showed RSP arriving as
 * 0 with a correct RIP for exactly this reason.  The stub copies both
 * structs aside with zero stack traffic and hands C the copies; the
 * originals are dead to us (sigreturn ignores them).  One set of statics
 * is enough: the C handler reads them only at entry, into its own
 * locals, so a nested fault's re-snapshot cannot corrupt the outer
 * dispatch. */
static siginfo_t seh_fault_si __attribute__((used));
static ucontext_t seh_fault_uc __attribute__((used));
/* Entry f/sf stashed outside the frame (re-seat after the first pass). */
static struct w32_seh_dispatch *seh_entry_f;
static struct seh_signal_frame *seh_entry_sf;
/* siginfo is 32 bytes (the 12-byte waitid arm pads the union to 16). */
#define SEH_SI_QWORDS 4u
#define SEH_UC_QWORDS 23u
_Static_assert(sizeof(siginfo_t) == SEH_SI_QWORDS * 8, "siginfo size");
_Static_assert(sizeof(ucontext_t) == SEH_UC_QWORDS * 8, "ucontext size");
#define SEH_STR_(x) #x
#define SEH_STR(x) SEH_STR_(x)

/* Naked entry for the fault signals (installed by w32_seh_init).  SysV
 * args on arrival: RDI = signo, RSI = &siginfo, RDX = &ucontext, RSP =
 * the trampoline slot.  Uses no stack at all (every scratch is a
 * caller-saved register, dead to the handler by definition), snapshots
 * both structs, then tail-jumps to the C handler as
 * seh_fault_c(signo, &snap_si, &snap_uc, frame) with frame = entry RSP+8.
 * The C handler's eventual `ret` lands in the trampoline, as usual. */
__asm__(
".globl seh_fault_entry\n"
"seh_fault_entry:\n"
"    mov %rdi, %r8\n"
"    mov %rsi, %r9\n"
"    mov %rdx, %r10\n"
"    mov %rsp, %r11\n"
"    mov %r9, %rsi\n"
"    lea seh_fault_si(%rip), %rdi\n"
"    mov $" SEH_STR(SEH_SI_QWORDS) ", %rcx\n"
"    rep movsq\n"
"    mov %r10, %rsi\n"
"    lea seh_fault_uc(%rip), %rdi\n"
"    mov $" SEH_STR(SEH_UC_QWORDS) ", %rcx\n"
"    rep movsq\n"
"    mov %r8, %rdi\n"
"    lea seh_fault_si(%rip), %rsi\n"
"    lea seh_fault_uc(%rip), %rdx\n"
"    lea 8(%r11), %rcx\n"
"    jmp seh_fault_c\n"
);

/* Copy a resume CONTEXT into the kernel's signal frame (GPRs + RIP/RSP +
 * RFLAGS; do_sigreturn restores exactly these, pins CS/SS itself, and
 * restores the untouched FPU image -- the ucontext copy is NOT read back,
 * so writing it was a no-op). */
static void seh_apply_resume(struct seh_signal_frame *sf,
                             const w32_context_t *r) {
    sf->rax = r->rax;
    sf->rcx = r->rcx;
    sf->rdx = r->rdx;
    sf->rbx = r->rbx;
    sf->rsp = r->rsp;
    sf->rbp = r->rbp;
    sf->rsi = r->rsi;
    sf->rdi = r->rdi;
    sf->r8 = r->r8;
    sf->r9 = r->r9;
    sf->r10 = r->r10;
    sf->r11 = r->r11;
    sf->r12 = r->r12;
    sf->r13 = r->r13;
    sf->r14 = r->r14;
    sf->r15 = r->r15;
    sf->rip = r->rip;
    sf->rflags = (sf->rflags & ~0x8d5ull) | (r->eflags & 0x8d5ull);
}

static void seh_fault_c(int signo, siginfo_t *info, ucontext_t *uc,
                         struct seh_signal_frame *volatile sf)
    __attribute__((used));
static void seh_fault_c(int signo, siginfo_t *info, ucontext_t *uc,
                         struct seh_signal_frame *volatile sf) {
    /* Volatile: both survive the RtlUnwind longjmp below (their stack
     * slots are re-read in the back path), and the fault path re-seats
     * them after the first pass (see below). */
    struct w32_seh_dispatch *volatile f = seh_current_frame();
    w32_context_t ctx;
    w32_exception_record_t *rec;
    int si_code = info ? info->si_code : 0;

    if (!f || !sf) {
        /* Before init (or a foreign thread): die on the signal. */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(signo, &sa, NULL);
        raise(signo);
        _exit(127);
    }
    if (f->depth >= W32_SEH_NEST_MAX) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(signo, &sa, NULL);
        raise(signo);
        _exit(127);
    }
    if (f->state & W32_SEH_ST_UNWIND) {
        /* A fault during the second pass is a collided unwind.  (During
         * a C++ sweep it is a throwing destructor.) */
        seh_die((f->state & W32_SEH_ST_CXX) ? W32_MSG_CXX_DTOR_THROW
                                            : W32_MSG_COLLIDED_UNWIND,
                seh_signal_to_code(signo, si_code),
                (uint64_t)uc->uc_mcontext.rip);
    }

    /* Build the fault CONTEXT from the machine state.  XMMs read zero:
     * AuraLite's mcontext does not save them (the header's documented
     * divergence), so there is nothing to restore. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.context_flags = W32_CONTEXT_FULL | W32_CONTEXT_SEGMENTS;
    ctx.mx_csr = 0x1f80;
    ctx.seg_cs = (uint16_t)uc->uc_mcontext.cs;
    ctx.seg_ss = (uint16_t)uc->uc_mcontext.ss;
    ctx.eflags = (uint32_t)uc->uc_mcontext.rflags;
    ctx.rax = uc->uc_mcontext.rax;
    ctx.rcx = uc->uc_mcontext.rcx;
    ctx.rdx = uc->uc_mcontext.rdx;
    ctx.rbx = uc->uc_mcontext.rbx;
    ctx.rsp = uc->uc_mcontext.rsp;
    ctx.rbp = uc->uc_mcontext.rbp;
    ctx.rsi = uc->uc_mcontext.rsi;
    ctx.rdi = uc->uc_mcontext.rdi;
    ctx.r8 = uc->uc_mcontext.r8;
    ctx.r9 = uc->uc_mcontext.r9;
    ctx.r10 = uc->uc_mcontext.r10;
    ctx.r11 = uc->uc_mcontext.r11;
    ctx.r12 = uc->uc_mcontext.r12;
    ctx.r13 = uc->uc_mcontext.r13;
    ctx.r14 = uc->uc_mcontext.r14;
    ctx.r15 = uc->uc_mcontext.r15;
    ctx.rip = uc->uc_mcontext.rip;
    if (signo == SIGTRAP) {
        /* int3 leaves RIP past the byte; Windows reports the byte. */
        ctx.rip -= 1;
    }

    memset(&f->records[f->depth], 0, sizeof(f->records[f->depth]));
    rec = &f->records[f->depth];
    rec->code = seh_signal_to_code(signo, si_code);
    rec->address = (void *)(uintptr_t)ctx.rip;
    if (signo == SIGSEGV && info && info->si_addr) {
        rec->num_params = 2;
        rec->params[0] = 0; /* read (si_code does not say; documented) */
        rec->params[1] = (uint64_t)(uintptr_t)info->si_addr;
    }
    if (f->depth > 0)
        rec->nested = &f->records[f->depth - 1];
    seh_nest_signo[f->depth] = signo;
    f->depth++;
    f->state |= W32_SEH_ST_ACTIVE;
    f->state &= ~(unsigned)W32_SEH_ST_RAISE;
    f->live = ctx;
    f->live_valid = 1;
    f->resume_valid = 0;
    seh_entry_f = f;
    seh_entry_sf = sf;

    sigprocmask(0 /*SIG_BLOCK with NULL set: query*/, NULL, &f->jb_mask);
    if (setjmp(f->jb) != 0) {
        /* Back from RtlUnwind: apply the resume CONTEXT to the ucontext
         * and return -- sigreturn restores the mask, so the SECOND fault
         * is still caught. */
        if (f->resume_valid) {
            seh_apply_resume(sf, &f->resume);
            f->depth--;
            if (f->depth == 0)
                f->state &= ~(unsigned)W32_SEH_ST_ACTIVE;
            return;
        }
        /* Unwind-to-bottom: the unhandled path (dies). */
        {
            w32_context_t uctx = f->live;
            w32_exception_record_t urec = f->records[f->depth - 1];
            if (seh_unhandled(f, &urec, &uctx)) {
                f->depth--;
                if (f->depth == 0)
                    f->state &= ~(unsigned)W32_SEH_ST_ACTIVE;
                sf->rip = f->resume.rip;
                sf->rsp = f->resume.rsp;
                return;
            }
            _exit(127); /* unreachable */
        }
    }

    {
        w32_context_t cur = ctx;
        int cont = seh_first_pass(f, rec, &cur);
        /* Re-seat f/sf from the entry snapshots: one QEMU run showed
         * f's stack slot holding garbage after the first pass (the
         * personality calls foreign asm on this stack), faulting the
         * resume copy.  Never reproduced since; the re-seat is cheap
         * insurance with no behaviour change when the slots are intact. */
        f = seh_entry_f;
        sf = seh_entry_sf;
        if (cont) {
            seh_apply_resume(sf, &f->resume);
            f->depth--;
            if (f->depth == 0)
                f->state &= ~(unsigned)W32_SEH_ST_ACTIVE;
            return;
        }
        {
            int k;
            w32_context_t uctx = f->live;
            w32_exception_record_t urec = *rec;
            k = seh_unhandled(f, &urec, &uctx);
            if (k) {
                f->depth--;
                if (f->depth == 0)
                    f->state &= ~(unsigned)W32_SEH_ST_ACTIVE;
                sf->rip = f->resume.rip;
                sf->rsp = f->resume.rsp;
                return;
            }
            _exit(127); /* unreachable */
        }
    }
}

int w32_seh_init(void) {
    struct sigaction sa;
    int sigs[] = { SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGTRAP };
    size_t i;
    if (seh_armed)
        return 0;
    extern void seh_fault_entry(int, siginfo_t *, void *);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = seh_fault_entry;
    sa.sa_flags = SA_SIGINFO;
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0)
            return -1;
    }
    seh_armed = 1;
    return 0;
}

int w32_seh_armed(void) {
    return seh_armed;
}

#else /* AURALITE_W32_HOST_TEST */

int w32_seh_init(void) {
    /* Host tests drive dispatch directly; no signals to arm. */
    return 0;
}

int w32_seh_armed(void) {
    return 0;
}

#endif /* AURALITE_W32_HOST_TEST */

/* ---- the msvcrt C++ surface (D7: named, never silently wrong) -------------- */

/* _CxxThrowException(obj, throw_info): sweep cleanups to bottom,
 * then terminate with the typed-catch gap message.  Catch clauses are
 * NEVER matched (the D7 gap): a silent wrong-catch would be worse than a
 * named death. */
static void W32ABI __attribute__((used))
_CxxThrowException_c(void *obj, void *throw_info,
                     const w32_context_t *caller) {
    struct w32_seh_dispatch *f = seh_current_frame();
    w32_context_t ctx;
    w32_exception_record_t rec;

    (void)obj;
    (void)throw_info;
    if (!f) {
        seh_die(W32_MSG_CXX_TYPED_CATCH_GAP, W32_CODE_MSCXX, 0);
        return;
    }
    if (f->depth >= W32_SEH_NEST_MAX) {
        seh_die(W32_MSG_CXX_TYPED_CATCH_GAP, W32_CODE_MSCXX, 0);
        return;
    }
    ctx = *caller;   /* the trampoline captured the guest frame */
    memset(&rec, 0, sizeof(rec));
    rec.code = W32_CODE_MSCXX;
    rec.flags = W32_EXCEPTION_NONCONTINUABLE;
    rec.address = (void *)(uintptr_t)ctx.rip;
    rec.num_params = 3;
    rec.params[0] = 0x19930520;                     /* magic */
    rec.params[1] = (uint64_t)(uintptr_t)obj;
    rec.params[2] = (uint64_t)(uintptr_t)throw_info;

    f->records[f->depth] = rec;
    if (f->depth > 0)
        f->records[f->depth].nested = &f->records[f->depth - 1];
    f->depth++;
    f->state |= W32_SEH_ST_ACTIVE | W32_SEH_ST_RAISE | W32_SEH_ST_CXX;
    f->live = ctx;
    f->live_valid = 1;
    f->resume_valid = 0;

    sigprocmask(0 /*SIG_BLOCK with NULL set: query*/, NULL, &f->jb_mask);
    if (setjmp(f->jb) != 0) {
        /* The sweep is done (unwind-to-bottom never resumes): the gap. */
        f->depth--;
        if (f->depth == 0)
            f->state &= ~(unsigned)(W32_SEH_ST_ACTIVE | W32_SEH_ST_RAISE |
                                    W32_SEH_ST_CXX);
#ifdef AURALITE_W32_HOST_TEST
        if (f->harness_set)
            siglongjmp(f->harness_jb, 1);
        abort();
#else
        seh_die(W32_MSG_CXX_TYPED_CATCH_GAP, W32_CODE_MSCXX,
                f->live.rip);
#endif
    }

    /* Sweep every frame's cleanups, then fall into the gap above. */
    RtlUnwindEx(NULL, NULL, &f->records[f->depth - 1], NULL, NULL, NULL);
    /* NOTREACHED */
}

void W32ABI w32_cxx_terminate(void) {
    struct w32_seh_dispatch *f = seh_current_frame();
    uint64_t pc = (f && f->live_valid) ? f->live.rip : 0;
    seh_die(W32_MSG_CXX_TERMINATE, 0, pc);
}

void W32ABI _purecall(void) {
    struct w32_seh_dispatch *f = seh_current_frame();
    uint64_t pc = (f && f->live_valid) ? f->live.rip : 0;
    seh_die(W32_MSG_PURECALL, 0, pc);
}
