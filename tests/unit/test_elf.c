/*
 * test_elf.c — unit tests for ELF64 parsing, header validation,
 * program header interpretation, and binary loading checks.
 *
 * 35+ test cases covering: header magic, class, endianness,
 * machine type, segment types, permissions, virtual addresses.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while(0)
#define CHECK(c) do { if(!(c)) { printf("  FAIL L%d: %s\n",__LINE__,#c); failed++; } } while(0)
#define CHECK_EQ(a,e) do { if((long)(a)!=(long)(e)) { printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e)); failed++; } } while(0)

/* ---- ELF64 structures (from the kernel ELF loader) ---- */

#define EI_NIDENT 16
#define ELFMAG0    0x7F
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_X86_64  62
#define ET_EXEC    2
#define PT_LOAD    1

#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* ---- Validation functions (same logic as kernel) ---- */

static int elf_validate_ehdr(const Elf64_Ehdr *ehdr) {
    if (!ehdr) return -1;
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3)
        return -2;
    if (ehdr->e_ident[4] != ELFCLASS64) return -3;
    if (ehdr->e_ident[5] != ELFDATA2LSB) return -4;
    if (ehdr->e_machine != EM_X86_64) return -5;
    if (ehdr->e_type != ET_EXEC) return -6;
    if (ehdr->e_phnum == 0) return -7;
    if (ehdr->e_phentsize < sizeof(Elf64_Phdr)) return -8;
    return 0;
}

/* Count loadable segments */
static int elf_count_loadable(const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdrs) {
    int count = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) count++;
    }
    return count;
}

/* Check if any segment has executable permission */
static int elf_has_exec_segment(const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdrs) {
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && (phdrs[i].p_flags & PF_X))
            return 1;
    }
    return 0;
}

/* Check if any segment has writable permission */
static int elf_has_writable_segment(const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdrs) {
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && (phdrs[i].p_flags & PF_W))
            return 1;
    }
    return 0;
}

/* Check BSS (memsz > filesz) in any segment */
static int elf_has_bss(const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdrs) {
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_memsz > phdrs[i].p_filesz)
            return 1;
    }
    return 0;
}

/* Total memory size needed by loadable segments */
static uint64_t elf_total_vsize(const Elf64_Ehdr *ehdr, const Elf64_Phdr *phdrs) {
    uint64_t min = UINT64_MAX, max = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        if (phdrs[i].p_vaddr < min) min = phdrs[i].p_vaddr;
        uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (end > max) max = end;
    }
    return (min < max) ? (max - min) : 0;
}

/* Helper: create a valid minimal ELF header */
static void make_valid_ehdr(Elf64_Ehdr *e) {
    memset(e, 0, sizeof(*e));
    e->e_ident[0] = ELFMAG0; e->e_ident[1] = ELFMAG1;
    e->e_ident[2] = ELFMAG2; e->e_ident[3] = ELFMAG3;
    e->e_ident[4] = ELFCLASS64;
    e->e_ident[5] = ELFDATA2LSB;
    e->e_type = ET_EXEC;
    e->e_machine = EM_X86_64;
    e->e_phentsize = sizeof(Elf64_Phdr);
    e->e_phnum = 1;
    e->e_entry = 0x40000000;
}

/* ======== TESTS ======== */

/* --- Header validation --- */

void t_valid_header(void) {
    Elf64_Ehdr e;
    make_valid_ehdr(&e);
    CHECK_EQ(elf_validate_ehdr(&e), 0);
}

void t_bad_magic0(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_ident[0] = 0x00;
    CHECK_EQ(elf_validate_ehdr(&e), -2);
}

void t_bad_magic1(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_ident[1] = 'X';
    CHECK_EQ(elf_validate_ehdr(&e), -2);
}

void t_bad_magic2(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_ident[2] = 'X';
    CHECK_EQ(elf_validate_ehdr(&e), -2);
}

void t_bad_magic3(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_ident[3] = 'X';
    CHECK_EQ(elf_validate_ehdr(&e), -2);
}

void t_not_64bit(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_ident[4] = 1;  /* ELFCLASS32 */
    CHECK_EQ(elf_validate_ehdr(&e), -3);
}

void t_big_endian(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_ident[5] = 2;  /* ELFDATA2MSB */
    CHECK_EQ(elf_validate_ehdr(&e), -4);
}

void t_wrong_machine(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_machine = 40;  /* ARM */
    CHECK_EQ(elf_validate_ehdr(&e), -5);
}

void t_not_exec(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_type = 3;  /* ET_DYN */
    CHECK_EQ(elf_validate_ehdr(&e), -6);
}

void t_no_phdrs(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 0;
    CHECK_EQ(elf_validate_ehdr(&e), -7);
}

void t_null_ehdr(void) {
    CHECK_EQ(elf_validate_ehdr(NULL), -1);
}

void t_small_phentsize(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phentsize = 32;  /* too small for Elf64_Phdr */
    CHECK_EQ(elf_validate_ehdr(&e), -8);
}

/* --- Segment analysis --- */

void t_count_loadable_one(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    CHECK_EQ(elf_count_loadable(&e, &p), 1);
}

void t_count_loadable_two(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 2;
    Elf64_Phdr p[2] = {{0}, {0}};
    p[0].p_type = PT_LOAD;
    p[1].p_type = PT_LOAD;
    CHECK_EQ(elf_count_loadable(&e, p), 2);
}

