#ifndef AURALITE_MM_PAGE_CACHE_H
#define AURALITE_MM_PAGE_CACHE_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/* Minimal page cache for MAP_SHARED file-backed mappings. */
uint64_t page_cache_get(struct ofd *file, uint64_t offset);
void     page_cache_put(struct ofd *file, uint64_t offset, uint64_t phys);
int      page_cache_get_or_alloc(struct ofd *file, uint64_t offset,
                                 uint64_t *phys_out,
                                 void (*fill_fn)(uint64_t phys, void *arg),
                                 void *fill_arg);
/* A6: mark the page holding `offset` dirty so page_cache_flush() writes it
 * back.  Without this the `dirty` field was only ever cleared, never set,
 * so flush() had nothing to do and a MAP_SHARED store never reached disk. */
int      page_cache_mark_dirty(struct ofd *file, uint64_t offset);
/* A6: writeback for one page-aligned range; returns 0 or -1. */
int      page_cache_flush_range(struct ofd *file, uint64_t start, uint64_t end);
void     page_cache_invalidate(struct ofd *file);
void     page_cache_flush(struct ofd *file);

#endif /* AURALITE_MM_PAGE_CACHE_H */
