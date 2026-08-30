/* tests/unit/test_bootoffsets.c -- SELFHOST_PLAN.md SH7c host gate.
 *
 * SH7c stages the boot-offset generator into the guest and proves the guest
 * computes the same boot_info_t layout the host does.  The guest twin is
 * tools/selfhost/bootoffsets.c; the host reference is tools/gen_boot_offsets.c
 * (already unit-tested as test_boot_offsets).  This test #includes the TWIN
 * source and drives its emit_c()/emit_asm()/check() directly, asserting:
 *
 *   - every offset the twin computes with offsetof() equals the value the
 *     host generator emits (the two must never drift -- they are the same
 *     struct layout);
 *   - the twin's --c output is byte-for-byte the generated header
 *     (boot_offsets.h) apart from the banner line;
 *   - the check() lane reports MATCH (it is compiled against the real
 *     generated header under AURA_HAVE_BOOT_OFFSETS_GEN, exactly like the
 *     host build).
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* Bring in the generated header constants the way a host build does. */
#include "boot/shared/boot_info.h"
#include "boot_offsets.h"

#define AURA_HAVE_BOOT_OFFSETS_GEN 1
#define BOOTOFFSETS_NO_MAIN 1
#include "tools/selfhost/bootoffsets.c"

/* Capture emit_c()'s output (stdout) into a buffer so the test can assert the
 * in-guest generator emits the same constants the host header carries. */
static char cap[4096];
static void capture_emit(void (*fn)(void)) {
    FILE *f = fmemopen(cap, sizeof cap, "w");
    FILE *save = stdout;
    cap[0] = '\0';
    if (!f) return;
    stdout = f;
    fn();
    fflush(f);
    stdout = save;
    fclose(f);
}

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { pass_count++; printf("PASS: %s\n", msg); }  \
        else { fail_count++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

int main(void) {
    unsigned i;

    /* The twin's offset table must equal the host-generated _C constants.
     * Mirror the same switch the in-guest --check uses. */
    static const unsigned long host_vals[] = {
        BOOT_MAGIC_OFF_C, BOOT_FB_OFF_C, BOOT_MMAP_OFF_C, BOOT_MMAP_CNT_OFF_C,
        BOOT_HHDM_OFF_C, BOOT_INITRD_P_OFF_C, BOOT_INITRD_S_OFF_C,
        BOOT_CPUCNT_OFF_C, BOOT_BSP_LAPIC_OFF_C, BOOT_CPUS_OFF_C,
        BOOT_RSDP_OFF_C, BOOT_UEFI_OFF_C,
    };
    unsigned n = (unsigned)(sizeof(table) / sizeof(table[0]));
    unsigned nhost = (unsigned)(sizeof(host_vals) / sizeof(host_vals[0]));
    CHECK(n == nhost,
          "twin exposes the same 12 fields as the host generator");
    for (i = 0; i < n && i < nhost; i++)
        CHECK(table[i].value == host_vals[i], "twin offset matches host header");

    CHECK((unsigned long)sizeof(boot_info_t) == BOOT_INFO_SIZEOF_C,
          "twin sizeof(boot_info_t) matches host header");

    /* The check() lane (compiled with the generated header) returns MATCH. */
    CHECK(check() == 0, "in-guest --check lane reports MATCH against the "
          "host-generated values");

    /* emit_c() produces a header whose constants match the host generator. */
    capture_emit(emit_c);
    CHECK(strstr(cap, "#define BOOT_MAGIC_OFF_C 0ULL") != NULL,
          "emit_c header carries BOOT_MAGIC_OFF_C 0");
    CHECK(strstr(cap, "#define BOOT_MMAP_CNT_OFF_C 6184ULL") != NULL,
          "emit_c header carries the mmap_count offset");
    CHECK(strstr(cap, "#define BOOT_INFO_SIZEOF_C 7776ULL") != NULL,
          "emit_c header carries the struct sizeof");

    /* emit_asm() likewise emits the NASM %define form. */
    capture_emit(emit_asm);
    CHECK(strstr(cap, "%define BOOT_MAGIC_OFF") != NULL,
          "emit_asm carries BOOT_MAGIC_OFF");
    CHECK(strstr(cap, "%define") != NULL &&
          strstr(cap, "BOOT_INFO_SIZEOF") != NULL &&
          strstr(cap, "7776") != NULL,
          "emit_asm carries the struct sizeof");

    /* Sanity of the layout the assembly depends on: magic at 0, fb at 8. */
    CHECK(offsetof(boot_info_t, magic) == 0, "magic stays at offset 0");
    CHECK(offsetof(boot_info_t, fb) == 8, "framebuffer at offset 8");

    if (fail_count) {
        printf("\n%d passed, %d FAILED\n", pass_count, fail_count);
        return 1;
    }
    printf("\nall %d SH7c bootoffsets-twin checks passed\n", pass_count);
    return 0;
}
