#ifndef _SEARCH_H
#define _SEARCH_H

#include <stddef.h>

typedef struct entry {
    char *key;
    void *data;
} ENTRY;

typedef enum { FIND, ENTER } ACTION;

typedef enum { preorder, postorder, endorder, leaf } VISIT;

/* Hash table */
ENTRY *hsearch(ENTRY item, ACTION action);
int    hcreate(size_t nel);
void   hdestroy(void);

/* Binary tree */
void  *tsearch(const void *key, void **rootp,
               int (*compar)(const void *, const void *));
void  *tfind(const void *key, void *const *rootp,
             int (*compar)(const void *, const void *));
void  *tdelete(const void *key, void **rootp,
               int (*compar)(const void *, const void *));
void   twalk(const void *root,
             void (*action)(const void *, VISIT, int));

/* Linear search */
void  *lsearch(const void *key, void *base, size_t *nelp, size_t width,
               int (*compar)(const void *, const void *));
void  *lfind(const void *key, const void *base, size_t *nelp, size_t width,
             int (*compar)(const void *, const void *));

#endif /* _SEARCH_H */
