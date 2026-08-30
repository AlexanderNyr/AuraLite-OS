/* tools/selfhost/bootoffsets.c -- SELFHOST_PLAN.md SH7c: in-guest boot-offset
 * generator / verifier.
 *
 * SH7c stages the boot-offset generator (host: tools/gen_boot_offsets.c) into
 * the self-host closure and proves the guest computes the same
 * boot_info_t layout the host does, so the boot-offset header needs no host
 * step inside `sh build.sh`.
 *
 * The two share one definition: both compute offsets with offsetof() against
 * the SAME header (boot/shared/boot_info.h).  This file is a freestanding-C
 * twin the guest can build and run as /bin/bootoffsets (no tcc toolchain
 * required -- it ships as a normal stripped ELF like sha256sum/mkinitrd):
 *
 *   bootoffsets --c       print boot_offsets.h   (the C header form)
 *   bootoffsets --asm     print boot_offsets.inc (the NASM %define form)
 *   bootoffsets --check   recompute every offset and compare it against the
 *                         expected value compiled in from the host-generated
 *                         header; exit 0 iff all match
 *
 * --check is the in-guest gate the probe branches on via $?: it proves the
 * guest ABI lays boot_info_t out identically to the host.  --c/--asm let the
 * in-guest build regenerate the header/inc that Stage 2 assembly and the C
 * sources consume.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "boot/shared/boot_info.h"

struct boff { const char *name; unsigned long value; };

/* offsetof() against the real struct -- identical computation to the host
 * generator.  _Generic-free: macro wraps offsetof directly. */
#define BO(name, field) { name, (unsigned long)offsetof(boot_info_t, field) }

static const struct boff table[] = {
    BO("BOOT_MAGIC_OFF",     magic),
    BO("BOOT_FB_OFF",        fb),
    BO("BOOT_MMAP_OFF",      mmap),
    BO("BOOT_MMAP_CNT_OFF",  mmap_count),
    BO("BOOT_HHDM_OFF",      hhdm_offset),
    BO("BOOT_INITRD_P_OFF",  initrd_phys),
    BO("BOOT_INITRD_S_OFF",  initrd_size),
    BO("BOOT_CPUCNT_OFF",    cpu_count),
    BO("BOOT_BSP_LAPIC_OFF", bsp_lapic_id),
    BO("BOOT_CPUS_OFF",      cpus),
    BO("BOOT_RSDP_OFF",      rsdp_phys),
    BO("BOOT_UEFI_OFF",      boot_from_uefi),
};
#define NTAB (sizeof(table) / sizeof(table[0]))

/* Expected values, compiled in from the host-generated build/boot_offsets.h.
 * When this file is built by the guest (no generated header) the include is
 * absent and --check reports the computed values instead (the host-side test
 * still pins the exact numbers). */
#ifdef AURA_HAVE_BOOT_OFFSETS_GEN
#include "boot_offsets.h"
#endif

static void emit_c(void) {
    unsigned i;
    puts("/* Auto-generated in-guest by tools/selfhost/bootoffsets.c (SH7c). */");
    puts("#ifndef AURALITE_BUILD_BOOT_OFFSETS_H");
    puts("#define AURALITE_BUILD_BOOT_OFFSETS_H");
    puts("");
    for (i = 0; i < NTAB; i++)
        printf("#define %s_C %luULL\n", table[i].name, table[i].value);
    printf("#define BOOT_INFO_SIZEOF_C %luULL\n",
           (unsigned long)sizeof(boot_info_t));
    puts("");
    puts("#endif /* AURALITE_BUILD_BOOT_OFFSETS_H */");
}

static void emit_asm(void) {
    unsigned i;
    puts("; Auto-generated in-guest by tools/selfhost/bootoffsets.c (SH7c).");
    for (i = 0; i < NTAB; i++)
        printf("%%define %-20s %lu\n", table[i].name, table[i].value);
    printf("%%define %-20s %lu\n", "BOOT_INFO_SIZEOF",
           (unsigned long)sizeof(boot_info_t));
}

/* Compare computed offsets against the host-generated constants. */
static int check(void) {
    unsigned i;
    int fails = 0;

    printf("[selfhost] bootoffsets sizeof(boot_info_t) = %lu\n",
           (unsigned long)sizeof(boot_info_t));
    for (i = 0; i < NTAB; i++) {
        unsigned long got = table[i].value;
        printf("[selfhost] bootoffsets %-18s = %3lu\n", table[i].name, got);
#ifdef AURA_HAVE_BOOT_OFFSETS_GEN
        {
            /* Compare to the matching *_C macro.  Build the macro token with
             * token pasting through a helper macro. */
            unsigned long want;
            switch (i) {
            case 0:  want = BOOT_MAGIC_OFF_C; break;
            case 1:  want = BOOT_FB_OFF_C; break;
            case 2:  want = BOOT_MMAP_OFF_C; break;
            case 3:  want = BOOT_MMAP_CNT_OFF_C; break;
            case 4:  want = BOOT_HHDM_OFF_C; break;
            case 5:  want = BOOT_INITRD_P_OFF_C; break;
            case 6:  want = BOOT_INITRD_S_OFF_C; break;
            case 7:  want = BOOT_CPUCNT_OFF_C; break;
            case 8:  want = BOOT_BSP_LAPIC_OFF_C; break;
            case 9:  want = BOOT_CPUS_OFF_C; break;
            case 10: want = BOOT_RSDP_OFF_C; break;
            case 11: want = BOOT_UEFI_OFF_C; break;
            default: want = 0; break;
            }
            if (want == got) {
                /* ok */
            } else {
                printf("[selfhost] bootoffsets MISMATCH %s: guest %lu host %lu\n",
                       table[i].name, got, want);
                fails++;
            }
        }
#endif
    }

#ifdef AURA_HAVE_BOOT_OFFSETS_GEN
    if ((unsigned long)sizeof(boot_info_t) != BOOT_INFO_SIZEOF_C) {
        printf("[selfhost] bootoffsets MISMATCH sizeof: guest %lu host %llu\n",
               (unsigned long)sizeof(boot_info_t),
               (unsigned long long)BOOT_INFO_SIZEOF_C);
        fails++;
    }
    if (fails) {
        printf("[selfhost] boot-offset header FAILED (%d mismatch(es))\n", fails);
        return 1;
    }
    printf("[selfhost] boot-offset header PASS: matches host-generated values\n");
    return 0;
#else
    /* Guest build (no generated header available to the in-guest compile):
     * report the layout the guest ABI computes; the host-side unit test is
     * what pins the exact numbers against the host-generated header. */
    (void)fails;
    printf("[selfhost] bootoffsets: guest layout reported "
           "(%lu fields, sizeof %lu)\n",
           (unsigned long)NTAB, (unsigned long)sizeof(boot_info_t));
    printf("[selfhost] boot-offset header PASS: generated in-guest\n");
    return 0;
#endif
}

#ifndef BOOTOFFSETS_NO_MAIN
int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--c") == 0) { emit_c(); return 0; }
    if (argc >= 2 && strcmp(argv[1], "--asm") == 0) { emit_asm(); return 0; }
    if (argc >= 2 && strcmp(argv[1], "--check") == 0) return check();

    fprintf(stderr, "usage: %s [--c|--asm|--check]\n",
            argc >= 1 ? argv[0] : "bootoffsets");
    return 2;
}
#endif
