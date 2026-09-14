/*
 * test_w32_a4.c — host unit tests for W32APP_PLAN.md phase W32A-4.
 *
 * The gate from the plan, in order:
 *   - .pdata lookup + UNWIND_INFO chain validation refuse hostile input
 *     without crash or over-read (truncations, bitflips, bad versions,
 *     chained cycles, handler RVAs outside the image);
 *   - RtlVirtualUnwind applies every unwind op (push/alloc/setfp/save/
 *     savefar/xmm/machframe/chained/indirect) with exact caller state;
 *   - __C_specific_handler runs filters innermost-first, cleanups on the
 *     unwind pass, and unwinds to JumpTarget on EXECUTE;
 *   - the dispatch core walks synthetic frames and resumes where the
 *     personality said, running the between-frames cleanups.
 *
 * Like test_w32_pe.c, this file #includes the implementation and drives it
 * over synthetic images (no build artefacts needed).  Personalities and
 * funclets run as in-image machine-code stubs (movabs+jmp, 12 bytes) that
 * tail-call C functions -- the dispatch CALLS them for real, through the
 * MS ABI, exactly like the guest calls fixture funclets.  The images are
 * mmap'd PROT_EXEC for that; ASan stays quiet (all in-bounds).
 *
 * The llvm-readobj equivalence gate lives beside this file
 * (tests/unit/test_w32_unwind_equiv.sh): an independent decoder
 * cross-checks the same fixture shape this file's builder models.
 */

#define AURALITE_W32_HOST_TEST 1
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>

#include "w32/w32_seh.h"
#include "w32/w32_crt.h"
#include "../../w32/src/w32_seh.c"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { unsigned long long _a = (unsigned long long)(a), \
                            _e = (unsigned long long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=0x%llx want 0x%llx\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- synthetic image builder ----------------------------------------------
 *
 * Layout (RVAs):
 *   0x000 DOS header, e_lfanew = 0x80
 *   0x080 PE sig + COFF + optional (dir[3] = .pdata)
 *   0x400 .pdata (RUNTIME_FUNCTIONs, sorted)
 *   0x600 .xdata (UNWIND_INFOs + handlers' data)
 *   0x800 code (opaque bytes, or movabs+jmp stubs -- see below)
 *
 * Stack convention used by every test (ascending addresses):
 *   [saved reg][return address] -- push decrements RSP, so the saved
 *   register sits BELOW (at a lower address than) the return address.
 */
#define IMG_SIZE   0x1000u
#define PE_OFF     0x80u
#define OPT_OFF    (PE_OFF + 24u)
#define PDATA_RVA  0x400u
#define XDATA_RVA  0x600u
#define CODE_RVA   0x800u

static void w16(uint8_t *b, uint32_t o, uint16_t v) {
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8);
}
static void w32_(uint8_t *b, uint32_t o, uint32_t v) {
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v>>8);
    b[o+2] = (uint8_t)(v>>16); b[o+3] = (uint8_t)(v>>24);
}

static void img_headers(uint8_t *b) {
    b[0] = 'M'; b[1] = 'Z';
    w32_(b, 0x3c, PE_OFF);
    b[PE_OFF+0] = 'P'; b[PE_OFF+1] = 'E';
    w16(b, OPT_OFF, 0x020b);            /* PE32+ */
    w16(b, PE_OFF + 20, 240);           /* SizeOfOptionalHeader (full dirs) */
    w32_(b, OPT_OFF + 112 + 3*8, PDATA_RVA);   /* dir[3].rva */
}

static uint8_t *make_image(void) {
    uint8_t *b = (uint8_t *)calloc(1, IMG_SIZE);
    if (!b) { printf("oom\n"); exit(1); }
    img_headers(b);
    return b;
}

/* Executable image: same bytes, PROT_EXEC, for the stub tests. */
static uint8_t *make_ximage(void) {
    uint8_t *b = (uint8_t *)mmap(NULL, IMG_SIZE, PROT_READ | PROT_WRITE |
                                 PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1, 0);
    if (b == MAP_FAILED) { printf("mmap failed\n"); exit(1); }
    memset(b, 0, IMG_SIZE);
    img_headers(b);
    return b;
}

static void free_ximage(uint8_t *b) {
    munmap(b, IMG_SIZE);
}

/* Emit a tail-call stub at CODE_RVA+code_off: movabs rax,target; jmp rax.
 * Transparent under the MS ABI (argument registers untouched).  Returns
 * the stub's RVA (a valid in-image handler address). */
static uint32_t emit_stub(uint8_t *b, uint32_t code_off, void *target) {
    uint8_t *p = b + CODE_RVA + code_off;
    uint64_t t = (uint64_t)(uintptr_t)target;
    p[0] = 0x48; p[1] = 0xb8;
    memcpy(p + 2, &t, 8);
    p[10] = 0xff; p[11] = 0xe0;
    return CODE_RVA + code_off;
}

static void set_pdata_size(uint8_t *b, uint32_t nentries) {
    w32_(b, OPT_OFF + 112 + 3*8 + 4, nentries * 12u);
}

static void add_func(uint8_t *b, uint32_t idx, uint32_t begin, uint32_t end,
                     uint32_t unwind) {
    uint32_t o = PDATA_RVA + idx * 12u;
    w32_(b, o, begin); w32_(b, o + 4, end); w32_(b, o + 8, unwind);
}

/* Write an UNWIND_INFO at xdata_off (relative to XDATA_RVA).  Returns the
 * RVA just past the codes (the handler tail goes there). */
