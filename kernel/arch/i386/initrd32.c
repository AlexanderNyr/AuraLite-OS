/* kernel/arch/i386/initrd32.c -- USTAR initrd reader (I386_PLAN I5).
 *
 * Mirrors kernel/fs/initrd.c's parsing rules at lookup-only scope:
 * 512-byte headers, octal size field, entries rounded to 512, "ustar"
 * magic checked on the first header, paths normalised of "./" and
 * leading "/".  The archive itself is the one BL4 loaded at
 * INITRD_LOAD_PHYS; it sits inside the PMM's low reserve, so the data
 * pointers handed out stay valid for the kernel's lifetime.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/initrd32.h"
#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/kprintf32.h"

static const uint8_t *archive;
static uint32_t archive_size;
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

static uint32_t octal_field(const char *s, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < len && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7')
            continue;
        v = (v << 3) | (uint32_t)(s[i] - '0');
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

int initrd32_init(const boot_info_t *bi)
{
    if (!bi->initrd_phys || !bi->initrd_size) {
        kprintf32("[initrd] absent (boot_info reports none)\n");
        return -1;
    }
    if (bi->initrd_phys + bi->initrd_size > DIRECT_MAP_BYTES) {
        kprintf32("[initrd] beyond the direct map; unreachable\n");
        return -1;
    }

    archive      = (const uint8_t *)p2v_32((uint32_t)bi->initrd_phys);
    archive_size = (uint32_t)bi->initrd_size;

    const struct ustar_hdr *h = (const struct ustar_hdr *)archive;
    if (!(h->magic[0] == 'u' && h->magic[1] == 's' && h->magic[2] == 't' &&
          h->magic[3] == 'a' && h->magic[4] == 'r')) {
        kprintf32("[initrd] bad USTAR magic at %p\n", (const void *)archive);
        return -1;
    }

    /* Count regular files for the boot log. */
    file_count = 0;
    uint32_t off = 0;
    while (off + 512 <= archive_size) {
        h = (const struct ustar_hdr *)(archive + off);
        if (h->name[0] == '\0')
            break;
        uint32_t size = octal_field(h->size, 12);
        if (h->typeflag == '0' || h->typeflag == '\0')
            file_count++;
        off += 512 + ((size + 511) & ~511u);
    }

    kprintf32("[initrd] USTAR at phys %x, %u KiB, %u files\n",
              (uint32_t)bi->initrd_phys, archive_size / 1024, file_count);
    return 0;
}

int initrd32_find(const char *path, const uint8_t **data, uint32_t *size)
{
    if (!archive)
        return 0;

    uint32_t off = 0;
    while (off + 512 <= archive_size) {
        const struct ustar_hdr *h = (const struct ustar_hdr *)(archive + off);
        if (h->name[0] == '\0')
            break;
        uint32_t fsize = octal_field(h->size, 12);
        if ((h->typeflag == '0' || h->typeflag == '\0') &&
            str_eq(normalise(h->name), path)) {
            *data = archive + off + 512;
            *size = fsize;
            return 1;
        }
        off += 512 + ((fsize + 511) & ~511u);
    }
    return 0;
}

uint32_t initrd32_file_count(void)
{
    return file_count;
}

/* PARITY P4: the N-th regular file's name and size (readdir's
 * backend; same walk as initrd32_find, indexed).  1 on hit. */
int initrd32_stat_index(uint32_t idx, const char **name, uint32_t *size)
{
    if (!archive)
        return 0;
    uint32_t off = 0, seen = 0;
    while (off + 512 <= archive_size) {
        const struct ustar_hdr *h = (const struct ustar_hdr *)(archive + off);
        if (h->name[0] == '\0')
            break;
        uint32_t fsize = octal_field(h->size, 12);
        if (h->typeflag == '0' || h->typeflag == '\0') {
            if (seen == idx) {
                *name = normalise(h->name);
                *size = fsize;
                return 1;
            }
            seen++;
        }
        off += 512 + ((fsize + 511) & ~511u);
    }
    return 0;
}
