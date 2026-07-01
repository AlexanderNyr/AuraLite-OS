#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../../kernel/arch/x86_64/gdt.h"

void gdt_flush(uint64_t gdtr_ptr) { (void)gdtr_ptr; }

static uint64_t decode_tss_base(struct gdt_entry *buf, int index) {
    uint64_t low = (uint64_t)buf[index].base_low |
                   ((uint64_t)buf[index].base_middle << 16) |
                   ((uint64_t)buf[index].base_high << 24);
    uint64_t high = (uint64_t)buf[index + 1].limit_low |
                    ((uint64_t)buf[index + 1].base_low << 16);
    return low | (high << 32);
}

static uint32_t decode_tss_limit(struct gdt_entry *buf, int index) {
    return (uint32_t)buf[index].limit_low |
           ((uint32_t)(buf[index].granularity & 0x0F) << 16);
}

static void test_gdt_set_tss_in_targets_supplied_buffer(void) {
    struct gdt_entry local_a[GDT_NUM_ENTRIES];
    struct gdt_entry local_b[GDT_NUM_ENTRIES];
    memset(gdt, 0, sizeof(gdt));
    memset(local_a, 0, sizeof(local_a));
    memset(local_b, 0, sizeof(local_b));

    gdt_set_tss_in(local_a, 5, 0x1122334455667788ULL, 0xABCDEU);
    assert(decode_tss_base(local_a, 5) == 0x1122334455667788ULL);
    assert(decode_tss_limit(local_a, 5) == 0xABCDEU);
    assert(local_a[5].access == 0x89);
    assert(decode_tss_base(gdt, 5) == 0);

    gdt_set_tss_in(local_b, 5, 0x8877665544332211ULL, 0x54321U);
    assert(decode_tss_base(local_b, 5) == 0x8877665544332211ULL);
    assert(decode_tss_limit(local_b, 5) == 0x54321U);
    assert(decode_tss_base(local_a, 5) == 0x1122334455667788ULL);
}

static void test_gdt_set_tss_still_updates_global_gdt(void) {
    memset(gdt, 0, sizeof(gdt));
    gdt_set_tss(5, 0x123456789ABCDEF0ULL, 0x13579U);
    assert(decode_tss_base(gdt, 5) == 0x123456789ABCDEF0ULL);
    assert(decode_tss_limit(gdt, 5) == 0x13579U);
    assert(gdt[5].access == 0x89);
}

int main(void) {
    test_gdt_set_tss_in_targets_supplied_buffer();
    test_gdt_set_tss_still_updates_global_gdt();
    return 0;
}
