#include "stdlib.h"
#include "unistd.h"
#include "string.h"
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
    size_t pad;   /* SELFHOST SH2: header is 32 bytes so every malloc()
                     payload is 16-byte aligned (max_align_t).  The old
                     24-byte header misaligned every block by 8, which is
                     fine for scalar code but faults (GP#0) the moment a
                     clang-built binary -- the guest tcc -- does a 16-byte
                     movaps on a malloc'd buffer.  sbrk() hands out
                     page-aligned chunks, so a 16-multiple header keeps
                     every payload 16-aligned for the life of the block. */
} block_meta;

#define META_SIZE sizeof(block_meta)
static block_meta *base = NULL;

static block_meta *find_free_block(block_meta **last, size_t size) {
    block_meta *current = base;
    while (current && !(current->free && current->size >= size)) {
        *last = current;
        current = current->next;
    }
    return current;
}

/* WR-3: chunked bump allocation.  The kernel services SYS_BRK in whole
 * pages and reports the ROUNDED break back, so a naive "sbrk(exact)"
 * loses up to 4095 bytes of tail padding on EVERY growth call: a chatty
 * allocator (Notepad++'s start-up makes a few hundred thousand small
 * HeapAllocs) silently burned ~4 KB of address space and physical memory
 * per block and died of OOM at ~1 GB of pure rounding waste.  Grabbing
 * 256 KB chunks and bump-allocating inside them turns the per-block waste
 * into per-chunk waste (1/64th of one page each).  The leftover tail of a
 * chunk that no longer fits the next request is dropped -- bounded by one
 * chunk per size class transition, negligible against the old behaviour. */
#define MALLOC_CHUNK (256ull << 10)

static volatile unsigned char rs_lock;

/* WR-3: the heap lock.  request_space() above serialises growth, but the
 * free-list half -- find_free_block() plus the free=0 mark in malloc(),
 * and the zero+free=1 in free() -- ran unlocked: two threads could check
 * out the same base block together, and one thread's free() then zeroed
 * live bytes out from under the other.  Under Notepad++'s allocator
 * chatter that mis-share was a matter of time, and the victim surfaced
 * anywhere (a wild memcpy length, a torn snprintf) far from the cause.
 * The lock nests heap -> growth only (request_space takes rs_lock, never
 * this one back), and malloc never takes the stdio lock -- its diagnosis
 * below writes with a raw write() -- so the global order stays acyclic. */
static volatile unsigned char heap_lock;

