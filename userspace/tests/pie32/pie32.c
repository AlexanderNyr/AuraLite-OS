/* pie32.c -- RESIDUE2 T8 (RES-18): the PIE receipt program.
 *
 * Built -fPIE and linked as a static PIE (ld.lld -pie + pie32.ld), so
 * the image is ET_DYN with .rel.dyn R_386_RELATIVE entries: the i386
 * tenant's elf32load seats it at 0x10000000 and applies them.  The
 * program prints the RUNTIME address of a static object -- a value
 * that only exists if relocation actually happened -- and checks its
 * own pointer table before printing (an unrelocated entry points at
 * the link-time base 0 and the receipt line says so loudly).
 *
 * Self-contained on purpose: no libc, raw SYS_WRITE via int 0x80,
 * exactly one printable line.
 */
static const char msg[] = "PIE32: running from a RELOCATED base";
static int counter = 7;

/* Pointer tables in writable/rel-ro data cannot be pc-relative: each
 * entry is an R_386_RELATIVE relocation the loader must apply. */
static int bounce(int x) { return x + counter; }

void *volatile hooks[3] = { (void *)msg, (void *)bounce, (void *)&counter };
static volatile int pick = 2;   /* runtime index: keeps the table real */

int main(void) {
    /* Print the runtime address of a static object: != link-time base
     * proves the loader relocated us.  The write syscall number is the
     * AuraLite x86 ABI (SYS_WRITE=1) via inline int 0x80. */
    unsigned long addr = (unsigned long)msg;
    char buf[80];
    int n = 0;
    const char *s;
    for (s = msg; *s; s++) buf[n++] = *s;
    /* hex of the address */
    const char *hex = "0123456789abcdef";
    buf[n++] = ' '; buf[n++] = '@'; buf[n++] = ' ';
    buf[n++] = '0'; buf[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        buf[n++] = hex[(addr >> shift) & 0xF];
    buf[n++] = '\n';
    /* Touch every hook: forces the pointer table (and its RELATIVE
     * relocations) to survive linking and proves they were applied --
     * an unrelocated entry points at the link-time base (page 0) and
     * this check fails loudly. */
    if (hooks[0] != (void *)msg || hooks[pick] != (void *)&counter ||
        hooks[pick - 1] == 0)
        buf[3] = '!';
    if (bounce(0) < 0) buf[0] = 'X';
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(1L), "b"(1L), "c"(buf), "d"((long)n)
                      : "memory");
    (void)ret;
    return 0;
}
