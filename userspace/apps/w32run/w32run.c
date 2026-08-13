/* w32run.c — run a PE32+ .exe.  WIN32_PLAN.md phase W32-4.
 *
 * The division of labour is decision D2:
 *
 *   kernel (W32-3)  maps sections, applies base relocations, enforces W^X
 *                   and the subsystem policy;
 *   here (W32-4)    resolves the import table and calls the entry point.
 *
 * Import binding lives in user space because it walks attacker-controlled
 * structures and writes addresses into the image.  A bug in it kills this
 * process; the same bug in Ring 0 kills the machine.
 *
 * How the image gets mapped: this program maps it itself, with mmap, rather
 * than asking the kernel to exec it.  That is deliberate for this phase --
 * `run foo.exe` already goes through the kernel loader (W32-3), but a PE with
 * imports needs its IAT written *after* mapping and *before* the entry point
 * runs, and there is no hook between those two points yet.  Doing the whole
 * sequence in one process keeps the phase self-contained; wiring the kernel
 * path to call back into a user-space binder is W32-6's CRT work.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "w32/w32_pe.h"
#include "w32/w32_bind.h"
#include "w32/w32_crt.h"
#include "w32/kernel32.h"

#ifndef PROT_READ
#define PROT_READ  0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC  0x4
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

static void die(const char *what) {
    printf("w32run: %s\n", what);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: w32run <program.exe> [args...]\n");
        return 2;
    }
    const char *path = argv[1];

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { printf("w32run: cannot open %s\n", path); return 1; }

    static unsigned char buf[512 * 1024];
    long total = 0;
    for (;;) {
        long n = read(fd, buf + total, sizeof buf - (unsigned long)total);
        if (n <= 0) break;
        total += n;
        if ((unsigned long)total >= sizeof buf) break;
    }
    close(fd);
    if (total <= 0) die("empty file");

    pe_image_t img;
    int rc = pe_parse(buf, (size_t)total, &img);
    if (rc != PE_OK) { printf("w32run: %s\n", pe_strerror(rc)); return 1; }

    rc = pe_check_loadable(&img);
    if (rc != PE_OK) {
        printf("w32run: refused: %s (subsystem=%u)\n",
               pe_strerror(rc), img.subsystem);
        return 1;
    }

    /* Map one writable+executable region for the whole image.
     *
     * This is weaker than the kernel loader's per-section W^X (W32-3), and it
     * is a limitation of running the image inside an existing process rather
     * than a property of the design: mprotect per section would need the
     * mapping to be page-aligned per section, which mmap here does not
     * guarantee across the whole span.  Stated plainly rather than glossed:
     * the hardened path is the kernel one, and W32-6 moves this there. */
    unsigned long span = (img.size_of_image + 0xFFFu) & ~0xFFFu;
    void *mem = mmap(0, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!mem || (long)mem < 0) die("mmap failed");

    unsigned char *base = mem;
    memset(base, 0, span);

    /* Headers first: a PE expects them readable at its base. */
    unsigned long hdr = img.size_of_headers;
    if (hdr > (unsigned long)total) hdr = (unsigned long)total;
    memcpy(base, buf, hdr);

    for (unsigned i = 0; i < img.section_count; i++) {
        pe_section_t s;
        if (pe_get_section(&img, (unsigned short)i, &s) != PE_OK)
            die("bad section");
        if (s.raw_size) {
            if ((unsigned long)s.virtual_address + s.raw_size > span)
                die("section outside image");
            memcpy(base + s.virtual_address, buf + s.raw_offset, s.raw_size);
        }
    }

    /* Base relocations: this mapping is nowhere near the preferred base. */
    static pe_reloc_t relocs[4096];
    size_t nrel = 0;
    rc = pe_relocations(&img, relocs, sizeof relocs / sizeof relocs[0], &nrel);
    if (rc != PE_OK) die("bad relocation table");
    if (nrel > sizeof relocs / sizeof relocs[0]) die("too many relocations");

    unsigned long long delta = (unsigned long long)(unsigned long)base
                             - img.image_base;
    for (size_t i = 0; i < nrel; i++) {
        if (relocs[i].type != PE_REL_DIR64) die("unsupported relocation type");
        unsigned char *p = base + relocs[i].rva;
        unsigned long long v = 0;
        for (int k = 7; k >= 0; k--) v = (v << 8) | p[k];
        v += delta;
        for (int k = 0; k < 8; k++) p[k] = (unsigned char)((v >> (k * 8)) & 0xFF);
    }

    /* Bind the imports.  An unresolved name is fatal and named. */
    const char *mdll = 0, *mname = 0;
    size_t nimp = 0;
    { pe_import_t probe[1]; (void)pe_imports(&img, probe, 0, &nimp); }
    rc = w32_bind_imports(buf, (size_t)total,
                          (unsigned long long)(unsigned long)base, &mdll, &mname);
    if (rc != 0) {
        if (mdll && mname)
            printf("w32run: unresolved import %s!%s\n", mdll, mname);
        else
            printf("w32run: import binding failed (%d)\n", rc);
        return 1;
    }

    /* The CRT state the program will use.  argv is shifted so the .exe sees
     * itself as argv[0], which is what GetCommandLineA should report. */
    w32_kernel32_init(argc - 1, argv + 1);

    printf("w32run: %s mapped at %p, %lu import(s) bound\n",
           path, (void *)base, (unsigned long)nimp);

    /* --- W32-6: the startup sequence a real CRT expects -----------------
     *
     * Order matters and is the documented one: SEH must be armed before any
     * image code runs (a TLS callback can fault too), TLS callbacks run
     * before the entry point, and static initialisers run before main.  A
     * program whose global constructor divides by zero should reach its
     * __except, not die -- which only works if the handlers are already in
     * place here. */
    if (w32_seh_init() != 0)
        printf("w32run: warning: SEH handlers not installed; "
               "__try will not catch faults\n");

    size_t image_span = (size_t)img.size_of_image;
    int ntls = w32_crt_run_tls_callbacks((unsigned char *)base, image_span,
                                         img.dir[PE_DIR_TLS].rva,
                                         img.dir[PE_DIR_TLS].size);
    if (ntls < 0) {
        /* A malformed TLS directory is refused rather than followed: the
         * callback array is data out of the file, and calling through it
         * unchecked is how a bad file becomes arbitrary execution. */
        printf("w32run: refusing malformed TLS directory (%d)\n", ntls);
        return 1;
    }
    if (ntls > 0) printf("w32run: ran %d TLS callback(s)\n", ntls);

    /* Static initialisers live in .CRT.  A real linker merges the
     * .CRT$XCA/.CRT$XCU/.CRT$XCZ contributions into that one section,
     * sorted by the text after the '$', so the section bounds ARE the
     * table bounds -- which is why they are found by section here rather
     * than by symbols the image would have to export. */
    for (uint16_t i = 0; i < img.section_count; i++) {
        pe_section_t sec;
        if (pe_get_section(&img, i, &sec) != 0) continue;
        if (sec.name[0] != '.' || sec.name[1] != 'C' ||
            sec.name[2] != 'R' || sec.name[3] != 'T' || sec.name[4] != '\0')
            continue;

        int nini = w32_crt_run_initializers((unsigned char *)base, image_span,
                                            sec.virtual_address,
                                            sec.virtual_address +
                                                sec.virtual_size);
        if (nini < 0) {
            printf("w32run: refusing malformed .CRT table (%d)\n", nini);
            return 1;
        }
        if (nini > 0) printf("w32run: ran %d static initialiser(s)\n", nini);
        break;
    }

    /* Enter the image.  The entry point is ms_abi, like every export. */
    typedef void __attribute__((ms_abi)) (*w32_entry_fn)(void);
    w32_entry_fn entry = (w32_entry_fn)(void *)(base + img.entry_point_rva);
    entry();

    /* A console PE is expected to call ExitProcess; reaching here means it
     * returned instead, which is legal for a bare entry point. */
    printf("w32run: entry point returned\n");
    return 0;
}
