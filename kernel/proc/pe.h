#ifndef AURALITE_PROC_PE_H
#define AURALITE_PROC_PE_H

#include <stdint.h>

/*
 * PE32+ image loader — WIN32_PLAN.md phase W32-3.
 *
 * The kernel-side counterpart of w32/src/w32_pe.c.  Structure decoding lives
 * in that file and is exercised by a host unit test with a fuzz corpus
 * (WIN32_PLAN.md D2); this file does only what must happen in Ring 0: map
 * sections, copy their bytes, apply base relocations and derive final page
 * protections.
 *
 * It is deliberately shaped like kernel/proc/elf.c — same page-flag
 * derivation, same "zero a new frame before exposing it to user space", same
 * merge rule for pages two sections share.  Where the two differ, PE is the
 * more forgiving format: an image carries its own relocations, so unlike an
 * ELF at a fixed p_vaddr it can be moved when its preferred base is taken.
 */

/* Load a PE32+ image from memory into the CURRENT address space.
 *
 * @param image      raw file bytes
 * @param size       image size
 * @param out_brk    if non-NULL, receives the end of the highest mapped page
 * @param out_base   if non-NULL, receives the base the image was loaded at,
 *                   which is not necessarily its preferred ImageBase
 * @returns the entry-point virtual address, or 0 on failure.
 *
 * On failure nothing is guaranteed about partial mappings: the caller is
 * expected to tear the address space down, exactly as it does for elf_load().
 */
uint64_t pe_load(const void *image, uint64_t size, uint64_t *out_brk,
                 uint64_t *out_base);

/* Cheap sniff used by the exec path to choose a loader.  Only looks at the
 * first two bytes, so it is safe on any buffer of at least that length. */
int pe_image_probe(const void *image, uint64_t size);

#endif /* AURALITE_PROC_PE_H */