static void heap_take(void) {
    while (__atomic_test_and_set(&heap_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
}
static void heap_drop(void) { __atomic_clear(&heap_lock, __ATOMIC_RELEASE); }

static block_meta *request_space(block_meta* last, size_t size) {
    static unsigned char *cur = NULL, *end = NULL;
    size_t need = (size + META_SIZE + 15u) & ~(size_t)15u;
    unsigned char *p;

    (void)last;
    /* Growth moves shared bump pointers; NPP allocates from several
     * threads at once, so serialise the chunk grab (the free-list paths
     * stay as lock-free as they always were). */
    while (__atomic_test_and_set(&rs_lock, __ATOMIC_ACQUIRE))
        ;
    if (cur == NULL) {
        cur = (unsigned char *)sbrk(0);
        if (cur == (unsigned char *)-1) {
            cur = NULL;
            __atomic_clear(&rs_lock, __ATOMIC_RELEASE);
            return NULL;
        }
        end = cur;
    }
    if ((size_t)(end - cur) < need) {
        size_t chunk = need > MALLOC_CHUNK ? (need + 4095u) & ~(size_t)4095u
                                           : (size_t)MALLOC_CHUNK;
        unsigned char *nb = (unsigned char *)sbrk((intptr_t)chunk);
        if (nb == (unsigned char *)-1) {
            __atomic_clear(&rs_lock, __ATOMIC_RELEASE);
            return NULL;
        }
        cur = nb;
        end = nb + chunk;
        if ((size_t)(end - cur) < need) {
            __atomic_clear(&rs_lock, __ATOMIC_RELEASE);
            return NULL;
        }
    }
    p = cur;
    cur += need;
    block_meta *block = (block_meta *)p;
    block->size = size;
    block->next = NULL;
    block->free = 0;
    __atomic_clear(&rs_lock, __ATOMIC_RELEASE);
    return block;
}

void *malloc(size_t size) {
    block_meta *block;
    {
        static uint64_t mtrc_tot = 0, mtrc_calls = 0;
        mtrc_calls++;
        mtrc_tot += size;
        {
            static int mtrc_mid;
            if (size >= (32ull << 10) && size < (1ull << 20) && mtrc_mid < 400) {
                mtrc_mid++;
                {
                    char b[40]; int n = 0, k;
                    unsigned long ra2 = (unsigned long)__builtin_return_address(0);
                    b[n++] = 'M'; b[n++] = 'I'; b[n++] = 'D'; b[n++] = ' ';
                    for (k = 60; k >= 0; k -= 4) {
                        int d = (int)((ra2 >> k) & 0xF);
                        b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                    }
                    b[n++] = ' ';
                    { uint64_t sz2 = size;
                      for (k = 60; k >= 0; k -= 4) {
                        int d = (int)((sz2 >> k) & 0xF);
                        b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                      } }
                    b[n++] = '\n';
                    /* Raw write, never stdio: this runs inside malloc(),
                     * where taking the stdio lock would invert the global
                     * stdio -> heap order (fopen allocates), and where a
                     * FILE race would corrupt the very heap being built. */
                    (void)write(1, b, (size_t)n);
                }
            }
        }
        if (size >= (1ull << 20) || (mtrc_calls % 20000ull) == 0) {
            if (1) {
            unsigned long ra = (unsigned long)__builtin_return_address(0);
            char b[64]; int n = 0, k;
            b[n++] = 'M'; b[n++] = '6'; b[n++] = '4'; b[n++] = ' ';
            for (k = 60; k >= 0; k -= 4) {
                int d = (int)((ra >> k) & 0xF);
                b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            }
            b[n++] = ' ';
            for (k = 60; k >= 0; k -= 4) {
                uint64_t sz = size;
                int d = (int)((sz >> k) & 0xF);
                b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            }
            b[n++] = ' ';
            { uint64_t tt = mtrc_tot; int k2;
              for (k2 = 60; k2 >= 0; k2 -= 4) {
                int d = (int)((tt >> k2) & 0xF);
                b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
              } }
            b[n++] = '\n';
            (void)write(1, b, (size_t)n);   /* raw: see the MID site above */
            }
        }
    }
    if (size <= 0) return NULL;
    
    // align size
    size = (size + 7) & ~7;
    
    heap_take();
    if (base) {
        block_meta *last = base;
        block = find_free_block(&last, size);
        if (block) {
            block->free = 0;
            heap_drop();
            return (block + 1);
        }
    }
    heap_drop();
    /* Miss (or first call): grow outside the heap lock.  request_space()
     * carries its own lock and hands back a detached block, so no shared
     * list state is touched here; only the base publication below needs
     * the lock again. */
    block = request_space(NULL, size);
    if (!block) return NULL;
    heap_take();
    if (!base) base = block;
    heap_drop();

    return (block + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    block_meta *block_ptr = ((block_meta*)ptr) - 1;
    heap_take();
    memset(ptr, 0, block_ptr->size);
    block_ptr->free = 1;
    heap_drop();
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb && total / nmemb != size) return NULL;   /* overflow */
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    block_meta *block_ptr = ((block_meta *)ptr) - 1;
    if (block_ptr->size >= size) return ptr;   /* fits in place */

    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block_ptr->size);
    free(ptr);
    return new_ptr;
}

/* ---- POSIX.1-2024 aligned allocation ---- */

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!memptr) return EINVAL;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;
    if (size == 0) { *memptr = NULL; return 0; }
    /* Allocate extra space: alignment + sizeof(void*) header for the raw ptr. */
    void *raw = malloc(size + alignment - 1 + sizeof(void *));
    if (!raw) return ENOMEM;
    /* Align forward. */
    uintptr_t a = ((uintptr_t)raw + sizeof(void *) + alignment - 1)
                  & ~(uintptr_t)(alignment - 1);
    ((void **)a)[-1] = raw;   /* store original ptr just before aligned block */
    *memptr = (void *)a;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}
