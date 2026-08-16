/* kernel/arch/i386/kheap32.c -- first-fit kernel heap (I386_PLAN I3).
 *
 * Mirrors kernel/mm/kheap.c's design at bring-up scope: a fixed
 * virtual window ([0xF0000000, +64 MiB), above the direct map), pages
 * committed on demand through paging32_map + pmm32_alloc_frame, a
 * doubly-linked free list with split-on-alloc and coalesce-on-free,
 * and the same boot self-test contract (N alloc/free cycles, no
 * corruption, no leak).
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/kheap32.h"
#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/pmm32.h"
#include "kernel/arch/i386/kprintf32.h"

#define HEAP_MAGIC_FREE  0xF7EEF7EEu
#define HEAP_MAGIC_USED  0xA110CA7Eu
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))

struct block {
    uint32_t      magic;
    uint32_t      size;          /* payload bytes */
    struct block *next;
    struct block *prev;
};

static struct block *head;
static uint32_t committed_top;   /* first uncommitted virt addr */
static uint32_t used_bytes;

static int commit_through(uint32_t needed_top)
{
    while (committed_top < needed_top) {
        if (committed_top >= KHEAP32_BASE + KHEAP32_SIZE)
            return -1;                          /* window exhausted */
        uint32_t frame = pmm32_alloc_frame();
        if (!frame)
            return -1;
        if (paging32_map(committed_top, frame, PAGE32_FLAG_WRITE) != 0)
            return -1;
        committed_top += PAGE_SIZE_32;
    }
    return 0;
}

void kheap32_init(void)
{
    committed_top = KHEAP32_BASE;
    used_bytes    = 0;

    if (commit_through(KHEAP32_BASE + PAGE_SIZE_32) != 0) {
        kprintf32("[heap] FAIL: could not commit the first page\n");
        head = NULL;
        return;
    }

    head        = (struct block *)KHEAP32_BASE;
    head->magic = HEAP_MAGIC_FREE;
    head->size  = KHEAP32_SIZE - sizeof(struct block);
    head->next  = NULL;
    head->prev  = NULL;

    kprintf32("[heap] window %x (%u MiB), committed on demand\n",
              KHEAP32_BASE, KHEAP32_SIZE / (1024 * 1024));
}

void *kmalloc32(size_t size)
{
    if (!head || size == 0)
        return NULL;
    size = ALIGN_UP(size, 16);

    for (struct block *b = head; b; b = b->next) {
        if (b->magic != HEAP_MAGIC_FREE || b->size < size)
            continue;

        /* Commit the pages this block's header+payload will touch. */
        uint32_t need_top = (uint32_t)b + sizeof(struct block) + (uint32_t)size;
        /* A split writes a header just past the payload. */
        if (b->size >= size + sizeof(struct block) + 16)
            need_top += sizeof(struct block);
        if (commit_through(ALIGN_UP(need_top, PAGE_SIZE_32)) != 0)
            return NULL;

        /* Split when the remainder can hold a header + minimal payload. */
        if (b->size >= size + sizeof(struct block) + 16) {
            struct block *rest = (struct block *)
                ((uint32_t)b + sizeof(struct block) + size);
            rest->magic = HEAP_MAGIC_FREE;
            rest->size  = b->size - (uint32_t)size - sizeof(struct block);
            rest->next  = b->next;
            rest->prev  = b;
            if (b->next)
                b->next->prev = rest;
            b->next = rest;
            b->size = (uint32_t)size;
        }

        b->magic    = HEAP_MAGIC_USED;
        used_bytes += b->size;
        return (void *)((uint32_t)b + sizeof(struct block));
    }
    return NULL;
}

void kfree32(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)((uint32_t)ptr - sizeof(struct block));
    if (b->magic != HEAP_MAGIC_USED) {
        kprintf32("[heap] kfree32: BAD MAGIC at %p -- corruption or "
                  "double free\n", ptr);
        return;
    }

    b->magic    = HEAP_MAGIC_FREE;
    used_bytes -= b->size;

    /* Coalesce with the next block, then the previous. */
    if (b->next && b->next->magic == HEAP_MAGIC_FREE &&
        (uint32_t)b + sizeof(struct block) + b->size == (uint32_t)b->next) {
        b->size += sizeof(struct block) + b->next->size;
        b->next  = b->next->next;
        if (b->next)
            b->next->prev = b;
    }
    if (b->prev && b->prev->magic == HEAP_MAGIC_FREE &&
        (uint32_t)b->prev + sizeof(struct block) + b->prev->size == (uint32_t)b) {
        b->prev->size += sizeof(struct block) + b->size;
        b->prev->next  = b->next;
        if (b->next)
            b->next->prev = b->prev;
    }
}

/* Same contract as kheap's boot self-test: cycles of varied-size
 * alloc/free with a write pattern, then a no-leak check. */
#define ST_CYCLES 10000
#define ST_SLOTS  16

int kheap32_selftest(void)
{
    static void *slot[ST_SLOTS];
    uint32_t used_before = used_bytes;

    kprintf32("[heap] self-test: %u alloc/free cycles...\n", (uint32_t)ST_CYCLES);

    for (int i = 0; i < ST_CYCLES; i++) {
        int s = i % ST_SLOTS;
        if (slot[s]) {
            /* Verify the pattern the earlier cycle wrote. */
            uint8_t *p = (uint8_t *)slot[s];
            if (p[0] != (uint8_t)s || p[7] != (uint8_t)~s)
                return -1;
            kfree32(slot[s]);
            slot[s] = NULL;
        } else {
            size_t sz = 16u + (uint32_t)(i * 37 % 4000);
            slot[s] = kmalloc32(sz);
            if (!slot[s])
                return -1;
            uint8_t *p = (uint8_t *)slot[s];
            p[0] = (uint8_t)s;
            p[7] = (uint8_t)~s;
        }
    }

    for (int s = 0; s < ST_SLOTS; s++) {
        if (slot[s]) {
            kfree32(slot[s]);
            slot[s] = NULL;
        }
    }

    if (used_bytes != used_before)
        return -1;                       /* leak */

    kprintf32("[heap] PASS: %u cycles, no corruption, no leak\n",
              (uint32_t)ST_CYCLES);
    return 0;
}
