/* kernel/arch/i386/elf32load.c -- ELF32 user loader (I386_PLAN I5).
 *
 * RESIDUE2 T8 (RES-18): the loader also accepts ET_DYN -- a static
 * PIE.  Dynamic images are mapped at a dedicated PIE base and their
 * .rel.dyn R_386_RELATIVE entries are applied by hand (the kernel is
 * its own dynamic linker: add base to every relocated word).  The
 * receipt lives in tests/integration/i386_pie_smoke.sh.
 *
 * The validation order mirrors kernel/proc/elf.c: magic, class,
 * machine, type, then bounds-checked phdr walk.  Every PT_LOAD page
 * is allocated fresh, copied through the direct map, and mapped
 * user; p_flags PF_W maps writable -- and PF_X does NOT map
 * no-exec-otherwise, because there is no NX on non-PAE i386 (plan D3;
 * the loader is where the x86_64 kernel enforces it, so this is where
 * the i386 kernel says it cannot).
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/elf32load.h"
#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/pmm32.h"
#include "kernel/arch/i386/kprintf32.h"

#define EI_NIDENT 16
#define ELFCLASS32 1
#define EM_386     3
#define ET_EXEC    2
#define ET_DYN     3
#define PT_LOAD    1
#define PF_W       2
#define SHT_REL    9
#define R_386_RELATIVE 8

/* Where a PIE is seated.  Distinct from the ET_EXEC child base
 * (0x08048000) and the shell (0x30000000) on purpose: if relocation
 * were skipped, absolute pointers would aim at the link-time image
 * (base 0) and fault, not silently alias a live mapping. */
#define ELF32_PIE_BASE 0x10000000u

struct elf32_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
};

struct elf32_phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
};

struct elf32_shdr {
    uint32_t sh_name, sh_type, sh_flags, sh_addr;
    uint32_t sh_offset, sh_size, sh_link, sh_info;
    uint32_t sh_addralign, sh_entsize;
};

struct elf32_rel {
    uint32_t r_offset, r_info;
};

/* Track what we mapped so unmap can undo it.  I7 turned the single
 * image slot into a mark/release stack: SYS_SPAWN nests -- the shell
 * (itself a user image) traps into the kernel, and the spawn handler
 * runs the child image from inside the shell's syscall, on the same
 * kernel thread.  elf32load_mark() checkpoints the mapping list
 * before the child; elf32load_unmap() releases back to the newest
 * mark, leaving the parent's pages untouched.  The images' VIRTUAL
 * windows must not overlap -- the shell links at 0x30000000
 * (shell32.ld) so children at 0x08048000 fit beside it, and
 * paging32_map's already-mapped refusal is the backstop. */
#define MAX_MAPPED 64
#define MAX_NEST   4
static uint32_t MV[MAX_MAPPED];
static uint32_t MF[MAX_MAPPED];
static uint32_t mapped_count;
static uint32_t marks[MAX_NEST];
static uint32_t mark_depth;

void elf32load_mark(void)
{
    if (mark_depth < MAX_NEST)
        marks[mark_depth++] = mapped_count;
}

static int map_user_page(uint32_t vaddr, uint32_t flags)
{
    if (mapped_count >= MAX_MAPPED)
        return -1;
    uint32_t frame = pmm32_alloc_frame();
    if (!frame)
        return -1;
    /* Zero through the direct map: BSS bytes and file-tail slack must
     * not leak a previous owner's data into user space. */
    uint32_t *p = (uint32_t *)p2v_32(frame);
    for (int i = 0; i < 1024; i++)
        p[i] = 0;
    if (paging32_map(vaddr, frame, flags) != 0) {
        pmm32_free_frame(frame);
        return -1;
    }
    MV[mapped_count] = vaddr;
    MF[mapped_count] = frame;
    mapped_count++;
    return 0;
}

/* Frame backing a mapped user page (0 if not ours). */
static uint32_t frame_of(uint32_t va)
{
    va &= ~(PAGE_SIZE_32 - 1);
    for (uint32_t m = 0; m < mapped_count; m++)
        if (MV[m] == va)
            return MF[m];
    return 0;
}

/* Apply .rel.dyn R_386_RELATIVE relocations for an image seated at
 * `base` (RES-18: the kernel as its own dynamic linker).  Returns the
 * number applied, or -1 on a record pointing outside the image. */
