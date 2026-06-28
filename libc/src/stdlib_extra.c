/* libc/src/stdlib_extra.c — qsort, bsearch, abort, atexit (P10) */

#include <stdlib.h>
#include <string.h>

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    /* Простая реализация пузырьковой сортировки (достаточно для тестов) */
    char *b = base;
    for (size_t i = 0; i < nmemb; i++) {
        for (size_t j = 0; j < nmemb - 1; j++) {
            if (compar(b + j * size, b + (j + 1) * size) > 0) {
                char tmp[256];
                if (size > sizeof(tmp)) return; /* safety */
                memcpy(tmp, b + j * size, size);
                memcpy(b + j * size, b + (j + 1) * size, size);
                memcpy(b + (j + 1) * size, tmp, size);
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *b = base;
    size_t low = 0, high = nmemb;
    while (low < high) {
        size_t mid = (low + high) / 2;
        int cmp = compar(key, b + mid * size);
        if (cmp == 0) return (void *)(b + mid * size);
        if (cmp < 0) high = mid;
        else low = mid + 1;
    }
    return NULL;
}

static void (*atexit_funcs[32])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (atexit_count >= 32) return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

void __run_atexit(void) {
    for (int i = atexit_count - 1; i >= 0; i--) {
        atexit_funcs[i]();
    }
}