#ifndef AURALITE_MM_KHEAP_H
#define AURALITE_MM_KHEAP_H

#include <stdint.h>

/*
 * Kernel heap: a thin wrapper over the generic allocator (heap.c) that backs
 * it with physical frames from the PMM, mapped into a fixed kernel virtual
 * region by the VMM. Pages are committed on demand as the heap grows.
 */

/* Kernel heap lives at 0xFFFFFFFF88000000 and may grow to 64 MiB.
 *
 * Why 64 MiB and not the original 16 MiB: a single full-screen GUI
 * framebuffer back-buffer plus the software 3D z-buffer already cost
 * ~2 * width * height * 4 bytes (e.g. ~8 MiB combined at 1280x800) and are
 * allocated once at boot and held for the whole session (see
 * drivers/framebuffer/graphics.c and drivers/framebuffer/render3d.c). With
 * only 16 MiB total, that left so little headroom that opening more than a
 * couple of GUI windows made kmalloc() start failing ("[gui] create_window:
 * kmalloc failed ..."), producing windows with no content back buffer (blank
 * windows) and, for spawned helper processes, ENOMEM while reading their ELF
 * image into kernel memory. Pages are only committed on demand
 * (kheap_expand()), so raising this ceiling costs nothing unless the heap
 * genuinely needs the room. Keep this comfortably below
 * THREAD_STACK_REGION_BASE (see kernel/proc/thread.c) so heap growth can
 * never collide with the kernel thread stack region. */
#define KHEAP_BASE  0xFFFFFFFF88000000ULL
#define KHEAP_LIMIT 0x04000000ULL            /* 64 MiB */

void  kheap_init(void);

/* Standard kernel allocation interface. NULL on OOM. */
void *kmalloc(uint64_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, uint64_t size);

/* Print heap statistics (committed / used / free) to the kernel console. */
void kheap_dump(void);

/* Gate self-test: 10 000 alloc/free cycles with corruption + leak checks. */
void kheap_self_test(void);

#endif /* AURALITE_MM_KHEAP_H */
