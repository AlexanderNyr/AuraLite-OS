#ifndef AURALITE_MM_SLAB_H
#define AURALITE_MM_SLAB_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/lib/spinlock.h"

struct slab_free_node {
    struct slab_free_node *next;
};

struct slab_page {
    struct slab_page *next;
};

typedef struct slab_cache {
    spinlock_t             lock;
    const char            *name;
    uint64_t               object_size;
    uint64_t               align;
    struct slab_free_node *free_list;
    struct slab_page      *page_list;
    uint64_t               objects_allocated;
    uint64_t               objects_active;
} slab_cache_t;

extern slab_cache_t *tcb_cache;
extern slab_cache_t *ofd_cache;
extern slab_cache_t *vnode_cache;

void          slab_init(void);
slab_cache_t *slab_create(const char *name, uint64_t object_size, uint64_t align);
void         *slab_alloc(slab_cache_t *cache);
void          slab_free(slab_cache_t *cache, void *obj);

#endif /* AURALITE_MM_SLAB_H */