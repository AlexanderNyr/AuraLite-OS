/* boot/uefi/efi_acpi.c -- locate the RSDP via the UEFI configuration table
 * and enumerate CPUs from the ACPI MADT.
 *
 * This is the UEFI-path counterpart of boot/bios/stage2/acpi.inc: without
 * it, boot_info.cpu_count stays hard-coded at 1 and the kernel never learns
 * that more than one logical CPU exists (or their Local APIC IDs, needed to
 * target each one with an INIT-SIPI-SIPI sequence), so it unconditionally
 * runs BSP-only regardless of how many CPUs are actually present.
 *
 * Unlike the BIOS path, we don't need to physically search memory for the
 * "RSD PTR " signature: UEFI publishes a pointer to it directly in the
 * system table's configuration table array, keyed by a well-known GUID.
 *
 * References:
 *   UEFI Specification 2.10, s.4.6 (EFI Configuration Table).
 *   ACPI Specification 6.5, s.5.2.5 (RSDP), s.5.2.8 (MADT), s.5.2.12.2
 *   (Processor Local APIC Structure).
 */

#include <stdint.h>
#include "boot/uefi/efi_types.h"
#include "boot/shared/boot_info.h"

/* ACPI 2.0+ RSDP table GUID: 8868E871-E4F1-11D3-BC22-0080C73C8881. */
static const uint8_t ACPI_20_TABLE_GUID[16] = {
    0x71, 0xe8, 0x68, 0x88,  0xf1, 0xe4,  0xd3, 0x11,
    0xbc, 0x22,  0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81,
};
/* ACPI 1.0 RSDP table GUID: EB9D2D30-2D88-11D3-9A16-0090273FC14D. */
static const uint8_t ACPI_10_TABLE_GUID[16] = {
    0x30, 0x2d, 0x9d, 0xeb,  0x88, 0x2d,  0xd3, 0x11,
    0x9a, 0x16,  0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d,
};

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* ACPI System Description Table common header (36 bytes), shared by
 * RSDT/XSDT/MADT/... (ACPI 6.5 s.5.2.6). */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/* Sum every byte of a table and check the ACPI checksum rule (total must be
 * 0 mod 256).  Returns 1 if valid. */
static int acpi_checksum_ok(const void *table, uint32_t len) {
    const uint8_t *p = (const uint8_t *)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

/* Find the UEFI configuration table entry matching a vendor GUID.
 * Returns the VendorTable pointer, or NULL if not present. */
static void *find_config_table(EFI_SYSTEM_TABLE *st, const uint8_t *guid) {
    EFI_CONFIGURATION_TABLE *tables =
        (EFI_CONFIGURATION_TABLE *)st->ConfigurationTable;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(tables[i].VendorGuid, guid)) {
            return tables[i].VendorTable;
        }
    }
    return 0;
}

/* Walk the MADT's variable-length interrupt-controller-structure list and
 * record every enabled "Processor Local APIC" entry (type 0) into
 * info->cpus[]/cpu_count.  Leaves cpu_count untouched (the caller's
 * single-CPU default of 1 stands) if the table looks malformed or contains
 * no usable entries -- publishing a garbage CPU table would make the
 * kernel's AP wake-up logic target non-existent processors. */
static void parse_madt(boot_info_t *info, const uint8_t *madt) {
    const struct acpi_sdt_header *hdr = (const struct acpi_sdt_header *)madt;
    if (hdr->length < 44) return;   /* header + LAPIC addr + flags, minimum */
    if (!acpi_checksum_ok(madt, hdr->length)) return;

    const uint8_t *end = madt + hdr->length;
    const uint8_t *ics = madt + 44;   /* first Interrupt Controller Structure */
    uint32_t found = 0;

    while (ics + 2 <= end) {
        uint8_t type = ics[0];
        uint8_t ics_len = ics[1];
        if (ics_len == 0) break;              /* malformed: stop scanning */
        if (ics + ics_len > end) break;        /* truncated entry: stop */

        if (type == 0 && ics_len >= 8) {
            /* Processor Local APIC (ACPI 6.5 s.5.2.12.2):
             *   +2 byte  ACPI Processor ID
             *   +3 byte  APIC ID
             *   +4 dword Flags (bit 0 = Enabled) */
            uint32_t flags = (uint32_t)ics[4] | ((uint32_t)ics[5] << 8) |
                              ((uint32_t)ics[6] << 16) | ((uint32_t)ics[7] << 24);
            if ((flags & 1) && found < BOOT_MAX_CPUS) {
                info->cpus[found].processor_id   = ics[2];
                info->cpus[found].lapic_id       = ics[3];
                info->cpus[found].goto_address   = 0;
                info->cpus[found].extra_argument = 0;
                found++;
            }
        }
        ics += ics_len;
    }

    if (found > 0) {
        info->cpu_count = found;
    }
}

