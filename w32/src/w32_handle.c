/* w32_handle.c — the HANDLE table.  WIN32_PLAN.md phase W32-4.
 *
 * W32A-2 grows the slot from fd-only to fd-or-payload: find sessions, file
 * mappings, process objects and change notifications share the one handle
 * space (one forgery rule) with an opaque payload instead of an fd.
 */

#include "w32/w32_handle.h"

#include <stdint.h>

struct slot {
    int in_use;
    int kind;
    int fd;
    int closable;
    void *obj;
    void (*free_obj)(void *);
};

static struct slot table[W32_HANDLE_MAX];

/* Handles are minted as (index + BIAS) so that:
 *   - no valid handle is ever NULL, which many callers treat as failure;
 *   - no valid handle is (HANDLE)-1, the CreateFile failure sentinel;
 *   - a forged small integer (1, 2, 3...) does not resolve, because the bias
 *     puts real handles well away from the values a program might invent.
 * The bias is arbitrary but must stay clear of the negative std selectors. */
#define HANDLE_BIAS 0x100

static W32_HANDLE index_to_handle(int i) {
    return (W32_HANDLE)(intptr_t)(HANDLE_BIAS + i);
}

static int handle_to_index(W32_HANDLE h) {
    intptr_t v = (intptr_t)h;
    if (v < HANDLE_BIAS) return -1;
    intptr_t i = v - HANDLE_BIAS;
    if (i >= W32_HANDLE_MAX) return -1;
    return (int)i;
}

static void clear_slot(int i) {
    table[i].in_use = 0;
    table[i].kind = W32_HANDLE_KIND_FD;
    table[i].fd = -1;
    table[i].closable = 0;
    table[i].obj = 0;
    table[i].free_obj = 0;
}

void w32_handle_init(void) {
    for (int i = 0; i < W32_HANDLE_MAX; i++) clear_slot(i);
    /* Slots 0..2 are the standard streams.  They are pre-bound and marked
     * non-closable: a program that calls CloseHandle(GetStdHandle(...)) must
     * not take stdout away from the rest of the process. */
    for (int i = 0; i < 3; i++) {
        table[i].in_use = 1;
        table[i].fd = i;
        table[i].closable = 0;
    }
}

W32_HANDLE w32_handle_alloc(int fd, int closable) {
    if (fd < 0) return (W32_HANDLE)0;
    for (int i = 3; i < W32_HANDLE_MAX; i++) {
        if (!table[i].in_use) {
            table[i].in_use = 1;
            table[i].kind = W32_HANDLE_KIND_FD;
            table[i].fd = fd;
            table[i].closable = closable ? 1 : 0;
            return index_to_handle(i);
        }
    }
    return (W32_HANDLE)0;              /* caller sets ERROR_TOO_MANY_OPEN_FILES */
}

W32_HANDLE w32_handle_alloc_obj(int kind, void *obj, void (*free_obj)(void *)) {
    if (!obj) return (W32_HANDLE)0;
    if (kind != W32_HANDLE_KIND_FIND && kind != W32_HANDLE_KIND_MAP &&
        kind != W32_HANDLE_KIND_PROC && kind != W32_HANDLE_KIND_CHANGE)
        return (W32_HANDLE)0;
    for (int i = 3; i < W32_HANDLE_MAX; i++) {
        if (!table[i].in_use) {
            table[i].in_use = 1;
            table[i].kind = kind;
            table[i].fd = -1;
            table[i].closable = 1;
            table[i].obj = obj;
            table[i].free_obj = free_obj;
            return index_to_handle(i);
        }
    }
    return (W32_HANDLE)0;
}

int w32_handle_to_fd(W32_HANDLE h) {
    intptr_t v = (intptr_t)h;

    /* The standard selectors are accepted directly, so a program that passes
     * STD_OUTPUT_HANDLE where a HANDLE is expected still works -- real Win32
     * programs do this.
     *
     * Compared on the low 32 bits only.  The selectors are DWORDs, so
     * (HANDLE)(intptr_t)STD_OUTPUT_HANDLE is 0x00000000FFFFFFF5 (zero-extended
     * from an unsigned 32-bit value), while (intptr_t)(int32_t)-11 would be
     * 0xFFFFFFFFFFFFFFF5.  Comparing full pointers makes those two disagree
     * and every std handle stop resolving -- which is exactly what the unit
     * test caught.  Masking compares what the value actually is. */
    uint32_t low = (uint32_t)(uintptr_t)v;
    if (low == (uint32_t)W32_STD_INPUT_HANDLE  && (v >> 32) <= 0) return 0;
    if (low == (uint32_t)W32_STD_OUTPUT_HANDLE && (v >> 32) <= 0) return 1;
    if (low == (uint32_t)W32_STD_ERROR_HANDLE  && (v >> 32) <= 0) return 2;

    int i = handle_to_index(h);
    if (i < 0) return -1;
    if (!table[i].in_use) return -1;
    return table[i].fd;   /* -1 for payload kinds: fd callers refuse them */
}

int w32_handle_kind(W32_HANDLE h) {
    int i = handle_to_index(h);
    if (i < 0) return -1;
    if (!table[i].in_use) return -1;
    return table[i].kind;
}

void *w32_handle_get_obj(W32_HANDLE h, int kind) {
    int i = handle_to_index(h);
    if (i < 0) return 0;
    if (!table[i].in_use) return 0;
    if (table[i].kind != kind) return 0;
    return table[i].obj;
}

void *w32_handle_release_obj(W32_HANDLE h, int kind) {
    int i = handle_to_index(h);
    if (i < 0) return 0;
    if (!table[i].in_use) return 0;
    if (table[i].kind != kind) return 0;
    void *obj = table[i].obj;
    clear_slot(i);
    return obj;
}

int w32_handle_release(W32_HANDLE h) {
    int i = handle_to_index(h);
    if (i < 0) return -1;
    if (!table[i].in_use) return -1;
    if (table[i].kind == W32_HANDLE_KIND_FIND ||
        table[i].kind == W32_HANDLE_KIND_CHANGE) {
        /* Win32 wants FindClose for these; CloseHandle refuses them. */
        return -1;
    }
    if (table[i].kind == W32_HANDLE_KIND_MAP ||
        table[i].kind == W32_HANDLE_KIND_PROC) {
        void *obj = table[i].obj;
        void (*free_obj)(void *) = table[i].free_obj;
        clear_slot(i);
        if (free_obj) free_obj(obj);
        return W32_HANDLE_FREED;
    }
    if (!table[i].closable) {
        /* A live but non-closable handle (a standard stream): CloseHandle
         * reports success and does nothing, which is what Win32 programs
         * expect and what keeps stdout alive. */
        return -1;
    }
    int fd = table[i].fd;
    clear_slot(i);
    return fd;
}

int w32_handle_live_count(void) {
    int n = 0;
    for (int i = 3; i < W32_HANDLE_MAX; i++) if (table[i].in_use) n++;
    return n;
}
