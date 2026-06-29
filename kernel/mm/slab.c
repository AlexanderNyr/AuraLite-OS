/* kernel/mm/slab.c — Slab Allocator (H6) */

#include "kernel/mm/slab.h"

#ifdef ARCH_X86_64
#include "kernel/mm/kheap.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/proc/thread.h"
#include "kernel/fs/vfs.h"
#else
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "kernel/lib/spinlock.h"
#define kmalloc(s) malloc(s)
#define kfree(p)   free(p)
#define kprintf    printf
#define spinlock_init(l)             (void)(l)
#define spinlock_acquire_irqsave(l)  0
#define spinlock_release_irqrestore(l, f) (void)(l); (void)(f)
#endif

slab_cache_t *tcb_cache   = NULL;
slab_cache_t *ofd_cache   = NULL;
slab_cache_t *vnode_cache = NULL;

#define SLAB_PAGE_SIZE 4096

slab_cache_t *slab_create(const char *name, uint64_t object_size, uint64_t align) {
    if (object_size == 0) return NULL;
    if (align < 8) align = 8;
    
    /* Align object size up to the required alignment */
    object_size = (object_size + align - 1) & ~(align - 1);
    
    slab_cache_t *c = kmalloc(sizeof(slab_cache_t));
    if (!c) return NULL;
    memset(c, 0, sizeof(slab_cache_t));
    spinlock_init(&c->lock);
    c->name = name;
    c->object_size = object_size;
    c->align = align;
    c->free_list = NULL;
    c->page_list = NULL;
    c->objects_allocated = 0;
    c->objects_active = 0;
    return c;
}

void *slab_alloc(slab_cache_t *cache) {
    if (!cache) return NULL;
    uint64_t flags = spinlock_acquire_irqsave(&cache->lock);
    
    if (!cache->free_list) {
        /* Allocate a new slab page */
        uint8_t *page = kmalloc(SLAB_PAGE_SIZE);
        if (!page) {
            spinlock_release_irqrestore(&cache->lock, flags);
            return NULL;
        }
        
        struct slab_page *sp = (struct slab_page *)page;
        sp->next = cache->page_list;
        cache->page_list = sp;
        
        uint64_t start = ((uint64_t)(uintptr_t)(page + sizeof(struct slab_page)) + cache->align - 1) & ~(cache->align - 1);
        uint64_t end = (uint64_t)(uintptr_t)page + SLAB_PAGE_SIZE;
        
        while (start + cache->object_size <= end) {
            struct slab_free_node *node = (struct slab_free_node *)(uintptr_t)start;
            node->next = cache->free_list;
            cache->free_list = node;
            cache->objects_allocated++;
            start += cache->object_size;
        }
    }
    
    struct slab_free_node *obj = cache->free_list;
    if (obj) {
        cache->free_list = obj->next;
        cache->objects_active++;
    }
    
    spinlock_release_irqrestore(&cache->lock, flags);
    if (obj) memset(obj, 0, cache->object_size);
    return obj;
}

void slab_free(slab_cache_t *cache, void *obj) {
    if (!cache || !obj) return;
    uint64_t flags = spinlock_acquire_irqsave(&cache->lock);
    
    struct slab_free_node *node = (struct slab_free_node *)obj;
    node->next = cache->free_list;
    cache->free_list = node;
    cache->objects_active--;
    
    spinlock_release_irqrestore(&cache->lock, flags);
}

void slab_init(void) {
#ifdef ARCH_X86_64
    tcb_cache   = slab_create("tcb_cache", sizeof(tcb_t), 16);
    ofd_cache   = slab_create("ofd_cache", sizeof(struct ofd), 16);
    vnode_cache = slab_create("vnode_cache", sizeof(struct vnode), 16);
    kprintf("[slab] initialized tcb_cache, ofd_cache, vnode_cache\n");
#endif
}
