/* limine_requests.c — declare the Limine boot-protocol requests.
 *
 * Layout rules enforced here + in kernel.ld:
 *   1. Start marker, request(s), end marker appear in this exact order.
 *   2. All live in a loaded, WRITABLE segment (.data), because Limine writes
 *      the resolved response pointer back into each request struct.
 *   3. `used` + `volatile` prevent the compiler from dropping or caching them.
 */

#include <stddef.h>
#include "limine/limine.h"

/* ---- Start marker: 4 qwords (resets the scanner's request list) ---- */
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

/* ---- Base revision: request revision 3 (Limine 12.3.3 supports up to 6) ---- */
__attribute__((used, section(".limine_requests")))
static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(3);

/* ---- Framebuffer: linear framebuffer (VBE/GOP) for on-screen text ---- */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

/* ---- Memory map: physical RAM regions for the PMM (Phase 3) ---- */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id       = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

/* ---- HHDM: a direct map of all physical memory at a fixed offset ---- */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id       = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

/* ---- End marker: 2 qwords (stops the scanner) ---- */
__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

/* -------------------------------------------------------------------------- */

struct limine_framebuffer *limine_get_framebuffer(void) {
    volatile struct limine_framebuffer_response *r = framebuffer_request.response;
    if (r == NULL || r->framebuffer_count < 1) {
        return NULL;
    }
    return r->framebuffers[0];
}

uint64_t limine_get_usable_memory(void) {
    volatile struct limine_memmap_response *r = memmap_request.response;
    if (r == NULL) {
        return 0;
    }
    uint64_t total = 0;
    for (uint64_t i = 0; i < r->entry_count; i++) {
        if (r->entries[i]->type == LIMINE_MEMMAP_USABLE) {
            total += r->entries[i]->length;
        }
    }
    return total;
}

struct limine_memmap_entry **limine_get_memmap(uint64_t *out_count) {
    volatile struct limine_memmap_response *r = memmap_request.response;
    if (r == NULL) {
        if (out_count) {
            *out_count = 0;
        }
        return NULL;
    }
    if (out_count) {
        *out_count = r->entry_count;
    }
    /* Reading a member of a volatile-qualified struct yields the member's
       declared (non-volatile) type, so this needs no qualifier-discarding cast. */
    return r->entries;
}

uint64_t limine_get_hhdm_offset(void) {
    volatile struct limine_hhdm_response *r = hhdm_request.response;
    if (r == NULL) {
        return 0;
    }
    return r->offset;
}

int limine_base_revision_supported(void) {
    return LIMINE_BASE_REVISION_SUPPORTED(base_revision);
}
