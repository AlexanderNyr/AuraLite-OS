/* kernel/arch/riscv64/kheap_rv.c -- first-fit kernel heap (RISCV_PLAN V3).
 *
 * The kheap32 design at LP64 (the plan said so; it holds): a fixed
 * virtual window above the HHDM, pages committed on demand through
 * paging_rv_map + pmm_rv_alloc_frame, a doubly-linked free list with
 * split-on-alloc and coalesce-on-free, magic-tagged headers, and the
 * same boot self-test contract (N alloc/free cycles, no corruption,
 * no leak).  Pointer-width differences end at the types: uint64_t
 * sizes, 16-byte alignment (the LP64 psABI's malloc rule).
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/riscv64/kheap_rv.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"

#define HEAP_MAGIC_FREE  0xF7EEF7EEF7EEF7EEUL
#define HEAP_MAGIC_USED  0xA110CA7EA110CA7EUL
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

struct block {
    uint64_t      magic;
    uint64_t      size;          /* payload bytes */
    struct block *next;
    struct block *prev;
};

static struct block *head;
static uint64_t committed_top;   /* first uncommitted virt addr */
static uint64_t used_bytes;

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}

static int commit_through(uint64_t needed_top)
{
    while (committed_top < needed_top) {
        if (committed_top >= KHEAP_RV_BASE + KHEAP_RV_SIZE)
            return -1;                          /* window exhausted */
        uint64_t frame = pmm_rv_alloc_frame();
        if (!frame)
            return -1;
        if (paging_rv_map(committed_top, frame, PTE_R | PTE_W) != 0)
            return -1;
        committed_top += PAGE_SIZE_RV;
    }
    return 0;
}

void kheap_rv_init(void)
{
    committed_top = KHEAP_RV_BASE;
    if (commit_through(KHEAP_RV_BASE + PAGE_SIZE_RV) != 0) {
        sbi_puts("[heap] init FAILED: cannot commit the first page\n");
        return;
    }
    head = (struct block *)KHEAP_RV_BASE;
    head->magic = HEAP_MAGIC_FREE;
    head->size  = PAGE_SIZE_RV - sizeof(struct block);
    head->next  = 0;
    head->prev  = 0;
    used_bytes  = 0;

    sbi_puts("[heap] window at ");
    sbi_puts("0xffffffe000000000, 64 MiB, committed on demand\n");
}

/* Split b when the remainder can hold a header + 16 bytes, mark it
 * used, return its payload. */
static void *take_block(struct block *b, uint64_t need);

void *kmalloc_rv(size_t size)
{
    if (size == 0 || !head)
        return 0;
    uint64_t need = ALIGN_UP(size, 16);
    struct block *last = head;

    for (struct block *b = head; b; last = b, b = b->next) {
        if (b->magic != HEAP_MAGIC_FREE)
            continue;
        if (b->size < need) {
            /* A free TAIL block can grow by committing pages. */
            if (!b->next) {
                uint64_t want_end = (uint64_t)(b + 1) + need;
                if (commit_through(ALIGN_UP(want_end, PAGE_SIZE_RV)) == 0)
                    b->size = committed_top - (uint64_t)(b + 1);
            }
            if (b->size < need)
                continue;
        }
        return take_block(b, need);
    }

    /* Nothing fit and the tail block is USED: append a fresh block at
     * the committed edge (the invariant "last block ends exactly at
     * committed_top" makes the address unambiguous).  This branch was
     * MISSING at first writing -- the self-test's alloc-heavy phase
     * found it on the host build before QEMU ever saw it. */
    uint64_t hdr = (uint64_t)(last + 1) + last->size;
    if (commit_through(ALIGN_UP(hdr + sizeof(struct block) + need,
                                PAGE_SIZE_RV)) != 0)
        return 0;
    struct block *nb = (struct block *)hdr;
    nb->magic = HEAP_MAGIC_FREE;
    nb->size  = committed_top - hdr - sizeof(struct block);
    nb->next  = 0;
    nb->prev  = last;
    last->next = nb;
    return take_block(nb, need);
}

static void *take_block(struct block *b, uint64_t need)
{
    /* Split when the remainder can hold a header + 16 bytes. */
    if (b->size >= need + sizeof(struct block) + 16) {
        struct block *rest =
            (struct block *)((uint8_t *)(b + 1) + need);
        rest->magic = HEAP_MAGIC_FREE;
        rest->size  = b->size - need - sizeof(struct block);
        rest->next  = b->next;
        rest->prev  = b;
        if (b->next)
            b->next->prev = rest;
        b->next = rest;
        b->size = need;
    }
    b->magic = HEAP_MAGIC_USED;
    used_bytes += b->size;
    return b + 1;
}

void kfree_rv(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    if (b->magic != HEAP_MAGIC_USED) {
        sbi_puts("[heap] kfree_rv: BAD MAGIC (double free or corruption)\n");
        return;
    }
    b->magic = HEAP_MAGIC_FREE;
    used_bytes -= b->size;

    /* Coalesce forward, then backward. */
    if (b->next && b->next->magic == HEAP_MAGIC_FREE &&
        (uint8_t *)(b + 1) + b->size == (uint8_t *)b->next) {
        b->size += sizeof(struct block) + b->next->size;
        b->next  = b->next->next;
        if (b->next)
            b->next->prev = b;
    }
    if (b->prev && b->prev->magic == HEAP_MAGIC_FREE &&
        (uint8_t *)(b->prev + 1) + b->prev->size == (uint8_t *)b) {
        b->prev->size += sizeof(struct block) + b->size;
        b->prev->next  = b->next;
        if (b->next)
            b->next->prev = b->prev;
    }
}

/* The [heap] gate: kheap32's contract -- cycles of varied sizes,
 * pattern-checked, everything freed, zero bytes leaked. */
int kheap_rv_selftest(void)
{
    enum { CYCLES = 64, SLOTS = 8 };
    void *slot[SLOTS] = { 0 };
    uint64_t before = used_bytes;

    sbi_puts("[heap] self-test: ");
    put_udec_(CYCLES);
    sbi_puts(" alloc/free cycles...\n");

    for (int c = 0; c < CYCLES; c++) {
        int i = c % SLOTS;
        if (slot[i]) {
            /* Verify the fill pattern survived neighbours. */
            uint8_t *q = (uint8_t *)slot[i];
            for (int k = 0; k < 32; k++)
                if (q[k] != (uint8_t)(i * 41 + k))
                    return -1;
            kfree_rv(slot[i]);
            slot[i] = 0;
        } else {
            size_t sz = 24 + (size_t)(c * 97 % 4000);
            slot[i] = kmalloc_rv(sz);
            if (!slot[i])
                return -1;
            uint8_t *q = (uint8_t *)slot[i];
            for (int k = 0; k < 32; k++)
                q[k] = (uint8_t)(i * 41 + k);
        }
    }
    for (int i = 0; i < SLOTS; i++)
        if (slot[i])
            kfree_rv(slot[i]);

    return used_bytes == before ? 0 : -1;
}
