/* usercopy.c — small user pointer validation/copy helpers.
 *
 * These helpers are intentionally conservative.  They validate against the
 * current address space's page tables before the kernel dereferences a Ring 3
 * pointer, then use a tiny assembly copy primitive with a #PF fixup label.
 * If a mapping changes between validation and the actual copy, the exception
 * handler rewrites RIP to that fixup and the copy returns -1 instead of
 * panicking the kernel.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel/proc/usercopy.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/lib/string.h"

#define UACCESS_MAX_CPUS 64
volatile uint64_t uaccess_recover_ip_percpu[UACCESS_MAX_CPUS];
extern int64_t uaccess_copy_asm(void *dst, const void *src, uint64_t len);

static uint64_t uaccess_cpu_index(void) {
    if (!cpu_local_ready) return 0;
    struct cpu_local *c = get_cpu_local();
    if (!c || c->cpu_id >= UACCESS_MAX_CPUS) return 0;
    return c->cpu_id;
}

int usercopy_recover_fault(uint64_t *saved_rip) {
    uint64_t fixup = uaccess_recover_ip_percpu[uaccess_cpu_index()];
    if (!fixup || !saved_rip) return 0;
    *saved_rip = fixup;
    return 1;
}

static int user_range_bounds(uint64_t start, uint64_t len, uint64_t *end_out) {
    if (len == 0) {
        if (end_out) *end_out = start;
        return 1;
    }
    /* Reserve the first page so NULL and near-NULL pointers are never valid. */
    if (start < PAGE_SIZE_BYTES) return 0;
    uint64_t last = start + len - 1;
    if (last < start) return 0;          /* overflow */
    if (last >= USER_VADDR_TOP) return 0;
    if (end_out) *end_out = last;
    return 1;
}

int validate_user_range(const void *user_ptr, uint64_t len, int write_required) {
    uint64_t start = (uint64_t)(uintptr_t)user_ptr;
    uint64_t last;
    if (!user_range_bounds(start, len, &last)) return 0;
    if (len == 0) return 1;

    uint64_t page = start & ~(PAGE_SIZE_BYTES - 1ULL);
    uint64_t last_page = last & ~(PAGE_SIZE_BYTES - 1ULL);
    for (;;) {
        uint64_t flags = paging_get_flags(page);
        if (!(flags & PAGE_FLAG_PRESENT)) return 0;
        if (!(flags & PAGE_FLAG_USER)) return 0;
        if (write_required && (flags & PAGE_FLAG_COW) && !(flags & PAGE_FLAG_WRITABLE)) {
            if (!paging_handle_cow_fault(page, 0x3)) return 0;
            flags = paging_get_flags(page);
        }
        if (write_required && !(flags & PAGE_FLAG_WRITABLE)) return 0;
        if (page == last_page) break;
        page += PAGE_SIZE_BYTES;
        if (page == 0) return 0;         /* defensive overflow guard */
    }
    return 1;
}

int copy_from_user(void *kernel_dst, const void *user_src, uint64_t len) {
    if (len == 0) return 0;
    if (!kernel_dst) return -1;
    if (!validate_user_range(user_src, len, 0)) return -1;
    uint64_t irqf = user_access_begin();
    int64_t r = uaccess_copy_asm(kernel_dst, user_src, len);
    user_access_end(irqf);
    return r == 0 ? 0 : -1;
}

int copy_to_user(void *user_dst, const void *kernel_src, uint64_t len) {
    if (len == 0) return 0;
    if (!kernel_src) return -1;
    if (!validate_user_range(user_dst, len, 1)) return -1;
    uint64_t irqf = user_access_begin();
    int64_t r = uaccess_copy_asm(user_dst, kernel_src, len);
    user_access_end(irqf);
    return r == 0 ? 0 : -1;
}

int copy_string_from_user(char *kernel_dst, const char *user_src,
                          uint64_t kernel_dst_size) {
    if (!kernel_dst || !user_src || kernel_dst_size == 0) return -1;

    for (uint64_t i = 0; i < kernel_dst_size; i++) {
        char c;
        if (copy_from_user(&c, user_src + i, 1) != 0) {
            kernel_dst[0] = 0;
            return -1;
        }
        kernel_dst[i] = c;
        if (c == '\0') return 0;
    }

    kernel_dst[kernel_dst_size - 1] = 0;
    return -1;
}
