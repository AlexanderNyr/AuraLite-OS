#ifndef AURALITE_MM_PAGE_CACHE_H
#define AURALITE_MM_PAGE_CACHE_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/* Minimal page cache for MAP_SHARED file-backed mappings. */
uint64_t page_cache_get(struct ofd *file, uint64_t offset);
void     page_cache_put(struct ofd *file, uint64_t offset, uint64_t phys);
void     page_cache_invalidate(struct ofd *file);
void     page_cache_flush(struct ofd *file);

#endif /* AURALITE_MM_PAGE_CACHE_H */