static uint32_t write_unwind(uint8_t *b, uint32_t xoff, uint8_t flags,
                             uint8_t prolog, uint8_t count, uint8_t fpreg,
                             uint8_t fpoff, const uint8_t *codes) {
    uint32_t o = XDATA_RVA + xoff;
    b[o] = (uint8_t)(W32_UNW_VERSION_1 | flags);
    b[o+1] = prolog;
    b[o+2] = count;
    b[o+3] = (uint8_t)((fpoff << 4) | fpreg);
    memcpy(b + o + 4, codes, (size_t)count * 2);
    return XDATA_RVA + xoff + 4u + (uint32_t)(((count + 1) & ~1u) * 2);
}

/* ---- lookup --------------------------------------------------------------- */

static void test_lookup(void) {
    uint8_t pd[36];
    const w32_runtime_function_t *e;
    memset(pd, 0, sizeof(pd));
    /* Three sorted entries: [0x800,0x820) [0x820,0x840) [0x900,0x910). */
    w32_(pd, 0, 0x800); w32_(pd, 4, 0x820); w32_(pd, 8, 0x600);
    w32_(pd, 12, 0x820); w32_(pd, 16, 0x840); w32_(pd, 20, 0x620);
    w32_(pd, 24, 0x900); w32_(pd, 28, 0x910); w32_(pd, 32, 0x640);

    e = w32_seh_lookup(pd, sizeof(pd), 0x800);
    CHECK(e && e->begin_address == 0x800);
    e = w32_seh_lookup(pd, sizeof(pd), 0x81f);
    CHECK(e && e->begin_address == 0x800);
    e = w32_seh_lookup(pd, sizeof(pd), 0x820);
    CHECK(e && e->begin_address == 0x820);
    e = w32_seh_lookup(pd, sizeof(pd), 0x83f);
    CHECK(e && e->begin_address == 0x820);
    e = w32_seh_lookup(pd, sizeof(pd), 0x840);
    CHECK(e == NULL);                       /* gap between entries */
    e = w32_seh_lookup(pd, sizeof(pd), 0x905);
    CHECK(e && e->begin_address == 0x900);
    e = w32_seh_lookup(pd, sizeof(pd), 0x910);
    CHECK(e == NULL);                       /* End is exclusive */
    e = w32_seh_lookup(pd, sizeof(pd), 0x7ff);
    CHECK(e == NULL);                       /* before the first */
    e = w32_seh_lookup(pd, sizeof(pd), 0x2000);
    CHECK(e == NULL);                       /* past the last */
    e = w32_seh_lookup(pd, 0, 0x800);
    CHECK(e == NULL);                       /* empty table */
    e = w32_seh_lookup(pd, 13, 0x800);
    CHECK(e == NULL);                       /* hostile size */
    e = w32_seh_lookup(NULL, sizeof(pd), 0x800);
    CHECK(e == NULL);
}

/* ---- chain validation ----------------------------------------------------- */

static void test_validate_chain(void) {
    uint8_t *b = make_image();
    /* A plain 2-code info at xoff 0. */
    const uint8_t codes[] = { 0x04, 0x32,   /* off 4: ALLOC_SMALL+2 */
                              0x01, 0x50 }; /* off 1: PUSH_NONVOL rbp */
    write_unwind(b, 0, 0, 5, 2, 0, 0, codes);
    CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA), 0);

    /* Indirect entry: .xdata holds a second RUNTIME_FUNCTION. */
    w32_(b, XDATA_RVA + 0x40, CODE_RVA);
    w32_(b, XDATA_RVA + 0x44, CODE_RVA + 0x20);
    w32_(b, XDATA_RVA + 0x48, XDATA_RVA);
    CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA + 0x40 + 1), 0);

    /* Chained info: flags + chained triple. */
    {
        const uint8_t cc[] = { 0x01, 0x50 };
        uint32_t t = write_unwind(b, 0x80, W32_UNW_FLAG_CHAININFO, 2, 1,
                                  0, 0, cc);
        w32_(b, t, CODE_RVA);
        w32_(b, t + 4, CODE_RVA + 0x20);
        w32_(b, t + 8, XDATA_RVA);
        CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA + 0x80), 0);
    }

    /* Handler tail: EHANDLER + in-image handler RVA. */
    {
        const uint8_t hc[] = { 0x01, 0x50 };
        uint32_t t = write_unwind(b, 0xc0, W32_UNW_FLAG_EHANDLER, 2, 1,
                                  0, 0, hc);
        w32_(b, t, CODE_RVA + 0x100);   /* handler RVA */
        w32_(b, t + 4, 0);              /* scope count 0 (handler's own) */
        CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA + 0xc0), 0);
    }

    /* Hostile: version 2. */
    b[XDATA_RVA] = (uint8_t)(0x02 | 0);
    CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA), -1);
    b[XDATA_RVA] = W32_UNW_VERSION_1;

    /* Hostile: chained self-loop (hop cap). */
    {
        const uint8_t cc[] = { 0x01, 0x50 };
        uint32_t t = write_unwind(b, 0x100, W32_UNW_FLAG_CHAININFO, 2, 1,
                                  0, 0, cc);
        w32_(b, t, CODE_RVA);
        w32_(b, t + 4, CODE_RVA + 0x20);
        w32_(b, t + 8, XDATA_RVA + 0x100);   /* points at itself */
        CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA + 0x100),
                 -1);
    }

    /* Hostile: handler RVA outside the image. */
    {
        const uint8_t hc[] = { 0x01, 0x50 };
        uint32_t t = write_unwind(b, 0x140, W32_UNW_FLAG_EHANDLER, 2, 1,
                                  0, 0, hc);
        w32_(b, t, IMG_SIZE + 0x100);
        CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA + 0x140),
                 -1);
    }

    /* Hostile: truncation (span cuts mid-codes). */
    CHECK_EQ(w32_seh_validate_chain(b, XDATA_RVA + 2, XDATA_RVA), -1);
    /* Hostile: unwind_rva outside. */
    CHECK_EQ(w32_seh_validate_chain(b, IMG_SIZE, IMG_SIZE), -1);
    CHECK_EQ(w32_seh_validate_chain(NULL, IMG_SIZE, XDATA_RVA), -1);
    CHECK_EQ(w32_seh_validate_chain(b, 0, XDATA_RVA), -1);
    free(b);
}

