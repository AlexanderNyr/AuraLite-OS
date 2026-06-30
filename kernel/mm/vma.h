#ifndef AURALITE_MM_VMA_H
#define AURALITE_MM_VMA_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/fs/vfs.h"

/* VMA flags */
#define VMA_ANON     (1 << 0)   /* anonymous (zero-fill on fault) */
#define VMA_FILE     (1 << 1)   /* file-backed (read from ofd on fault) */
#define VMA_SHARED   (1 << 2)   /* MAP_SHARED — share physical frames */
#define VMA_READ     (1 << 3)   /* PROT_READ */
#define VMA_WRITE    (1 << 4)   /* PROT_WRITE */
#define VMA_EXEC     (1 << 5)   /* PROT_EXEC */

typedef struct vma {
    uint64_t    va_start;   /* page-aligned, inclusive */
    uint64_t    va_end;     /* page-aligned, exclusive */
    uint32_t    flags;      /* VMA_* */
    struct ofd  *file;      /* file-backed fields (ignored for VMA_ANON) */
    uint64_t    file_off;   /* offset into file at va_start */
    struct vma  *next;      /* sorted list linkage */
} vma_t;

/* VMA lifecycle and management */
vma_t  *vma_alloc(void);
void    vma_free(vma_t *v);
vma_t  *vma_find(vma_t *list, uint64_t va);
int     vma_insert(vma_t **list_head, uint64_t start, uint64_t end,
                   uint32_t flags, struct ofd *file, uint64_t file_off);
void    vma_remove_range(vma_t **list_head, uint64_t start, uint64_t end);
void    vma_free_all(vma_t **list_head);
int     handle_user_page_fault(uint64_t cr2, uint64_t err_code);

#endif /* AURALITE_MM_VMA_H */
