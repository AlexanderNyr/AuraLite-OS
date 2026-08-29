/* gen_ap_trampoline_inc.c -- embed the flat AP trampoline in a C header.
 *
 * Freestanding-C replacement for gen_ap_trampoline_inc.py.  The optional
 * output-file argument deliberately preserves the old stdout workflow while
 * also allowing the SH5d in-guest build to write a generated header without
 * shell redirection (AuraLite's current command interpreter has no redirects).
 *
 * Usage: gen_ap_trampoline_inc <ap_trampoline.bin> [output.inc]
 */

#include <stdint.h>
#include <stdio.h>

static int emit_header(FILE *out, FILE *in, const char *input, unsigned long size)
{
    unsigned char bytes[12];
    size_t got;

    if (size == 0) {
        fprintf(stderr, "gen_ap_trampoline_inc: empty input binary\n");
        return 2;
    }
    /* A SIPI vector identifies one physical page; the trampoline must never
     * overlap the AP hand-off data in the following page. */
    if (size > 4096) {
        fprintf(stderr,
                "gen_ap_trampoline_inc: trampoline is %lu bytes > one page\n",
                size);
        return 2;
    }

    fprintf(out, "/* Auto-generated from %s by\n", input);
    fputs(" * tools/gen_ap_trampoline_inc.c -- do not edit by hand. */\n", out);
    fprintf(out, "#define AP_TRAMPOLINE_SIZE %lu\n", size);
    fputs("static const unsigned char ap_trampoline_blob[AP_TRAMPOLINE_SIZE] = {\n",
          out);
    while ((got = fread(bytes, 1, sizeof(bytes), in)) != 0) {
        fputs("    ", out);
        for (size_t i = 0; i < got; i++) {
            if (i != 0) fputs(", ", out);
            fprintf(out, "0x%02x", (unsigned)bytes[i]);
        }
        fputs(",\n", out);
    }
    fputs("};\n", out);
    return 0;
}

int main(int argc, char **argv)
{
    FILE *in;
    FILE *out = stdout;
    long length;
    int rc;

    if (argc != 2 && argc != 3) {
        fprintf(stderr,
                "usage: gen_ap_trampoline_inc <ap_trampoline.bin> [output.inc]\n");
        return 2;
    }
    in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "gen_ap_trampoline_inc: cannot open '%s'\n", argv[1]);
        return 1;
    }
    if (fseek(in, 0, SEEK_END) != 0 || (length = ftell(in)) < 0 ||
        fseek(in, 0, SEEK_SET) != 0) {
        fprintf(stderr, "gen_ap_trampoline_inc: cannot size '%s'\n", argv[1]);
        fclose(in);
        return 1;
    }
    if (argc == 3) {
        out = fopen(argv[2], "wb");
        if (!out) {
            fprintf(stderr, "gen_ap_trampoline_inc: cannot create '%s'\n", argv[2]);
            fclose(in);
            return 1;
        }
    }

    rc = emit_header(out, in, argv[1], (unsigned long)length);
    if (out != stdout) fclose(out);
    fclose(in);
    return rc;
}
