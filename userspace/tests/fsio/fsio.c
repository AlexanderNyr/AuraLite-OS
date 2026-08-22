/* fsio.c — the R6 exit gate, one source for three ports: malloc a
 * buffer, write a file THROUGH stdio-lite onto the mounted ext2,
 * read it back, compare byte-for-byte.  Prints exactly one PASS or
 * FAIL line; exits 0/1 (the shell's `run` reports the code). */

#include AURA_LIBC

int main(void)
{
    char *buf = malloc(64);
    char *chk = malloc(64);
    if (!buf || !chk) {
        aura_puts("fsio: FAIL malloc\n");
        return 1;
    }
    for (int i = 0; i < 47; i++)
        buf[i] = (char)('A' + (i * 7) % 26);
    buf[47] = '\n';
    buf[48] = 0;

    /* The tenants mount ext2 at "/", i386 at "/ext2" -- one source,
     * two candidate paths, the mount table decides. */
    const char *path = "/R6IO.TXT";
    FILE *f = fopen(path, "w");
    if (!f) {
        path = "/ext2/R6IO.TXT";
        f = fopen(path, "w");
    }
    if (!f) {
        aura_puts("fsio: FAIL fopen(w)\n");
        return 1;
    }
    if (fwrite(buf, 1, 48, f) != 48) {
        aura_puts("fsio: FAIL fwrite\n");
        return 1;
    }
    fclose(f);

    f = fopen(path, "r");
    if (!f) {
        aura_puts("fsio: FAIL fopen(r)\n");
        return 1;
    }
    unsigned long got = fread(chk, 1, 48, f);
    fclose(f);
    if (got != 48) {
        aura_printf("fsio: FAIL fread got %d\n", (int)got);
        return 1;
    }
    for (int i = 0; i < 48; i++)
        if (buf[i] != chk[i]) {
            aura_printf("fsio: FAIL mismatch at %d\n", i);
            return 1;
        }
    free(buf);
    free(chk);
    aura_printf("fsio: PASS malloc+stdio round-trip (%d bytes)\n", 48);
    return 0;
}
