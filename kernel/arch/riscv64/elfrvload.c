/* kernel/arch/riscv64/elfrvload.c -- ELF64/EM_RISCV loader
 * (RISCV_PLAN V5).  elf32load.c's shape at the third width; see the
 * header for the three-way refusal contract and the real-p_flags
 * story.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/riscv64/elfrvload.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"

#define ELFCLASS64 2
#define EM_RISCV   243
#define EM_386     3
#define EM_X86_64  62
#define ET_EXEC    2
#define PT_LOAD    1
#define PF_X       1
#define PF_W       2

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr;
    uint64_t p_filesz, p_memsz, p_align;
};

#define MAX_MAPPED 64
#define MAX_NEST   4
static uint64_t MV[MAX_MAPPED];
static uint64_t MF[MAX_MAPPED];
static uint32_t mapped_count;
static uint32_t marks[MAX_NEST];
static uint32_t mark_depth;

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}
static void put_hex_(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    sbi_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        sbi_putc(hex[(v >> shift) & 0xF]);
}

void elfrvload_mark(void)
{
    if (mark_depth < MAX_NEST)
        marks[mark_depth++] = mapped_count;
}

static int map_user_page(uint64_t vaddr, uint64_t flags)
{
    if (mapped_count >= MAX_MAPPED)
        return -1;
    uint64_t frame = pmm_rv_alloc_frame();
    if (!frame)
        return -1;
    /* Zero through the HHDM: BSS bytes and file-tail slack must not
     * leak a previous owner's data into user space. */
    uint64_t *p = (uint64_t *)p2v_rv(frame);
    for (int i = 0; i < 512; i++)
        p[i] = 0;
    if (paging_rv_map(vaddr, frame, flags) != 0) {
        pmm_rv_free_frame(frame);
        return -1;
    }
    MV[mapped_count] = vaddr;
    MF[mapped_count] = frame;
    mapped_count++;
    return 0;
}

uint64_t elfrvload_map(const uint8_t *image, uint64_t size)
{
    if (size < sizeof(struct elf64_ehdr))
        return 0;

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;

    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L'  && eh->e_ident[3] == 'F')) {
        sbi_puts("[elfrv] refused: bad magic\n");
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS64) {
        sbi_puts("[elfrv] refused: not ELFCLASS64 -- the i386 kernel's "
                 "binaries do not run here\n");
        return 0;
    }
    if (eh->e_machine != EM_RISCV) {
        sbi_puts("[elfrv] refused: machine ");
        put_udec_(eh->e_machine);
        sbi_puts(eh->e_machine == EM_X86_64
                 ? " (x86_64 -- wrong kernel, right family)\n"
                 : " (not EM_RISCV)\n");
        return 0;
    }
    if (eh->e_type != ET_EXEC) {
        sbi_puts("[elfrv] refused: not ET_EXEC (PIE arrives with the "
                 "full libc port)\n");
        return 0;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        sbi_puts("[elfrv] refused: phdr table out of bounds\n");
        return 0;
    }

    uint32_t base = mapped_count;   /* refusal rewinds to here */

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)
            (image + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;

        if (ph->p_vaddr < ELF_RV_USER_MIN ||
            ph->p_vaddr + ph->p_memsz > ELF_RV_USER_MAX ||
            ph->p_offset + ph->p_filesz > size) {
            sbi_puts("[elfrv] refused: segment outside the user window\n");
            goto refuse;
        }

        /* p_flags -> REAL PTE bits.  PF_X excludes W; PF_W excludes
         * X; both absent still gives R (a data segment must be
         * readable to be worth loading).  A segment claiming W AND X
         * together is refused outright -- this loader will not build
         * the PTE V3 promised never to build. */
        uint64_t flags = PTE_U | PTE_R;
        if ((ph->p_flags & PF_W) && (ph->p_flags & PF_X)) {
            sbi_puts("[elfrv] refused: W+X segment (W^X is enforced "
                     "on this arch)\n");
            goto refuse;
        }
        if (ph->p_flags & PF_X)
            flags |= PTE_X;
        if (ph->p_flags & PF_W)
            flags |= PTE_W;

        uint64_t first = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE_RV - 1);
        uint64_t last  = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE_RV - 1)
                         & ~(uint64_t)(PAGE_SIZE_RV - 1);
        for (uint64_t va = first; va < last; va += PAGE_SIZE_RV) {
            uint32_t already = 0;
            for (uint32_t m = 0; m < mapped_count; m++)
                if (MV[m] == va) { already = 1; break; }
            if (!already && map_user_page(va, flags) != 0)
                goto refuse;
        }

        /* Copy file bytes through the HHDM (pages were zeroed, so
         * memsz > filesz BSS is already correct). */
        for (uint64_t off = 0; off < ph->p_filesz; off++) {
            uint64_t va    = ph->p_vaddr + off;
            uint64_t frame = 0;
            for (uint32_t m = 0; m < mapped_count; m++)
                if (MV[m] == (va & ~(uint64_t)(PAGE_SIZE_RV - 1)))
                    { frame = MF[m]; break; }
            ((uint8_t *)p2v_rv(frame))[va & (PAGE_SIZE_RV - 1)] =
                image[ph->p_offset + off];
        }
    }

    sbi_puts("[elfrv] mapped ");
    put_udec_(mapped_count - base);
    sbi_puts(" user page(s), entry ");
    put_hex_(eh->e_entry);
    sbi_puts(" (p_flags honoured: PF_X->RX, PF_W->RW)\n");
    return eh->e_entry;

refuse:
    while (mapped_count > base) {
        mapped_count--;
        paging_rv_unmap(MV[mapped_count]);
        pmm_rv_free_frame(MF[mapped_count]);
    }
    return 0;
}

void elfrvload_unmap(void)
{
    uint32_t floor = mark_depth ? marks[--mark_depth] : 0;
    while (mapped_count > floor) {
        mapped_count--;
        paging_rv_unmap(MV[mapped_count]);
        pmm_rv_free_frame(MF[mapped_count]);
    }
}