/* ---- fuzz corpus (the W32-2 sweep shape) ---------------------------------- */

static void test_fuzz_truncation(void) {
    uint8_t *good = make_image();
    const uint8_t codes[] = { 0x04, 0x32, 0x01, 0x50 };
    uint32_t t = write_unwind(good, 0, W32_UNW_FLAG_EHANDLER, 5, 2, 0, 0,
                              codes);
    w32_(good, t, CODE_RVA);
    set_pdata_size(good, 1);
    add_func(good, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    for (size_t len = 0; len <= IMG_SIZE; len += 7) {
        int rc = w32_seh_validate_chain(good, len, XDATA_RVA);
        CHECK(rc == 0 || rc == -1);
        if (rc == 0) {
            const w32_runtime_function_t *e =
                w32_seh_lookup(good + PDATA_RVA, 12, CODE_RVA + 4);
            CHECK(e != NULL);
        }
    }
    free(good);
}

static void test_fuzz_bitflips(void) {
    for (uint32_t byte = 0; byte < XDATA_RVA + 0x20; byte += 3) {
        for (int bit = 0; bit < 8; bit += 3) {
            uint8_t *b = make_image();
            const uint8_t codes[] = { 0x04, 0x32, 0x01, 0x50 };
            uint32_t t = write_unwind(b, 0, W32_UNW_FLAG_EHANDLER, 5, 2,
                                      0, 0, codes);
            w32_(b, t, CODE_RVA);
            b[byte] ^= (uint8_t)(1u << bit);
            CHECK(w32_seh_validate_chain(b, IMG_SIZE, XDATA_RVA) <= 0);
            free(b);
        }
    }
}

/* ---- RtlVirtualUnwind ops -------------------------------------------------- */

static uint64_t *mkstack(size_t nqwords) {
    uint64_t *s = (uint64_t *)calloc(nqwords, 8);
    if (!s) { printf("oom\n"); exit(1); }
    return s;
}

static void test_unwind_push_alloc(void) {
    uint8_t *b = make_image();
    uint64_t *st = mkstack(64);
    w32_context_t ctx;
    w32_runtime_function_t *e;
    void *hd = (void *)1;
    uint64_t est = 0;
    void *h;
    /* Prolog: push rbp; sub rsp,0x28.  Reversed codes: ALLOC_SMALL(4)
     * (0x28 = 4*8+8), PUSH rbp. */
    const uint8_t codes[] = { 0x04, 0x42, 0x01, 0x50 };
    uint64_t base;
    write_unwind(b, 0, 0, 5, 2, 0, 0, codes);
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    base = (uint64_t)(uintptr_t)b;
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    /* [saved rbp][ret], rsp 0x28 below the saved rbp. */
    st[10] = 0x1122334455667788ull;     /* saved rbp */
    st[11] = base + CODE_RVA + 0x100;   /* return address */
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + CODE_RVA + 8;
    ctx.rsp = (uint64_t)(uintptr_t)&st[10] - 0x28;
    ctx.rbp = 0xdead;

    e = RtlLookupFunctionEntry(ctx.rip, &base, NULL);
    CHECK(e != NULL);
    h = RtlVirtualUnwind(0, base, ctx.rip, e, &ctx, &hd, &est, NULL);
    CHECK(h == NULL && hd == NULL);
    CHECK_EQ(est, (uint64_t)(uintptr_t)&st[11]);
    CHECK_EQ(ctx.rbp, 0x1122334455667788ull);
    CHECK_EQ(ctx.rip, (uint64_t)(uintptr_t)b + CODE_RVA + 0x100);
    CHECK_EQ(ctx.rsp, (uint64_t)(uintptr_t)&st[12]);
    free(st);
    free(b);
}

static void test_unwind_prolog_skip(void) {
    uint8_t *b = make_image();
    uint64_t *st = mkstack(64);
    w32_context_t ctx;
    w32_runtime_function_t *e;
    void *hd = NULL;
    uint64_t est = 0;
    /* Same codes, but the fault struck at prolog offset 2 (after the
     * push, before the alloc): only the push undoes. */
    const uint8_t codes[] = { 0x04, 0x42, 0x01, 0x50 };
    uint64_t base;
    write_unwind(b, 0, 0, 5, 2, 0, 0, codes);
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    base = (uint64_t)(uintptr_t)b;
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    st[10] = 0x8877665544332211ull;     /* saved rbp */
    st[11] = base + CODE_RVA + 0x100;   /* ret */
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + CODE_RVA + 2;   /* prolog offset 2 */
    ctx.rsp = (uint64_t)(uintptr_t)&st[10];

    e = RtlLookupFunctionEntry(ctx.rip, &base, NULL);
    CHECK(e != NULL);
    (void)RtlVirtualUnwind(0, base, ctx.rip, e, &ctx, &hd, &est, NULL);
    CHECK_EQ(est, (uint64_t)(uintptr_t)&st[11]);
    CHECK_EQ(ctx.rbp, 0x8877665544332211ull);
    CHECK_EQ(ctx.rip, base + CODE_RVA + 0x100);
    CHECK_EQ(ctx.rsp, (uint64_t)(uintptr_t)&st[12]);
    free(st);
    free(b);
}

static void test_unwind_set_fpreg(void) {
    uint8_t *b = make_image();
    uint64_t *st = mkstack(64);
    w32_context_t ctx;
    w32_runtime_function_t *e;
    uint64_t est = 0;
    /* push rbp; mov rbp,rsp; sub rsp,0x20.  Codes: ALLOC_SMALL(3),
     * SET_FPREG, PUSH rbp.  fpreg=rbp, fpoff=0. */
    const uint8_t codes[] = { 0x06, 0x32, 0x04, 0x03, 0x01, 0x50 };
    uint64_t base;
    write_unwind(b, 0, 0, 7, 3, 5, 0, codes);
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    base = (uint64_t)(uintptr_t)b;
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    st[10] = (uint64_t)(uintptr_t)&st[20]; /* saved rbp */
    st[11] = base + CODE_RVA + 0x100;      /* ret */
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + CODE_RVA + 10;
    ctx.rsp = (uint64_t)(uintptr_t)&st[10] - 0x20;
    ctx.rbp = (uint64_t)(uintptr_t)&st[10];

    e = RtlLookupFunctionEntry(ctx.rip, &base, NULL);
    CHECK(e != NULL);
    (void)RtlVirtualUnwind(0, base, ctx.rip, e, &ctx, NULL, &est, NULL);
    /* ALLOC: rsp+=0x20; SET_FPREG: rsp=rbp; PUSH: rbp=[rsp], rsp+=8;
     * epilogue: rip=[rsp], rsp+=8. */
    CHECK_EQ(est, (uint64_t)(uintptr_t)&st[11]);
    CHECK_EQ(ctx.rbp, (uint64_t)(uintptr_t)&st[20]);
    CHECK_EQ(ctx.rsp, (uint64_t)(uintptr_t)&st[12]);
    free(st);
    free(b);
}

static void test_unwind_save_far_xmm(void) {
    uint8_t *b = make_image();
    uint64_t *st = mkstack(128);
    w32_context_t ctx;
    w32_runtime_function_t *e;
    uint64_t est = 0;
    /* save r12 at rsp+0x100 (far), save xmm8 at rsp+0x40.  No
     * pushes/allocs: rsp already points at the return address. */
    const uint8_t codes[] = {
        0x08, 0xc5, 0x00, 0x01, 0x00, 0x00, /* SAVE_NONVOL_FAR r12,+0x100 */
        0x04, 0x88, 0x04, 0x00,             /* SAVE_XMM128 xmm8,+0x40 */
    };
    uint64_t base;
    write_unwind(b, 0, 0, 9, 5, 0, 0, codes);
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    base = (uint64_t)(uintptr_t)b;
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + CODE_RVA + 12;
    ctx.rsp = (uint64_t)(uintptr_t)&st[0];
    st[0] = 0xaaaaaaaaaaaaaaaaull;      /* ret */
    *(uint64_t *)((uint8_t *)&st[0] + 0x100) = 0x1212121212121212ull;
    memcpy((uint8_t *)&st[0] + 0x40, "\x01\x02\x03\x04\x05\x06\x07\x08"
                                     "\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10", 16);
    e = RtlLookupFunctionEntry(ctx.rip, &base, NULL);
    CHECK(e != NULL);
    (void)RtlVirtualUnwind(0, base, ctx.rip, e, &ctx, NULL, &est, NULL);
    CHECK_EQ(ctx.r12, 0x1212121212121212ull);
    CHECK_EQ(ctx.xmm[8].low, 0x0807060504030201ull);
    CHECK_EQ(est, (uint64_t)(uintptr_t)&st[0]);
    CHECK_EQ(ctx.rip, 0xaaaaaaaaaaaaaaaaull);
    CHECK_EQ(ctx.rsp, (uint64_t)(uintptr_t)&st[1]);
    free(st);
    free(b);
}

static void test_unwind_machframe(void) {
    uint8_t *b = make_image();
    uint64_t *st = mkstack(64);
    w32_context_t ctx;
    w32_runtime_function_t *e;
    uint64_t est = 0;
    /* PUSH_MACHFRAME without error code. */
    const uint8_t codes[] = { 0x01, 0x0a };
    uint64_t base;
    write_unwind(b, 0, 0, 2, 1, 0, 0, codes);
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    base = (uint64_t)(uintptr_t)b;
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    st[5] = base + CODE_RVA + 0x50;  /* trapped RIP */
    st[6] = 0x33;                    /* CS */
    st[7] = 0x246;                   /* EFLAGS */
    st[8] = (uint64_t)(uintptr_t)&st[40]; /* trapped RSP */
    st[9] = 0x2b;                    /* SS */
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + CODE_RVA + 4;
    ctx.rsp = (uint64_t)(uintptr_t)&st[5];

    e = RtlLookupFunctionEntry(ctx.rip, &base, NULL);
    CHECK(e != NULL);
    (void)RtlVirtualUnwind(0, base, ctx.rip, e, &ctx, NULL, &est, NULL);
    CHECK_EQ(est, (uint64_t)(uintptr_t)&st[5]);
    CHECK_EQ(ctx.rip, base + CODE_RVA + 0x50);
    CHECK_EQ(ctx.rsp, (uint64_t)(uintptr_t)&st[40]);
    CHECK_EQ(ctx.eflags, 0x246u);
    free(st);
    free(b);
}

static void test_unwind_alloc_large(void) {
    uint8_t *b = make_image();
    uint64_t *st = mkstack(128);
    w32_context_t ctx;
    w32_runtime_function_t *e;
    uint64_t est = 0;
    /* ALLOC_LARGE info=1: 32-bit size 0x120. */
    const uint8_t codes[] = { 0x04, 0x11, 0x20, 0x01, 0x00, 0x00 };
    uint64_t base;
    write_unwind(b, 0, 0, 5, 3, 0, 0, codes);
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    base = (uint64_t)(uintptr_t)b;
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    st[40] = base + CODE_RVA + 0x100;   /* ret */
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + CODE_RVA + 8;
    ctx.rsp = (uint64_t)(uintptr_t)&st[40] - 0x120;
    e = RtlLookupFunctionEntry(ctx.rip, &base, NULL);
    CHECK(e != NULL);
    (void)RtlVirtualUnwind(0, base, ctx.rip, e, &ctx, NULL, &est, NULL);
    CHECK_EQ(est, (uint64_t)(uintptr_t)&st[40]);
    CHECK_EQ(ctx.rsp, (uint64_t)(uintptr_t)&st[41]);
    free(st);
    free(b);
}

static void test_unwind_handler_data(void) {
    uint8_t *b = make_image();
    uint64_t *st = mkstack(64);
    w32_context_t ctx;
    w32_runtime_function_t *e;
    void *hd = NULL;
    uint64_t est = 0;
    void *h;
    const uint8_t codes[] = { 0x01, 0x50 };
    uint64_t base, tail;
    tail = write_unwind(b, 0, W32_UNW_FLAG_EHANDLER, 2, 1, 0, 0, codes);
    w32_(b, (uint32_t)tail, CODE_RVA + 0x300);   /* handler RVA */
    w32_(b, (uint32_t)tail + 4, 0x12345678);     /* handler data word */
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    base = (uint64_t)(uintptr_t)b;
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    st[10] = 0x99;                       /* saved rbp */
    st[11] = base + CODE_RVA + 0x100;   /* ret */
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + CODE_RVA + 4;
    ctx.rsp = (uint64_t)(uintptr_t)&st[10];
    e = RtlLookupFunctionEntry(ctx.rip, &base, NULL);
    CHECK(e != NULL);
    h = RtlVirtualUnwind(0, base, ctx.rip, e, &ctx, &hd, &est, NULL);
    CHECK_EQ((uint64_t)(uintptr_t)h, base + CODE_RVA + 0x300);
    CHECK_EQ((uint64_t)(uintptr_t)hd, base + tail + 4);
    CHECK_EQ(*(uint32_t *)hd, 0x12345678u);
    free(st);
    free(b);
}

/* ---- module queries ------------------------------------------------------- */

static void test_module_queries(void) {
    uint8_t *b = make_image();
    uint64_t base = (uint64_t)(uintptr_t)b;
    uint64_t ob = 0;
    void *ib = NULL;
    void *fh;
    set_pdata_size(b, 1);
    add_func(b, 0, CODE_RVA, CODE_RVA + 0x20, XDATA_RVA);
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    fh = RtlPcToFileHeader((void *)(uintptr_t)(base + CODE_RVA + 1), &ib);
    CHECK_EQ((uint64_t)(uintptr_t)fh, base);
    CHECK_EQ((uint64_t)(uintptr_t)ib, base);
    fh = RtlPcToFileHeader((void *)(uintptr_t)(base + IMG_SIZE + 8), &ib);
    CHECK(fh == NULL);

    {
        w32_runtime_function_t *e =
            RtlLookupFunctionEntry(base + CODE_RVA + 1, &ob, NULL);
        CHECK(e != NULL && ob == base);
        e = RtlLookupFunctionEntry(base + CODE_RVA + 0x40, &ob, NULL);
        CHECK(e == NULL);
        e = RtlLookupFunctionEntry(base + IMG_SIZE + 8, &ob, NULL);
        CHECK(e == NULL);
    }
    free(b);
}

/* ---- RtlCaptureContext ----------------------------------------------------- */

static void test_capture_context(void) {
    w32_context_t ctx;
    struct { uint64_t lo, hi; } pat = { 0x1122334455667788ull,
                                        (uint64_t)0x8877665544332211ull };
    memset(&ctx, 0, sizeof(ctx));
    __asm__ volatile("movdqu %0, %%xmm7" :: "m"(pat) : "xmm7");
    RtlCaptureContext(&ctx);
    CHECK_EQ(ctx.xmm[7].low, pat.lo);
    CHECK_EQ((uint64_t)ctx.xmm[7].high, pat.hi);
    CHECK(ctx.rip != 0);
    CHECK_EQ(ctx.context_flags, 0x10001fu);
    {
        uint64_t live;
        __asm__ volatile("mov %%rsp, %0" : "=r"(live));
        /* The captured RSP is the caller's AT THE CALL: below live by
         * the MS ABI shadow space (32) plus alignment -- close, below,
         * and 16-aligned. */
        CHECK(ctx.rsp <= live && live - ctx.rsp < 128);
        CHECK(ctx.rsp % 16 == 0);
    }
}

/* ---- live funclets --------------------------------------------------------- */

static int filt_calls, clean_calls;
static int32_t filt_ret = 1;    /* what the filter returns */
static uint64_t clean_last_est;
static int clean_order[8];
static int clean_n;
static int clean_tag;           /* set before each unwind-pass call */

static int32_t W32ABI t_filter(w32_exception_pointers_t *eptrs,
                               uint64_t establisher) {
    (void)eptrs; (void)establisher;
    filt_calls++;
    return filt_ret;
}

static void W32ABI t_cleanup(uint64_t establisher) {
    clean_calls++;
    clean_last_est = establisher;
    if (clean_n < 8)
        clean_order[clean_n++] = clean_tag;
}

/* A recording personality: cleanups on the unwind pass, RtlUnwind to the
 * recorded target on the first pass. */
static int pers_calls;
static uint64_t pers_target_ip;
static uint32_t pers_saw_flags;
static int pers_first_pass_handles;

static int32_t W32ABI t_personality(w32_exception_record_t *record,
                                    uint64_t establisher_frame,
                                    w32_context_t *context,
                                    w32_dispatcher_context_t *dispatch) {
    (void)context; (void)dispatch;
    pers_calls++;
    pers_saw_flags = record->flags;
    if (record->flags & W32_EXCEPTION_UNWINDING) {
        /* Unwind pass: record the cleanup (the tag rides in the
         * handler-data word the test planted). */
        clean_calls++;
        clean_last_est = establisher_frame;
        if (dispatch && dispatch->handler_data)
            clean_tag = *(int *)dispatch->handler_data;
        if (clean_n < 8)
            clean_order[clean_n++] = clean_tag;
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    }
    if (!pers_first_pass_handles)
        return W32_EXCEPTION_CONTINUE_SEARCH_NT;
    /* First pass: handle it -- unwind to the recorded target. */
    RtlUnwind((void *)(uintptr_t)establisher_frame,
              (void *)(uintptr_t)pers_target_ip, record, NULL);
    return W32_EXCEPTION_CONTINUE_SEARCH_NT;    /* NOTREACHED */
}

static void test_c_specific_search(void) {
    /* One frame, one cleanup scope: skipped on the first pass, the
     * handler returns SEARCH and no funclet ran. */
    uint8_t *b = make_image();
    uint64_t base = (uint64_t)(uintptr_t)b;
    w32_exception_record_t rec;
    w32_context_t ctx;
    w32_dispatcher_context_t dc;
    uint32_t tail;
    int32_t d;
    const uint8_t codes[] = { 0x01, 0x50 };
    tail = write_unwind(b, 0, W32_UNW_FLAG_EHANDLER, 2, 1, 0, 0, codes);
    w32_(b, (uint32_t)tail, CODE_RVA);   /* handler (unused here) */
    w32_(b, (uint32_t)tail + 4, 1);      /* scope count */
    w32_(b, (uint32_t)tail + 8, CODE_RVA);
    w32_(b, (uint32_t)tail + 12, CODE_RVA + 0x20);
    w32_(b, (uint32_t)tail + 16, W32_SCOPE_CLEANUP);
    w32_(b, (uint32_t)tail + 20, CODE_RVA + 0x10);
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    memset(&rec, 0, sizeof(rec));
    memset(&ctx, 0, sizeof(ctx));
    memset(&dc, 0, sizeof(dc));
    dc.control_pc = base + CODE_RVA + 4;
    dc.image_base = base;
    dc.handler_data = (void *)(uintptr_t)(base + tail + 4);
    filt_calls = 0;
    d = __C_specific_handler(&rec, base + 0x900, &ctx, &dc);
    CHECK_EQ(d, W32_EXCEPTION_CONTINUE_SEARCH_NT);
    CHECK_EQ(filt_calls, 0);
    free(b);
}

/* Distinct cleanup funclets for the order test. */
static void W32ABI t_cleanup_a(uint64_t e) {
    (void)e; if (clean_n < 8) clean_order[clean_n++] = 'a';
}
static void W32ABI t_cleanup_b(uint64_t e) {
    (void)e; if (clean_n < 8) clean_order[clean_n++] = 'b';
}
static void W32ABI t_cleanup_c(uint64_t e) {
    (void)e; if (clean_n < 8) clean_order[clean_n++] = 'c';
}

static void test_c_specific_cleanup_order(void) {
    uint8_t *b = make_ximage();
    uint64_t base = (uint64_t)(uintptr_t)b;
    w32_exception_record_t rec;
    w32_context_t ctx;
    w32_dispatcher_context_t dc;
    uint32_t tail, sa, sb, sc;
    int32_t d;
    const uint8_t codes[] = { 0x01, 0x50 };
    sa = emit_stub(b, 0x100, (void *)t_cleanup_a);
    sb = emit_stub(b, 0x110, (void *)t_cleanup_b);
    sc = emit_stub(b, 0x120, (void *)t_cleanup_c);
    tail = write_unwind(b, 0, W32_UNW_FLAG_EHANDLER, 2, 1, 0, 0, codes);
    w32_(b, (uint32_t)tail, sa);
    w32_(b, (uint32_t)tail + 4, 3);
    /* Listed outermost-first: a covers all, b the middle, c the pc. */
    w32_(b, (uint32_t)tail + 8, CODE_RVA);
    w32_(b, (uint32_t)tail + 12, CODE_RVA + 0x20);
    w32_(b, (uint32_t)tail + 16, W32_SCOPE_CLEANUP);
    w32_(b, (uint32_t)tail + 20, sa);
    w32_(b, (uint32_t)tail + 24, CODE_RVA);
    w32_(b, (uint32_t)tail + 28, CODE_RVA + 0x20);
    w32_(b, (uint32_t)tail + 32, W32_SCOPE_CLEANUP);
    w32_(b, (uint32_t)tail + 36, sb);
    w32_(b, (uint32_t)tail + 40, CODE_RVA);
    w32_(b, (uint32_t)tail + 44, CODE_RVA + 0x20);
    w32_(b, (uint32_t)tail + 48, W32_SCOPE_CLEANUP);
    w32_(b, (uint32_t)tail + 52, sc);
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    memset(&rec, 0, sizeof(rec));
    rec.flags = W32_EXCEPTION_UNWINDING;
    memset(&ctx, 0, sizeof(ctx));
    memset(&dc, 0, sizeof(dc));
    dc.control_pc = base + CODE_RVA + 4;
    dc.image_base = base;
    dc.handler_data = (void *)(uintptr_t)(base + tail + 4);
    clean_n = 0;
    d = __C_specific_handler(&rec, base + 0x900, &ctx, &dc);
    CHECK_EQ(d, W32_EXCEPTION_CONTINUE_SEARCH_NT);
    CHECK_EQ(clean_n, 3);
    /* Innermost-first: listed a,b,c -- ran c,b,a. */
    CHECK(clean_n == 3 && clean_order[0] == 'c' &&
          clean_order[1] == 'b' && clean_order[2] == 'a');
    free_ximage(b);
}

/* Full dispatch: fault in a leaf, one plain frame, one __C_specific_handler
 * frame whose filter EXECUTEs.  The unwind runs the middle frame's
 * cleanup and resumes at JumpTarget with rsp == establisher. */
static void test_dispatch_exec(void) {
    uint8_t *b = make_ximage();
    uint64_t base = (uint64_t)(uintptr_t)b;
    uint64_t *st = mkstack(128);
    struct w32_seh_dispatch *f;
    w32_exception_record_t rec;
    w32_context_t ctx;
    const uint8_t codes[] = { 0x01, 0x50 }; /* push rbp */
    uint32_t inner = CODE_RVA;              /* leaf: NO .pdata entry */
    uint32_t mid = CODE_RVA + 0x40, out = CODE_RVA + 0x80;
    uint32_t tmid, tout, hstub, fstub, cstub;
    uint64_t mid_est, out_est;

    hstub = emit_stub(b, 0x200, (void *)__C_specific_handler);
    fstub = emit_stub(b, 0x210, (void *)t_filter);
    cstub = emit_stub(b, 0x220, (void *)t_cleanup);

    /* Mid: UHANDLER personality (t_personality via stub) + tag word. */
    {
        uint32_t pstub = emit_stub(b, 0x230, (void *)t_personality);
        tmid = write_unwind(b, 0, W32_UNW_FLAG_UHANDLER, 2, 1, 0, 0,
                            codes);
        w32_(b, tmid, pstub);
        w32_(b, tmid + 4, 0x4d4944);     /* "MID" tag for the recorder */
    }
    /* Out: __C_specific_handler + one filter scope. */
    tout = write_unwind(b, 0x40, W32_UNW_FLAG_EHANDLER, 2, 1, 0, 0, codes);
    w32_(b, tout, hstub);
    w32_(b, tout + 4, 1);                /* scope count */
    w32_(b, tout + 8, out);              /* scope covers the frame */
    w32_(b, tout + 12, out + 0x20);
    w32_(b, tout + 16, fstub);           /* filter */
    w32_(b, tout + 20, CODE_RVA + 0x300); /* JumpTarget (asserted) */
    set_pdata_size(b, 2);
    add_func(b, 0, mid, mid + 0x20, XDATA_RVA);
    add_func(b, 1, out, out + 0x20, XDATA_RVA + 0x40);
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    /* Stack (ascending): inner[ret] | mid[rbp,ret] | out[rbp,ret].
     * Inner is a leaf: no .pdata, rsp points straight at its ret. */
    st[20] = base + mid + 4;        /* inner ret -> mid */
    st[21] = 0xbbbbbbbbbbbbbbbbull; /* mid saved rbp */
    st[22] = base + out + 4;        /* mid ret -> out */
    st[23] = 0xccccccccccccccccull; /* out saved rbp */
    st[24] = base + 0xe00;          /* out ret (in-image leaf pc) */
    mid_est = (uint64_t)(uintptr_t)&st[22];
    out_est = (uint64_t)(uintptr_t)&st[24];

    f = (struct w32_seh_dispatch *)w32_seh_frame_new();
    w32_seh_test_set_frame(f);
    memset(&rec, 0, sizeof(rec));
    rec.code = W32_EXCEPTION_INT_DIVIDE_BY_ZERO;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rip = base + inner + 4;
    ctx.rsp = (uint64_t)(uintptr_t)&st[20];
    f->live = ctx;
    f->live_valid = 1;
    f->state = W32_SEH_ST_ACTIVE;
    pers_calls = 0;
    pers_first_pass_handles = 0;    /* mid searches; out's filter decides */
    filt_calls = 0;
    filt_ret = 1;   /* EXECUTE */
    clean_calls = 0;
    clean_n = 0;
    pers_target_ip = 0; /* unused here (the scope drives RtlUnwind) */
    (void)cstub;

    if (setjmp(f->jb) == 0) {
        int r = seh_first_pass(f, &rec, &ctx);
        CHECK(0);   /* NOTREACHED (the filter EXECUTEs via RtlUnwind) */
        (void)r;
    } else {
        /* Resumed at JumpTarget with the outer frame intact. */
        CHECK_EQ(f->resume_valid, 1);
        CHECK_EQ(f->resume.rip, base + CODE_RVA + 0x300);
        CHECK_EQ(f->resume.rsp, out_est - 8); /* out live rsp */
        CHECK_EQ(filt_calls, 1);
        CHECK_EQ(pers_calls, 2);    /* mid: first pass + unwind pass */
        /* Mid's cleanup ran on the way out (tag "MID"). */
        CHECK_EQ(clean_calls, 1);
        CHECK(clean_n == 1 && clean_order[0] == 0x4d4944);
        CHECK_EQ(clean_last_est, mid_est);
        CHECK((pers_saw_flags & W32_EXCEPTION_UNWINDING) != 0);
    }
    w32_seh_test_set_frame(NULL);
    w32_seh_frame_free(f);
    free(st);
    free_ximage(b);
}

/* RtlUnwindEx to a mid-stack target: stops AT the target (its handler
 * does not run), resumes at TargetIp on the target's LIVE rsp. */
static void test_unwind_with_cleanup(void) {
    uint8_t *b = make_image();
    uint64_t base = (uint64_t)(uintptr_t)b;
    uint64_t *st = mkstack(128);
    struct w32_seh_dispatch *f;
    w32_exception_record_t rec;
    const uint8_t codes[] = { 0x01, 0x50 }; /* push rbp */
    uint64_t inner = CODE_RVA, mid = CODE_RVA + 0x20, out = CODE_RVA + 0x40;

    write_unwind(b, 0, 0, 2, 1, 0, 0, codes);
    set_pdata_size(b, 3);
    add_func(b, 0, (uint32_t)inner, (uint32_t)inner + 0x20, XDATA_RVA);
    add_func(b, 1, (uint32_t)mid, (uint32_t)mid + 0x20, XDATA_RVA);
    add_func(b, 2, (uint32_t)out, (uint32_t)out + 0x20, XDATA_RVA);
    w32_seh_test_reset();
    w32_seh_test_add_image(base, IMG_SIZE);

    /* Ascending: inner[rbp,ret] | mid[rbp,ret] | out[rbp,ret]. */
    st[30] = 0xcccc;                /* inner rbp */
    st[31] = base + mid + 4;        /* inner ret */
    st[32] = 0xbbbb;                /* mid rbp */
    st[33] = base + out + 4;        /* mid ret */
    st[34] = 0xaaaa;                /* out rbp */
    st[35] = base + 0xe00;          /* out ret (in-image leaf pc) */

    f = (struct w32_seh_dispatch *)w32_seh_frame_new();
    w32_seh_test_set_frame(f);
    memset(&rec, 0, sizeof(rec));
    rec.code = 0xE0000001u;
    memset(&f->live, 0, sizeof(f->live));
    f->live.rip = base + inner + 4;
    f->live.rsp = (uint64_t)(uintptr_t)&st[30];
    f->live_valid = 1;
    f->state = W32_SEH_ST_ACTIVE;
    if (setjmp(f->jb) == 0) {
        /* Unwind to mid's establisher (= &st[33]) with a target ip. */
        RtlUnwindEx((void *)(uintptr_t)&st[33],
                    (void *)(uintptr_t)(base + 0x700),
                    &rec, NULL, NULL, NULL);
        CHECK(0);   /* NOTREACHED */
    } else {
        CHECK_EQ(f->resume_valid, 1);
        CHECK_EQ(f->resume.rip, base + 0x700);
        CHECK_EQ(f->resume.rsp, (uint64_t)(uintptr_t)&st[32]); /* mid live */
    }
    w32_seh_test_set_frame(NULL);
    w32_seh_frame_free(f);
    free(st);
    free(b);
}

/* ---- RaiseException record + bottom --------------------------------------- */

static void test_raise_bottom(void) {
    /* No handlers anywhere: host RaiseException returns after recording
     * (the guest would die in seh_unhandled; the gate covers that). */
    struct w32_seh_dispatch *f;
    uint64_t args[2] = { 0x1111, 0x2222 };
    w32_seh_test_reset();
    f = (struct w32_seh_dispatch *)w32_seh_frame_new();
    w32_seh_test_set_frame(f);
    RaiseException(0xE0000001u, 0, 2, args);
    /* After return the dispatch closed (depth back to 0). */
    CHECK_EQ(f->depth, 0u);
    CHECK((f->state & W32_SEH_ST_ACTIVE) == 0);
    w32_seh_test_set_frame(NULL);
    w32_seh_frame_free(f);
}

static void test_xcpt_filter(void) {
    CHECK_EQ(_XcptFilter(W32_EXCEPTION_ACCESS_VIOLATION, NULL), 1);
    CHECK_EQ(_XcptFilter(W32_EXCEPTION_INT_DIVIDE_BY_ZERO, NULL), 1);
    CHECK_EQ(_XcptFilter(W32_EXCEPTION_BREAKPOINT, NULL), 1);
    CHECK_EQ(_XcptFilter(0xE0000001u, NULL), 0);
    {
        w32_exception_pointers_t ep;
        memset(&ep, 0, sizeof(ep));
        CHECK_EQ(UnhandledExceptionFilter(&ep), 0);  /* no filter set */
    }
}

int main(void) {
    RUN(test_lookup);
    RUN(test_validate_chain);
    RUN(test_fuzz_truncation);
    RUN(test_fuzz_bitflips);
    RUN(test_unwind_push_alloc);
    RUN(test_unwind_prolog_skip);
    RUN(test_unwind_set_fpreg);
    RUN(test_unwind_save_far_xmm);
    RUN(test_unwind_machframe);
    RUN(test_unwind_alloc_large);
    RUN(test_unwind_handler_data);
    RUN(test_module_queries);
    RUN(test_capture_context);
    RUN(test_c_specific_search);
    RUN(test_c_specific_cleanup_order);
    RUN(test_dispatch_exec);
    RUN(test_unwind_with_cleanup);
    RUN(test_raise_bottom);
    RUN(test_xcpt_filter);

    printf("%s: %d/%d tests passed\n", failed ? "FAIL" : "PASS",
           passed, tn);
    return failed ? 1 : 0;
}
