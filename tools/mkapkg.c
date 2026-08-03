/*
 * mkapkg.c — build an AuraLite .apkg package from an ELF.
 *
 * Usage:
 *   mkapkg -n <name> -v <version> [-d <description>] -o <out.apkg> <input.elf>
 *
 * A host tool, but it links the SAME parser the OS uses (libc/src/apkg.c), so
 * the writer and the reader cannot disagree about the format. It also parses
 * back what it just wrote and refuses to leave a file that its own reader
 * would reject — a package builder that can emit something unreadable is a
 * builder that will eventually do it.
 */

/* HOST tool: these must be the HOST's headers.
 *
 * apkg.h is included by its full path rather than via -I libc/include,
 * because that -I would also put AuraLite's stdio.h ahead of the host's --
 * AuraLite's is a freestanding subset with no fseek/ftell/SEEK_END, so the
 * build fails in a way that looks like a missing libc.  The same trap cost
 * time in test_progpath; here it is avoided by not adding the -I at all. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/libc/include/apkg.h"

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s -n <name> -v <version> [-d <description>] "
        "-o <out.apkg> <input.elf>\n", argv0);
}

int main(int argc, char **argv) {
    const char *name = NULL, *version = NULL, *desc = NULL;
    const char *out_path = NULL, *in_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)      name    = argv[++i];
        else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) version = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) desc    = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path= argv[++i];
        else if (argv[i][0] == '-')                        { usage(argv[0]); return 2; }
        else                                                 in_path = argv[i];
    }

    if (!name || !version || !out_path || !in_path) { usage(argv[0]); return 2; }

    /* Validate against the format's own limits HERE, so the failure names the
     * field rather than surfacing later as a parse error on a written file. */
    if (strlen(name) >= APKG_NAME_MAX) {
        fprintf(stderr, "mkapkg: name is longer than %d bytes\n", APKG_NAME_MAX - 1);
        return 1;
    }
    if (strlen(version) >= APKG_VERSION_MAX) {
        fprintf(stderr, "mkapkg: version is longer than %d bytes\n", APKG_VERSION_MAX - 1);
        return 1;
    }
    if (desc && strlen(desc) >= APKG_DESC_MAX) {
        fprintf(stderr, "mkapkg: description is longer than %d bytes\n", APKG_DESC_MAX - 1);
        return 1;
    }
    /* A newline in a field would inject a header line. */
    if (strchr(name, '\n') || strchr(version, '\n') || (desc && strchr(desc, '\n'))) {
        fprintf(stderr, "mkapkg: header fields may not contain a newline\n");
        return 1;
    }

    FILE *in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "mkapkg: cannot open %s\n", in_path); return 1; }

    if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return 1; }
    long sz = ftell(in);
    if (sz < 0) { fclose(in); return 1; }
    rewind(in);

    if ((unsigned long)sz > APKG_PAYLOAD_MAX) {
        fprintf(stderr, "mkapkg: %s is %ld bytes, over the %d-byte limit\n"
                        "        (the kernel will not load an image that big)\n",
                in_path, sz, APKG_PAYLOAD_MAX);
        fclose(in);
        return 1;
    }

    unsigned char *payload = malloc((size_t)sz ? (size_t)sz : 1);
    if (!payload) { fclose(in); return 1; }
    if (sz > 0 && fread(payload, 1, (size_t)sz, in) != (size_t)sz) {
        fprintf(stderr, "mkapkg: short read on %s\n", in_path);
        fclose(in); free(payload); return 1;
    }
    fclose(in);

    /* A sanity check, not a restriction: warn rather than refuse, because a
     * package could legitimately carry something that is not an ELF. */
    if (sz >= 4 && memcmp(payload, "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "mkapkg: warning: %s does not look like an ELF\n", in_path);
    }

    uint32_t crc = apkg_crc32(payload, (size_t)sz);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "mkapkg: cannot create %s\n", out_path);
        free(payload); return 1;
    }
    fprintf(out, "%s\n", APKG_MAGIC);
    fprintf(out, "name: %s\n", name);
    fprintf(out, "version: %s\n", version);
    if (desc) fprintf(out, "description: %s\n", desc);
    fprintf(out, "size: %ld\n", sz);
    fprintf(out, "crc32: 0x%08x\n", crc);
    fprintf(out, "\n");
    if (sz > 0 && fwrite(payload, 1, (size_t)sz, out) != (size_t)sz) {
        fprintf(stderr, "mkapkg: short write on %s\n", out_path);
        fclose(out); free(payload); return 1;
    }
    fclose(out);
    free(payload);

    /* Read it back with the OS's own parser.  A builder that can emit a file
     * its reader rejects will eventually do so; this makes that a build
     * failure instead of an install failure on someone else's machine. */
    FILE *back = fopen(out_path, "rb");
    if (!back) { fprintf(stderr, "mkapkg: cannot reopen %s\n", out_path); return 1; }
    if (fseek(back, 0, SEEK_END) != 0) { fclose(back); return 1; }
    long total = ftell(back);
    rewind(back);
    unsigned char *whole = malloc((size_t)total ? (size_t)total : 1);
    if (!whole) { fclose(back); return 1; }
    if (fread(whole, 1, (size_t)total, back) != (size_t)total) {
        fclose(back); free(whole); return 1;
    }
    fclose(back);

    struct apkg_header h;
    int r = apkg_parse(whole, (size_t)total, &h);
    if (r != APKG_OK) {
        fprintf(stderr, "mkapkg: wrote an unreadable package: %s\n", apkg_strerror(r));
        free(whole); remove(out_path); return 1;
    }
    r = apkg_verify(whole, (size_t)total, &h);
    if (r != APKG_OK) {
        fprintf(stderr, "mkapkg: wrote a package that fails its own checksum: %s\n",
                apkg_strerror(r));
        free(whole); remove(out_path); return 1;
    }
    free(whole);

    printf("[mkapkg] %s: %s %s, %ld bytes, crc32 0x%08x\n",
           out_path, name, version, sz, crc);
    return 0;
}
