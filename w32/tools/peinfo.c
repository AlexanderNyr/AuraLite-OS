/* peinfo.c — dump a PE32+ image, WIN32_PLAN.md phase W32-2.
 *
 * A host tool, so the parser can be pointed at a real file without booting
 * anything.  Its output is deliberately field-per-line and stable, so it can
 * be diffed against `llvm-readobj --file-headers` in the test gate.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/w32_pe.h"

static const char *subsystem_name(unsigned s) {
    switch (s) {
    case PE_SUBSYSTEM_NATIVE:           return "native";
    case PE_SUBSYSTEM_WINDOWS_GUI:      return "Windows GUI";
    case PE_SUBSYSTEM_WINDOWS_CUI:      return "Windows console";
    case PE_SUBSYSTEM_EFI_APPLICATION:  return "EFI application";
    case PE_SUBSYSTEM_EFI_BOOT_DRIVER:  return "EFI boot driver";
    case PE_SUBSYSTEM_EFI_RUNTIME_DRV:  return "EFI runtime driver";
    case PE_SUBSYSTEM_EFI_ROM:          return "EFI ROM";
    default:                            return "unknown";
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: peinfo <file.exe|file.efi>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "empty file\n"); fclose(f); return 2; }

    unsigned char *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "short read\n"); fclose(f); free(buf); return 2;
    }
    fclose(f);

    pe_image_t img;
    int rc = pe_parse(buf, (size_t)sz, &img);
    if (rc != PE_OK) {
        printf("parse: FAILED (%s)\n", pe_strerror(rc));
        free(buf);
        return 1;
    }

    printf("file:             %s (%ld bytes)\n", argv[1], sz);
    printf("pe_offset:        0x%X\n", img.pe_offset);
    printf("machine:          0x%04X%s\n", img.machine,
           img.machine == PE_MACHINE_AMD64 ? " (AMD64)" : "");
    printf("magic:            0x%04X (PE32+)\n", img.opt_magic);
    printf("sections:         %u\n", img.section_count);
    printf("characteristics:  0x%04X\n", img.characteristics);
    printf("subsystem:        %u (%s)\n", img.subsystem,
           subsystem_name(img.subsystem));
    printf("image_base:       0x%llX\n", (unsigned long long)img.image_base);
    printf("entry_point_rva:  0x%X\n", img.entry_point_rva);
    printf("size_of_image:    0x%X\n", img.size_of_image);
    printf("size_of_headers:  0x%X\n", img.size_of_headers);
    printf("section_align:    0x%X\n", img.section_alignment);
    printf("file_align:       0x%X\n", img.file_alignment);
    printf("directories:      %u\n", img.num_directories);

    printf("\n%-10s %-10s %-10s %-10s %-10s %s\n",
           "name", "vaddr", "vsize", "raw_off", "raw_size", "flags");
    for (unsigned i = 0; i < img.section_count; i++) {
        pe_section_t s;
        if (pe_get_section(&img, (unsigned short)i, &s) != PE_OK) {
            printf("  <section %u unreadable>\n", i);
            continue;
        }
        char fl[8]; int n = 0;
        if (s.characteristics & PE_SCN_MEM_READ)    fl[n++] = 'R';
        if (s.characteristics & PE_SCN_MEM_WRITE)   fl[n++] = 'W';
        if (s.characteristics & PE_SCN_MEM_EXECUTE) fl[n++] = 'X';
        fl[n] = '\0';
        printf("%-10s 0x%08X 0x%08X 0x%08X 0x%08X %s\n",
               s.name, s.virtual_address, s.virtual_size,
               s.raw_offset, s.raw_size, fl);
    }

    pe_import_t imp[512]; size_t ni = 0;
    rc = pe_imports(&img, imp, 512, &ni);
    printf("\nimports:          %s", rc == PE_OK ? "" : pe_strerror(rc));
    if (rc == PE_OK) {
        printf("%zu\n", ni);
        const char *last = "";
        for (size_t i = 0; i < ni && i < 512; i++) {
            if (strcmp(last, imp[i].dll) != 0) {
                printf("  %s\n", imp[i].dll[0] ? imp[i].dll : "<unnamed>");
                last = imp[i].dll;
            }
            if (imp[i].by_ordinal) printf("      #%u\n", imp[i].ordinal);
            else                   printf("      %s\n", imp[i].name);
        }
    } else {
        printf("\n");
    }

    pe_reloc_t rel[4096]; size_t nr = 0;
    rc = pe_relocations(&img, rel, 4096, &nr);
    if (rc == PE_OK) printf("relocations:      %zu\n", nr);
    else             printf("relocations:      %s\n", pe_strerror(rc));

    rc = pe_check_loadable(&img);
    printf("loadable as w32:  %s\n",
           rc == PE_OK ? "yes" : pe_strerror(rc));

    free(buf);
    return 0;
}
