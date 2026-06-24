/* selftest.c — focused userspace regression checks for syscall hardening. */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

static int fails = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("SELFTEST PASS: %s\n", name);
    } else {
        printf("SELFTEST FAIL: %s\n", name);
        fails++;
    }
}

int main(void) {
    /* Usercopy validation: bad user pointers must fail instead of killing the
     * thread or faulting the kernel. */
    ssize_t w = write(1, (const void *)0xFFFF800000000000ULL, 4);
    check("write rejects kernel pointer", w < 0);

    w = write(1, (const void *)0x0ULL, 1);
    check("write rejects null pointer", w < 0);

    int fd = open((const char *)0xFFFF800000000000ULL);
    check("open rejects kernel path pointer", fd < 0);

    struct stat st;
    int r = stat("/hello", (struct stat *)0xFFFF800000000000ULL);
    check("stat rejects kernel output pointer", r < 0);

    r = stat("/hello", &st);
    check("stat accepts valid output pointer", r == 0 && st.st_size > 0);

    fd = open("/hello");
    check("open valid file returns process fd", fd >= 3);
    if (fd >= 3) {
        unsigned char hdr[4] = {0};
        ssize_t n = read(fd, hdr, sizeof(hdr));
        check("read valid file into user buffer", n == 4 && hdr[0] == 0x7F && hdr[1] == 'E');
        close(fd);
    }

    char tmp_path[] = "/tmp/selftest.txt";
    fd = open(tmp_path);
    check("open/create tmp file", fd >= 3);
    if (fd >= 3) {
        const char msg[] = "selftest-data";
        check("write tmp file", write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg));
        close(fd);
    }

    /* Socket-style API: creation and owner-local close should work even if the
     * network transport is not connected. */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    check("socket create AF_INET/SOCK_STREAM", s >= 0);
    if (s >= 0) {
        char buf[4];
        check("recv on unconnected socket fails", recv(s, buf, sizeof(buf)) < 0);
        check("closesocket succeeds", closesocket(s) == 0);
        check("closesocket twice fails", closesocket(s) < 0);
    }

    if (fails == 0) {
        puts("SELFTEST ALL PASS");
        return 0;
    }
    printf("SELFTEST %d FAILURES\n", fails);
    return 1;
}
