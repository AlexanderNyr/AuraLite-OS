/* kernel/arch/riscv64/initrd_rv.c -- USTAR initrd reader (RISCV_PLAN V5).
 *
 * initrd32.c's parsing rules at the third width: 512-byte headers,
 * octal size field, entries rounded to 512, "ustar" magic checked on
 * the first header, paths normalised of "./" and leading "/".  The
 * archive is wherever /chosen said (V1 recorded it in boot_info);
 * pmm_rv_init marked those frames used, so the data pointers handed
 * out stay valid for the kernel's lifetime.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/riscv64/initrd_rv.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/sbi.h"

static const uint8_t *archive;
static uint64_t archive_size;
static uint32_t file_count;

struct ustar_hdr {
    char name[100];
    char mode[8], uid[8], gid[8];
    char size[12];
    char mtime[12], chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6], version[2];
    char uname[32], gname[32];
    char devmajor[8], devminor[8];
    char prefix[155];
    char pad[12];
};

static uint64_t octal_field(const char *s, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7')
            continue;
        v = (v << 3) | (uint64_t)(s[i] - '0');
    }
    return v;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char *normalise(const char *name)
{
    if (name[0] == '.' && name[1] == '/')
        name += 2;
    while (name[0] == '/')
        name++;
    return name;
}

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}

int initrd_rv_init(const boot_info_t *bi)
{
    if (!bi->initrd_phys || !bi->initrd_size) {
        sbi_puts("[initrd] none present (-initrd not passed)\n");
        return -1;
    }

    archive      = (const uint8_t *)p2v_rv(bi->initrd_phys);
    archive_size = bi->initrd_size;

    const struct ustar_hdr *h = (const struct ustar_hdr *)archive;
    if (!(h->magic[0] == 'u' && h->magic[1] == 's' && h->magic[2] == 't' &&
          h->magic[3] == 'a' && h->magic[4] == 'r')) {
        sbi_puts("[initrd] bad ustar magic -- refusing the archive\n");
        archive = 0;
        return -1;
    }

    /* Count entries for the boot log. */
    uint64_t off = 0;
    file_count = 0;
    while (off + 512 <= archive_size) {
        h = (const struct ustar_hdr *)(archive + off);
        if (h->name[0] == '\0')
            break;
        uint64_t fsize = octal_field(h->size, 12);
        if (h->typeflag == '0' || h->typeflag == '\0')
            file_count++;
        off += 512 + ((fsize + 511) & ~511UL);
    }

    sbi_puts("[initrd] USTAR archive: ");
    put_udec_(file_count);
    sbi_puts(" files, ");
    put_udec_(archive_size);
    sbi_puts(" bytes (shared with the x86_64 and i386 kernels)\n");
    return 0;
}

int initrd_rv_find(const char *path, const uint8_t **data, uint64_t *size)
{
    if (!archive)
        return 0;

    uint64_t off = 0;
    while (off + 512 <= archive_size) {
        const struct ustar_hdr *h = (const struct ustar_hdr *)(archive + off);
        if (h->name[0] == '\0')
            break;
        uint64_t fsize = octal_field(h->size, 12);

        if ((h->typeflag == '0' || h->typeflag == '\0') &&
            str_eq(normalise(h->name), path)) {
            if (off + 512 + fsize > archive_size)
                return 0;            /* truncated entry: refuse */
            *data = archive + off + 512;
            *size = fsize;
            return 1;
        }
        off += 512 + ((fsize + 511) & ~511UL);
    }
    return 0;
}

uint32_t initrd_rv_file_count(void)
{
    return file_count;
}