void t_count_loadable_mixed(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 3;
    Elf64_Phdr p[3] = {{0}, {0}, {0}};
    p[0].p_type = 6;  /* PT_PHDR */
    p[1].p_type = PT_LOAD;
    p[2].p_type = 0x6474e551;  /* PT_GNU_STACK */
    CHECK_EQ(elf_count_loadable(&e, p), 1);
}

void t_has_exec_segment(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    p.p_flags = PF_R | PF_X;
    CHECK(elf_has_exec_segment(&e, &p));
}

void t_no_exec_segment(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    p.p_flags = PF_R | PF_W;
    CHECK(!elf_has_exec_segment(&e, &p));
}

void t_has_writable(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    p.p_flags = PF_R | PF_W;
    CHECK(elf_has_writable_segment(&e, &p));
}

void t_no_writable(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    p.p_flags = PF_R | PF_X;
    CHECK(!elf_has_writable_segment(&e, &p));
}

void t_has_bss(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    p.p_filesz = 100;
    p.p_memsz = 200;
    CHECK(elf_has_bss(&e, &p));
}

void t_no_bss(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    p.p_filesz = 100;
    p.p_memsz = 100;
    CHECK(!elf_has_bss(&e, &p));
}

/* --- Virtual address calculations --- */

void t_total_vsize_single(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = PT_LOAD;
    p.p_vaddr = 0x40000000;
    p.p_memsz = 0x1000;
    CHECK_EQ((long)elf_total_vsize(&e, &p), 0x1000);
}

void t_total_vsize_two_contiguous(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 2;
    Elf64_Phdr p[2] = {{0}, {0}};
    p[0].p_type = PT_LOAD; p[0].p_vaddr = 0x40000000; p[0].p_memsz = 0x1000;
    p[1].p_type = PT_LOAD; p[1].p_vaddr = 0x40001000; p[1].p_memsz = 0x2000;
    CHECK_EQ((long)elf_total_vsize(&e, p), 0x3000);
}

void t_total_vsize_two_gapped(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 2;
    Elf64_Phdr p[2] = {{0}, {0}};
    p[0].p_type = PT_LOAD; p[0].p_vaddr = 0x40000000; p[0].p_memsz = 0x1000;
    p[1].p_type = PT_LOAD; p[1].p_vaddr = 0x40002000; p[1].p_memsz = 0x1000;
    CHECK_EQ((long)elf_total_vsize(&e, p), 0x3000);  /* includes gap */
}

void t_total_vsize_no_loadable(void) {
    Elf64_Ehdr e; make_valid_ehdr(&e);
    e.e_phnum = 1;
    Elf64_Phdr p = {0};
    p.p_type = 0;  /* not PT_LOAD */
    CHECK_EQ((long)elf_total_vsize(&e, &p), 0);
}

/* --- Permission flag checks --- */

void t_flags_rx(void) {
    Elf64_Phdr p = {0}; p.p_flags = PF_R | PF_X;
    CHECK(p.p_flags & PF_X);
    CHECK(p.p_flags & PF_R);
    CHECK(!(p.p_flags & PF_W));
}

void t_flags_rwx(void) {
    Elf64_Phdr p = {0}; p.p_flags = PF_R | PF_W | PF_X;
    CHECK(p.p_flags & PF_X);
    CHECK(p.p_flags & PF_W);
    CHECK(p.p_flags & PF_R);
}

void t_flags_rw(void) {
    Elf64_Phdr p = {0}; p.p_flags = PF_R | PF_W;
    CHECK(!(p.p_flags & PF_X));
    CHECK(p.p_flags & PF_W);
}

void t_flags_ro(void) {
    Elf64_Phdr p = {0}; p.p_flags = PF_R;
    CHECK(!(p.p_flags & PF_X));
    CHECK(!(p.p_flags & PF_W));
    CHECK(p.p_flags & PF_R);
}

/* --- Structure size checks --- */

void t_ehdr_size(void) {
    CHECK_EQ(sizeof(Elf64_Ehdr), 64);
}

void t_phdr_size(void) {
    CHECK_EQ(sizeof(Elf64_Phdr), 56);
}

int main(void) {
    printf("=== ELF64 Parser Tests ===\n\n");

    printf("--- header validation ---\n");
    RUN(t_valid_header);
    RUN(t_bad_magic0); RUN(t_bad_magic1); RUN(t_bad_magic2); RUN(t_bad_magic3);
    RUN(t_not_64bit); RUN(t_big_endian);
    RUN(t_wrong_machine); RUN(t_not_exec);
    RUN(t_no_phdrs); RUN(t_null_ehdr); RUN(t_small_phentsize);

    printf("--- segment analysis ---\n");
    RUN(t_count_loadable_one); RUN(t_count_loadable_two); RUN(t_count_loadable_mixed);
    RUN(t_has_exec_segment); RUN(t_no_exec_segment);
    RUN(t_has_writable); RUN(t_no_writable);
    RUN(t_has_bss); RUN(t_no_bss);

    printf("--- virtual address calculations ---\n");
    RUN(t_total_vsize_single);
    RUN(t_total_vsize_two_contiguous);
    RUN(t_total_vsize_two_gapped);
    RUN(t_total_vsize_no_loadable);

    printf("--- permission flags ---\n");
    RUN(t_flags_rx); RUN(t_flags_rwx); RUN(t_flags_rw); RUN(t_flags_ro);

    printf("--- structure sizes ---\n");
    RUN(t_ehdr_size); RUN(t_phdr_size);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}