/* efi_acpi_discover_cpus -- entry point called from efi_main() after the
 * system table is available (i.e. before ExitBootServices, while
 * st->ConfigurationTable is still guaranteed valid).
 *
 * On entry: info->cpu_count is pre-set to 1 by the caller (single-CPU
 * fallback).  On exit: cpu_count/cpus[] are updated only if a well-formed
 * MADT with at least one enabled Local APIC was found; info->rsdp_phys is
 * recorded whenever an RSDP is found at all (even if MADT parsing fails),
 * matching the BIOS path's behaviour. */
void efi_acpi_discover_cpus(EFI_SYSTEM_TABLE *st, boot_info_t *info) {
    void *rsdp = find_config_table(st, ACPI_20_TABLE_GUID);
    if (!rsdp) {
        rsdp = find_config_table(st, ACPI_10_TABLE_GUID);
    }
    if (!rsdp) return;

    info->rsdp_phys = (uint64_t)(uintptr_t)rsdp;

    const uint8_t *r = (const uint8_t *)rsdp;
    /* RSDP signature check ("RSD PTR ", 8 bytes, no NUL). */
    static const char sig[8] = { 'R','S','D',' ','P','T','R',' ' };
    for (int i = 0; i < 8; i++) {
        if (r[i] != sig[i]) return;
    }

    uint8_t revision = r[15];
    uint64_t sdt_phys = 0;
    if (revision >= 2) {
        /* XsdtAddress at offset 24 (8 bytes). */
        uint64_t xsdt = 0;
        for (int i = 0; i < 8; i++) xsdt |= (uint64_t)r[24 + i] << (8 * i);
        sdt_phys = xsdt;
    }
    if (sdt_phys == 0) {
        /* RsdtAddress at offset 16 (4 bytes), ACPI 1.0 fallback. */
        uint32_t rsdt = 0;
        for (int i = 0; i < 4; i++) rsdt |= (uint32_t)r[16 + i] << (8 * i);
        sdt_phys = rsdt;
    }
    if (sdt_phys == 0) return;

    const struct acpi_sdt_header *sdt =
        (const struct acpi_sdt_header *)(uintptr_t)sdt_phys;
    if (sdt->length < sizeof(struct acpi_sdt_header)) return;

    int is_xsdt = (sdt->signature[0] == 'X' && sdt->signature[1] == 'S' &&
                   sdt->signature[2] == 'D' && sdt->signature[3] == 'T');
    uint32_t entry_stride = is_xsdt ? 8u : 4u;
    uint32_t entry_bytes = sdt->length - (uint32_t)sizeof(struct acpi_sdt_header);
    const uint8_t *entries = (const uint8_t *)sdt + sizeof(struct acpi_sdt_header);

    for (uint32_t off = 0; off + entry_stride <= entry_bytes; off += entry_stride) {
        uint64_t table_phys = 0;
        for (uint32_t i = 0; i < entry_stride; i++) {
            table_phys |= (uint64_t)entries[off + i] << (8 * i);
        }
        if (table_phys == 0) continue;

        const uint8_t *table = (const uint8_t *)(uintptr_t)table_phys;
        if (table[0] == 'A' && table[1] == 'P' && table[2] == 'I' && table[3] == 'C') {
            parse_madt(info, table);
            return;
        }
    }
}
