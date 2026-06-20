#ifndef NOVOS_LIMINE_REQUESTS_H
#define NOVOS_LIMINE_REQUESTS_H

#include <stdint.h>

/*
 * Bridge between the kernel and the Limine boot protocol.
 *
 * The request structs are declared in limine_requests.c with Limine's marker
 * protocol; Limine fills their .response fields at boot. These accessors hand
 * the resolved data to the rest of the kernel.
 */

struct limine_framebuffer;

/* First available framebuffer, or NULL if the request failed / none exist. */
struct limine_framebuffer *limine_get_framebuffer(void);

/* Total bytes of LIMINE_MEMMAP_USABLE memory, or 0 if unavailable. */
uint64_t limine_get_usable_memory(void);

/* Higher-Half Direct Map offset, or 0 if unavailable. */
uint64_t limine_get_hhdm_offset(void);

/* Non-zero if the requested Limine base revision is supported by the loader. */
int limine_base_revision_supported(void);

#endif /* NOVOS_LIMINE_REQUESTS_H */
