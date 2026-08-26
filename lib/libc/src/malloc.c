#include "stdlib.h"
#include "unistd.h"
#include "string.h"
#include <stdint.h>
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

static block_meta *request_space(block_meta* last, size_t size) {
    block_meta *block;
    block = sbrk(0);
    void *request = sbrk(size + META_SIZE);
    if (request == (void*)-1) {
        return NULL;
    }
    
    if (last) {
        last->next = block;
    }
    block->size = size;
    block->next = NULL;
    block->free = 0;
    return block;
}

void *malloc(size_t size) {
    block_meta *block;
    if (size <= 0) return NULL;
    
    // align size
    size = (size + 7) & ~7;
    
    if (!base) {
        block = request_space(NULL, size);
        if (!block) return NULL;
        base = block;
    } else {
        block_meta *last = base;
        block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (!block) return NULL;
        } else {
            block->free = 0;
        }
    }
    
    return (block + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    block_meta *block_ptr = ((block_meta*)ptr) - 1;
    memset(ptr, 0, block_ptr->size);
    block_ptr->free = 1;
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
