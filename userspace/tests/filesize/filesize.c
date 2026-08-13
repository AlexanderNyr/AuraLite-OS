/* filesize — read a file to the end and report the byte count.
 *
 * The regression gate for the AHCI DMA leak (DOOM_PLAN.md D4).  That bug
 * truncated EVERY file on an AHCI-backed filesystem to exactly 173824
 * bytes, and nothing in the test suite noticed, because nothing read a
 * file large enough: the boot path and the self-tests touch a few dozen
 * sectors and the leak only bites after a few hundred.
 *
 * So this reads to EOF and prints the total, and the caller compares it
 * with the size the file is known to have.  It deliberately does not use
 * stat(): the point is to check that the READ PATH delivers every byte,
 * which is exactly what a size from the directory entry would hide.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

static char buf[8192];

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: filesize <path>\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("FILESIZE-OPEN-FAIL %s\n", argv[1]);
        return 1;
    }
    long total = 0;
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        total += n;
    }
    close(fd);
    if (n < 0) {
        printf("FILESIZE-READ-ERROR %s after %ld bytes\n", argv[1], total);
        return 1;
    }
    printf("FILESIZE %s %ld\n", argv[1], total);
    return 0;
}
