/* test_bsod.c — host gate for the STOP-code table (docs/bsod.md).
 *
 * Compiles the shipping kernel/lib/bsod.c with AURALITE_BSOD_HOST_TEST
 * so the lookup functions are the ones the guest paints, not a copy.
 */
#define AURALITE_BSOD_HOST_TEST 1
#include "kernel/lib/bsod.c"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, name) do {                                 \
    if (cond) printf("PASS %s\n", name);                       \
    else      { printf("FAIL %s\n", name); fails++; }          \
} while (0)

int main(void) {
    CHECK(strcmp(bsod_stop_name(0x0E), "PAGE_FAULT") == 0,
          "vector 14 is PAGE_FAULT");
    CHECK(strcmp(bsod_stop_name(0x08), "DOUBLE_FAULT") == 0,
          "vector 8 is DOUBLE_FAULT");
    CHECK(strcmp(bsod_stop_name(BSOD_KASSERT), "KASSERT") == 0,
          "0x1001 is KASSERT");
    CHECK(strcmp(bsod_stop_name(BSOD_KEXPLICIT), "KEXPLICIT") == 0,
          "0x1002 is KEXPLICIT");
    CHECK(BSOD_KEXPLICIT == 0x00001002u, "KEXPLICIT keeps stop 0x1002");
    CHECK(strcmp(bsod_stop_name(BSOD_KCANARY), "KCANARY") == 0,
          "0x1003 is KCANARY");
    CHECK(strcmp(bsod_stop_name(BSOD_KSTACK), "KSTACK") == 0,
          "0x1004 is KSTACK");
    CHECK(strcmp(bsod_stop_name(BSOD_KRECURSE), "KRECURSE") == 0,
          "0x1005 is KRECURSE");
    CHECK(strcmp(bsod_stop_name(BSOD_KHALT), "KHALT") == 0,
          "0x10FF is KHALT");
    CHECK(strcmp(bsod_stop_name(0xDEAD), "UNKNOWN") == 0,
          "unlisted code is UNKNOWN");
    CHECK(strstr(bsod_stop_meaning(0x0E), "CR2") != NULL,
          "PAGE_FAULT meaning names CR2");
    CHECK(strstr(bsod_stop_meaning(0x08), "IST1") != NULL,
          "DOUBLE_FAULT meaning names IST1");
    CHECK(strstr(bsod_stop_meaning(BSOD_KEXPLICIT), "DETAIL") != NULL,
          "KEXPLICIT meaning names DETAIL");
    CHECK(bsod_table_has_panic_substring() == 0,
          "no STOP name or meaning contains PANIC");
    CHECK(BSOD_STOP_CPU(14) == 0x0E && BSOD_STOP_CPU(0x8E) == 0x0E,
          "BSOD_STOP_CPU masks to the low 5 bits");

    if (fails) {
        printf("%d checks failed\n", fails);
        return 1;
    }
    printf("all STOP-code checks passed\n");
    return 0;
}