static int pie_apply_relative(const uint8_t *image, uint32_t size, uint32_t base)
{
    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)image;
    if (eh->e_shoff == 0 ||
        eh->e_shoff + (uint32_t)eh->e_shnum * eh->e_shentsize > size)
        return -1;

    int applied = 0;
    for (uint16_t s = 0; s < eh->e_shnum; s++) {
        const struct elf32_shdr *sh = (const struct elf32_shdr *)
            (image + eh->e_shoff + (uint32_t)s * eh->e_shentsize);
        if (sh->sh_type != SHT_REL)
            continue;
        if (sh->sh_offset + sh->sh_size > size || sh->sh_entsize != 8)
            continue;
        for (uint32_t r = 0; r + 8 <= sh->sh_size; r += 8) {
            const struct elf32_rel *rel = (const struct elf32_rel *)
                (image + sh->sh_offset + r);
            if ((rel->r_info & 0xFF) != R_386_RELATIVE)
                continue;           /* only the static-PIE contract */
            uint32_t va = base + rel->r_offset;
            uint32_t frame = frame_of(va);
            if (!frame)
                return -1;          /* relocation target not mapped */
            volatile uint32_t *slot =
                (volatile uint32_t *)(p2v_32(frame) + (va & (PAGE_SIZE_32 - 1)));
            *slot += base;
            applied++;
        }
    }
    return applied;
}

uint32_t elf32load_map(const uint8_t *image, uint32_t size)
{
    if (size < sizeof(struct elf32_ehdr))
        return 0;

    const struct elf32_ehdr *eh = (const struct elf32_ehdr *)image;

    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L'  && eh->e_ident[3] == 'F')) {
        kprintf32("[elf32] refused: bad magic\n");
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS32) {
        kprintf32("[elf32] refused: not ELFCLASS32 (class=%u) -- the "
                  "64-bit kernel's binaries do not run here\n",
                  eh->e_ident[4]);
        return 0;
    }
    if (eh->e_machine != EM_386 ||
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)) {
        kprintf32("[elf32] refused: machine=%u type=%u\n",
                  eh->e_machine, eh->e_type);
        return 0;
    }
    /* RES-18: a PIE links at image base 0; seat it at the PIE base.
     * ET_EXEC keeps its absolute addresses (base 0, as before). */
    const uint32_t base = (eh->e_type == ET_DYN) ? ELF32_PIE_BASE : 0;
    if (eh->e_phoff + (uint32_t)eh->e_phnum * eh->e_phentsize > size) {
        kprintf32("[elf32] refused: phdr table out of bounds\n");
        return 0;
    }

    mapped_count = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf32_phdr *ph = (const struct elf32_phdr *)
            (image + eh->e_phoff + (uint32_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;

        uint32_t vaddr = ph->p_vaddr + base;
        if (vaddr < ELF32_USER_MIN ||
            vaddr + ph->p_memsz > ELF32_USER_MAX ||
            ph->p_offset + ph->p_filesz > size) {
            kprintf32("[elf32] refused: segment outside the user window\n");
            elf32load_unmap();
            return 0;
        }

        uint32_t flags = PAGE32_FLAG_USER |
                         ((ph->p_flags & PF_W) ? PAGE32_FLAG_WRITE : 0);
        /* No NX on this arch: a non-PF_X segment still executes if
         * jumped to.  Stated at load, enforced never (plan D3). */

        uint32_t first = vaddr & ~(PAGE_SIZE_32 - 1);
        uint32_t last  = (vaddr + ph->p_memsz + PAGE_SIZE_32 - 1)
                         & ~(PAGE_SIZE_32 - 1);
        for (uint32_t va = first; va < last; va += PAGE_SIZE_32) {
            /* Segments may share a page boundary; map once. */
            uint32_t already = 0;
            for (uint32_t m = 0; m < mapped_count; m++)
                if (MV[m] == va) { already = 1; break; }
            if (!already && map_user_page(va, flags) != 0) {
                elf32load_unmap();
                return 0;
            }
        }

        /* Copy file bytes through the direct map (pages were zeroed,
         * so memsz > filesz BSS is already correct). */
        for (uint32_t off = 0; off < ph->p_filesz; off++) {
            uint32_t va    = vaddr + off;
            uint32_t frame = frame_of(va);
            ((uint8_t *)p2v_32(frame))[va & (PAGE_SIZE_32 - 1)] =
                image[ph->p_offset + off];
        }
    }

    if (eh->e_type == ET_DYN) {
        int rel = pie_apply_relative(image, size, base);
        if (rel < 0) {
            kprintf32("[elf32] PIE refused: relocation outside image\n");
            elf32load_unmap();
            return 0;
        }
        /* kprintf32 has no %08x: plain %x keeps the receipt greppable. */
        kprintf32("[elf32] PIE: base 0x%x, %d R_386_RELATIVE applied, "
                  "entry 0x%x\n", base, rel, base + eh->e_entry);
        return base + eh->e_entry;
    }
    kprintf32("[elf32] mapped %u user page(s), entry %x\n",
              mapped_count, eh->e_entry);
    return eh->e_entry;
}

void elf32load_unmap(void)
{
    /* Release back to the newest mark (or to empty when unnested). */
    uint32_t floor = mark_depth ? marks[--mark_depth] : 0;
    for (uint32_t m = floor; m < mapped_count; m++) {
        paging32_unmap(MV[m]);
        pmm32_free_frame(MF[m]);
    }
    mapped_count = floor;
}
